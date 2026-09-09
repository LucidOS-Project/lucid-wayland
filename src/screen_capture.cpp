#include "lucid/screen_capture.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <cstdio>
#include <ctime>

#include "ext-image-capture-source-v1-client-protocol.h"
#include "ext-image-copy-capture-v1-client-protocol.h"
#include "wlr-screencopy-unstable-v1-client-protocol.h"

namespace lucid {
namespace {

// A shm pool the compositor writes into. Anonymous and immediately unlinked:
// the file exists only as a shared mapping between us and the compositor, and
// leaving it in /dev/shm under a guessable name is how one process reads
// another's screen.
struct ShmBuffer {
    wl_buffer* buffer = nullptr;
    void* data = nullptr;
    std::size_t size = 0;
    int width = 0, height = 0, stride = 0;

    ~ShmBuffer() {
        if (data != nullptr && data != MAP_FAILED) munmap(data, size);
        if (buffer != nullptr) wl_buffer_destroy(buffer);
    }
};

// The four 32-bit orderings a wlroots compositor is likely to hand back. The
// two BGR ones need their red and blue swapped to become what cairo reads.
bool is_supported_shm_format(std::uint32_t f) {
    return f == WL_SHM_FORMAT_ARGB8888 || f == WL_SHM_FORMAT_XRGB8888 ||
           f == WL_SHM_FORMAT_ABGR8888 || f == WL_SHM_FORMAT_XBGR8888;
}
bool format_needs_swap(std::uint32_t f) {
    return f == WL_SHM_FORMAT_ABGR8888 || f == WL_SHM_FORMAT_XBGR8888;
}
bool format_is_opaque(std::uint32_t f) {
    return f == WL_SHM_FORMAT_XRGB8888 || f == WL_SHM_FORMAT_XBGR8888;
}

int anonymous_shm(std::size_t size) {
    char name[] = "/lucid-capture-XXXXXX";
    for (int attempt = 0; attempt < 16; ++attempt) {
        // Not mkstemp: shm_open wants a name, not a path, and the point is to
        // unlink it the moment both sides hold the descriptor.
        std::snprintf(name, sizeof name, "/lucid-capture-%d-%d",
                      static_cast<int>(getpid()), attempt);
        const int fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
        if (fd >= 0) {
            shm_unlink(name);
            if (ftruncate(fd, static_cast<off_t>(size)) == 0) return fd;
            close(fd);
            return -1;
        }
    }
    return -1;
}

// Allocate a buffer the compositor can write a frame into. Both capture
// protocols need exactly this and differ only in how they are told about it.
bool make_shm_buffer(wl_shm* shm, int width, int height, int stride,
                     std::uint32_t format, ShmBuffer& buf) {
    buf.width = width;
    buf.height = height;
    buf.stride = stride;
    buf.size = static_cast<std::size_t>(stride) * height;

    const int fd = anonymous_shm(buf.size);
    if (fd < 0) return false;
    buf.data = mmap(nullptr, buf.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (buf.data == MAP_FAILED) { close(fd); return false; }
    wl_shm_pool* pool = wl_shm_create_pool(shm, fd, static_cast<std::int32_t>(buf.size));
    buf.buffer = wl_shm_pool_create_buffer(pool, 0, width, height, stride, format);
    wl_shm_pool_destroy(pool);
    close(fd);
    return buf.buffer != nullptr;
}

}  // namespace

struct ScreenCapture::Impl {
    wl_display* display = nullptr;
    wl_registry* registry = nullptr;
    wl_shm* shm = nullptr;
    ext_image_copy_capture_manager_v1* copy_manager = nullptr;
    ext_output_image_capture_source_manager_v1* output_source_manager = nullptr;
    ext_foreign_toplevel_image_capture_source_manager_v1* toplevel_source_manager = nullptr;
    zwlr_screencopy_manager_v1* wlr_manager = nullptr;

    // Filled by the session, before any frame is asked for.
    int buffer_width = 0, buffer_height = 0;
    std::uint32_t shm_format = 0;
    bool have_format = false;
    bool session_done = false;
    bool session_failed = false;

    bool frame_ready = false;
    bool frame_failed = false;

    std::string error;

    static void registry_global(void* data, wl_registry* reg, std::uint32_t name,
                                const char* interface, std::uint32_t version) {
        auto* self = static_cast<Impl*>(data);
        const std::string iface = interface;
        if (iface == "wl_shm") {
            self->shm = static_cast<wl_shm*>(wl_registry_bind(reg, name, &wl_shm_interface, 1));
        } else if (iface == "ext_image_copy_capture_manager_v1") {
            self->copy_manager = static_cast<ext_image_copy_capture_manager_v1*>(
                wl_registry_bind(reg, name, &ext_image_copy_capture_manager_v1_interface,
                                 std::min(version, 1u)));
        } else if (iface == "ext_output_image_capture_source_manager_v1") {
            self->output_source_manager = static_cast<ext_output_image_capture_source_manager_v1*>(
                wl_registry_bind(reg, name, &ext_output_image_capture_source_manager_v1_interface,
                                 std::min(version, 1u)));
        } else if (iface == "ext_foreign_toplevel_image_capture_source_manager_v1") {
            self->toplevel_source_manager =
                static_cast<ext_foreign_toplevel_image_capture_source_manager_v1*>(
                    wl_registry_bind(
                        reg, name,
                        &ext_foreign_toplevel_image_capture_source_manager_v1_interface,
                        std::min(version, 1u)));
        } else if (iface == "zwlr_screencopy_manager_v1") {
            self->wlr_manager = static_cast<zwlr_screencopy_manager_v1*>(
                wl_registry_bind(reg, name, &zwlr_screencopy_manager_v1_interface,
                                 std::min(version, 3u)));
        }
    }
    static void registry_remove(void*, wl_registry*, std::uint32_t) {}

    // --- session -------------------------------------------------------------
    static void on_session_buffer_size(void* data, ext_image_copy_capture_session_v1*,
                                    std::uint32_t w, std::uint32_t h) {
        auto* self = static_cast<Impl*>(data);
        self->buffer_width = static_cast<int>(w);
        self->buffer_height = static_cast<int>(h);
    }
    static void on_session_shm_format(void* data, ext_image_copy_capture_session_v1*,
                                   std::uint32_t format) {
        auto* self = static_cast<Impl*>(data);
        // The first format offered that we can hand to cairo unchanged. Taking
        // any format and converting later means writing a converter for each
        // one; these two are what every compositor offers.
        if (!self->have_format && is_supported_shm_format(format)) {
            self->shm_format = format;
            self->have_format = true;
        }
    }
    static void on_session_dmabuf_device(void*, ext_image_copy_capture_session_v1*, wl_array*) {}
    static void on_session_dmabuf_format(void*, ext_image_copy_capture_session_v1*,
                                      std::uint32_t, wl_array*) {}
    static void on_session_done(void* data, ext_image_copy_capture_session_v1*) {
        static_cast<Impl*>(data)->session_done = true;
    }
    static void on_session_stopped(void* data, ext_image_copy_capture_session_v1*) {
        auto* self = static_cast<Impl*>(data);
        self->session_failed = true;
        self->session_done = true;
    }

    // --- frame ---------------------------------------------------------------
    static void on_frame_transform(void*, ext_image_copy_capture_frame_v1*, std::uint32_t) {}
    static void on_frame_damage(void*, ext_image_copy_capture_frame_v1*,
                             std::int32_t, std::int32_t, std::int32_t, std::int32_t) {}
    static void on_frame_presentation_time(void*, ext_image_copy_capture_frame_v1*,
                                        std::uint32_t, std::uint32_t, std::uint32_t) {}
    static void on_frame_ready(void* data, ext_image_copy_capture_frame_v1*) {
        static_cast<Impl*>(data)->frame_ready = true;
    }
    static void on_frame_failed(void* data, ext_image_copy_capture_frame_v1*, std::uint32_t) {
        auto* self = static_cast<Impl*>(data);
        self->frame_failed = true;
        self->frame_ready = true;   // stop waiting
    }

    CapturedImage capture_source(ext_image_capture_source_v1* source);

    // --- wlr-screencopy ------------------------------------------------------
    // A flatter protocol than ext-: the frame announces one buffer shape, the
    // client attaches one, and it is copied. There is no session to negotiate.
    int wlr_width = 0, wlr_height = 0, wlr_stride = 0;
    std::uint32_t wlr_format = 0;
    bool wlr_have_buffer = false, wlr_buffer_done = false;
    bool wlr_ready = false, wlr_failed = false;
    bool wlr_y_invert = false;

    // What was offered, whether or not it could be used, so a failure can name
    // the format instead of saying "no".
    std::vector<std::uint32_t> wlr_offered;

    static void on_wlr_buffer(void* data, zwlr_screencopy_frame_v1*, std::uint32_t format,
                              std::uint32_t w, std::uint32_t h, std::uint32_t stride) {
        auto* self = static_cast<Impl*>(data);
        self->wlr_offered.push_back(format);
        if (self->wlr_have_buffer) return;      // first usable offer wins
        // wlroots offers whatever its renderer prefers to read back, and on
        // GLES2 that is usually one of the BGR-ordered formats rather than the
        // ARGB one. Taking only ARGB meant this failed on every wlroots
        // compositor with "no format this can use", which was true and useless.
        if (!is_supported_shm_format(format)) return;
        self->wlr_format = format;
        self->wlr_width = static_cast<int>(w);
        self->wlr_height = static_cast<int>(h);
        self->wlr_stride = static_cast<int>(stride);
        self->wlr_have_buffer = true;
    }
    static void on_wlr_flags(void* data, zwlr_screencopy_frame_v1*, std::uint32_t flags) {
        static_cast<Impl*>(data)->wlr_y_invert =
            (flags & ZWLR_SCREENCOPY_FRAME_V1_FLAGS_Y_INVERT) != 0;
    }
    static void on_wlr_ready(void* data, zwlr_screencopy_frame_v1*, std::uint32_t, std::uint32_t,
                             std::uint32_t) {
        static_cast<Impl*>(data)->wlr_ready = true;
    }
    static void on_wlr_failed(void* data, zwlr_screencopy_frame_v1*) {
        auto* self = static_cast<Impl*>(data);
        self->wlr_failed = true;
        self->wlr_ready = true;
    }
    static void on_wlr_damage(void*, zwlr_screencopy_frame_v1*, std::uint32_t, std::uint32_t,
                              std::uint32_t, std::uint32_t) {}
    static void on_wlr_linux_dmabuf(void*, zwlr_screencopy_frame_v1*, std::uint32_t,
                                    std::uint32_t, std::uint32_t) {}
    static void on_wlr_buffer_done(void* data, zwlr_screencopy_frame_v1*) {
        static_cast<Impl*>(data)->wlr_buffer_done = true;
    }

    CapturedImage capture_output_wlr(wl_output* output);
};

CapturedImage ScreenCapture::Impl::capture_output_wlr(wl_output* output) {
    CapturedImage out;
    if (output == nullptr || wlr_manager == nullptr || shm == nullptr) {
        error = "the compositor does not offer screen capture";
        return out;
    }
    static const zwlr_screencopy_frame_v1_listener listener = {
        on_wlr_buffer, on_wlr_flags, on_wlr_ready, on_wlr_failed,
        on_wlr_damage, on_wlr_linux_dmabuf, on_wlr_buffer_done,
    };

    wlr_have_buffer = wlr_buffer_done = wlr_ready = wlr_failed = wlr_y_invert = false;
    wlr_width = wlr_height = wlr_stride = 0;
    wlr_offered.clear();

    zwlr_screencopy_frame_v1* frame =
        zwlr_screencopy_manager_v1_capture_output(wlr_manager, 0, output);
    zwlr_screencopy_frame_v1_add_listener(frame, &listener, this);

    // Version 3 ends the buffer offers with buffer_done; versions 1 and 2 send
    // only the one shm buffer event and nothing to close the list, so waiting
    // for buffer_done there would wait forever.
    const bool has_buffer_done =
        zwlr_screencopy_frame_v1_get_version(frame) >= ZWLR_SCREENCOPY_FRAME_V1_BUFFER_DONE_SINCE_VERSION;
    while (!(has_buffer_done ? wlr_buffer_done : wlr_have_buffer) && !wlr_failed) {
        if (wl_display_dispatch(display) < 0) {
            error = "the connection to the compositor failed while starting a capture";
            zwlr_screencopy_frame_v1_destroy(frame);
            return out;
        }
    }
    if (!wlr_have_buffer || wlr_width <= 0 || wlr_height <= 0) {
        error = "the compositor offered no format this can use (offered:";
        for (std::uint32_t f : wlr_offered) {
            char buf[16];
            std::snprintf(buf, sizeof buf, " 0x%08x", f);
            error += buf;
        }
        error += wlr_offered.empty() ? " nothing)" : ")";
        zwlr_screencopy_frame_v1_destroy(frame);
        return out;
    }

    ShmBuffer buf;
    if (!make_shm_buffer(shm, wlr_width, wlr_height, wlr_stride, wlr_format, buf)) {
        error = "could not allocate shared memory for the capture";
        zwlr_screencopy_frame_v1_destroy(frame);
        return out;
    }

    zwlr_screencopy_frame_v1_copy(frame, buf.buffer);
    while (!wlr_ready) {
        if (wl_display_dispatch(display) < 0) {
            error = "the connection to the compositor failed during a capture";
            zwlr_screencopy_frame_v1_destroy(frame);
            return out;
        }
    }

    if (!wlr_failed) {
        out.width = buf.width;
        out.height = buf.height;
        out.stride = buf.stride;
        out.argb.resize(buf.size);
        if (wlr_y_invert) {
            // Some backends render bottom-up. Unflipping here means no consumer
            // has to know, and a consumer that did not know would animate a
            // window upside down.
            for (int y = 0; y < buf.height; ++y) {
                std::memcpy(out.argb.data() + static_cast<std::size_t>(y) * buf.stride,
                            static_cast<std::uint8_t*>(buf.data) +
                                static_cast<std::size_t>(buf.height - 1 - y) * buf.stride,
                            buf.stride);
            }
        } else {
            std::memcpy(out.argb.data(), buf.data, buf.size);
        }
        if (format_needs_swap(wlr_format)) {
            for (std::size_t i = 0; i + 3 < out.argb.size(); i += 4)
                std::swap(out.argb[i], out.argb[i + 2]);
        }
        if (format_is_opaque(wlr_format)) {
            for (std::size_t i = 3; i < out.argb.size(); i += 4) out.argb[i] = 0xff;
        }
        error.clear();
    } else {
        error = "the compositor refused to capture the screen";
    }

    zwlr_screencopy_frame_v1_destroy(frame);
    return out;
}

CapturedImage ScreenCapture::Impl::capture_source(ext_image_capture_source_v1* source) {
    CapturedImage out;
    if (source == nullptr || copy_manager == nullptr || shm == nullptr) {
        error = "the compositor does not offer image capture";
        return out;
    }

    static const ext_image_copy_capture_session_v1_listener session_listener = {
        on_session_buffer_size, on_session_shm_format, on_session_dmabuf_device,
        on_session_dmabuf_format, on_session_done, on_session_stopped,
    };
    static const ext_image_copy_capture_frame_v1_listener frame_listener = {
        on_frame_transform, on_frame_damage, on_frame_presentation_time,
        on_frame_ready, on_frame_failed,
    };

    buffer_width = buffer_height = 0;
    have_format = session_done = session_failed = false;
    frame_ready = frame_failed = false;

    ext_image_copy_capture_session_v1* session =
        ext_image_copy_capture_manager_v1_create_session(copy_manager, source, 0);
    ext_image_copy_capture_session_v1_add_listener(session, &session_listener, this);

    // The session announces its size and formats and then sends done. Nothing
    // can be allocated before that, so this waits rather than guessing.
    while (!session_done) {
        if (wl_display_dispatch(display) < 0) {
            error = "the connection to the compositor failed while starting a capture";
            ext_image_copy_capture_session_v1_destroy(session);
            return out;
        }
    }
    if (session_failed || buffer_width <= 0 || buffer_height <= 0 || !have_format) {
        error = session_failed ? "the compositor stopped the capture session"
                               : "the compositor offered no format this can use";
        ext_image_copy_capture_session_v1_destroy(session);
        return out;
    }

    ShmBuffer buf;
    buf.width = buffer_width;
    buf.height = buffer_height;
    buf.stride = buffer_width * 4;
    buf.size = static_cast<std::size_t>(buf.stride) * buffer_height;

    const int fd = anonymous_shm(buf.size);
    if (fd < 0) {
        error = "could not allocate shared memory for the capture";
        ext_image_copy_capture_session_v1_destroy(session);
        return out;
    }
    buf.data = mmap(nullptr, buf.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (buf.data == MAP_FAILED) {
        close(fd);
        error = "could not map shared memory for the capture";
        ext_image_copy_capture_session_v1_destroy(session);
        return out;
    }
    wl_shm_pool* pool = wl_shm_create_pool(shm, fd, static_cast<std::int32_t>(buf.size));
    buf.buffer = wl_shm_pool_create_buffer(pool, 0, buf.width, buf.height, buf.stride,
                                           shm_format);
    wl_shm_pool_destroy(pool);
    close(fd);

    ext_image_copy_capture_frame_v1* frame =
        ext_image_copy_capture_session_v1_create_frame(session);
    ext_image_copy_capture_frame_v1_add_listener(frame, &frame_listener, this);
    ext_image_copy_capture_frame_v1_attach_buffer(frame, buf.buffer);
    ext_image_copy_capture_frame_v1_capture(frame);

    while (!frame_ready) {
        if (wl_display_dispatch(display) < 0) {
            error = "the connection to the compositor failed during a capture";
            ext_image_copy_capture_frame_v1_destroy(frame);
            ext_image_copy_capture_session_v1_destroy(session);
            return out;
        }
    }

    if (!frame_failed) {
        out.width = buf.width;
        out.height = buf.height;
        out.stride = buf.stride;
        out.argb.resize(buf.size);
        std::memcpy(out.argb.data(), buf.data, buf.size);
        // XRGB has no alpha channel, and cairo's ARGB32 reads one. Filling it
        // opaque here means a consumer never has to know which format the
        // compositor chose.
        if (format_needs_swap(shm_format)) {
            for (std::size_t i = 0; i + 3 < out.argb.size(); i += 4)
                std::swap(out.argb[i], out.argb[i + 2]);
        }
        if (format_is_opaque(shm_format)) {
            for (std::size_t i = 3; i < out.argb.size(); i += 4) out.argb[i] = 0xff;
        }
        error.clear();
    } else {
        error = "the compositor refused to capture this source";
    }

    ext_image_copy_capture_frame_v1_destroy(frame);
    ext_image_copy_capture_session_v1_destroy(session);
    return out;
}

ScreenCapture::ScreenCapture(wl_display* display) : impl_(std::make_unique<Impl>()) {
    impl_->display = display;
    static const wl_registry_listener listener = {Impl::registry_global, Impl::registry_remove};
    impl_->registry = wl_display_get_registry(display);
    wl_registry_add_listener(impl_->registry, &listener, impl_.get());
    // Two round trips: the first delivers the globals, the second any events
    // they send on binding.
    wl_display_roundtrip(display);
    wl_display_roundtrip(display);
}

ScreenCapture::~ScreenCapture() {
    if (impl_->registry != nullptr) wl_registry_destroy(impl_->registry);
}

bool ScreenCapture::can_capture_output() const {
    if (impl_->shm == nullptr) return false;
    return (impl_->copy_manager != nullptr && impl_->output_source_manager != nullptr) ||
           impl_->wlr_manager != nullptr;
}

const char* ScreenCapture::output_capture_protocol() const {
    if (impl_->shm == nullptr) return "none";
    if (impl_->copy_manager != nullptr && impl_->output_source_manager != nullptr)
        return "ext-image-copy-capture";
    if (impl_->wlr_manager != nullptr) return "wlr-screencopy";
    return "none";
}

bool ScreenCapture::can_capture_toplevel() const {
    return impl_->copy_manager != nullptr && impl_->toplevel_source_manager != nullptr &&
           impl_->shm != nullptr;
}

const std::string& ScreenCapture::last_error() const { return impl_->error; }

CapturedImage ScreenCapture::capture_output(wl_output* output) {
    if (!can_capture_output()) {
        impl_->error = "this compositor cannot capture a screen";
        return {};
    }
    // ext- is the standard and is preferred where it exists; wlr-screencopy is
    // what the compositors in the world today actually have.
    if (impl_->copy_manager == nullptr || impl_->output_source_manager == nullptr) {
        return impl_->capture_output_wlr(output);
    }
    ext_image_capture_source_v1* source =
        ext_output_image_capture_source_manager_v1_create_source(impl_->output_source_manager,
                                                                 output);
    CapturedImage img = impl_->capture_source(source);
    ext_image_capture_source_v1_destroy(source);
    return img;
}

CapturedImage ScreenCapture::capture_toplevel(ext_foreign_toplevel_handle_v1* toplevel) {
    if (!can_capture_toplevel()) {
        // The message names the cause, because this is the one that will be
        // hit: wlroots implements this source and labwc does not create it.
        impl_->error =
            "this compositor cannot capture a single window "
            "(ext_foreign_toplevel_image_capture_source_manager_v1 is not offered)";
        return {};
    }
    ext_image_capture_source_v1* source =
        ext_foreign_toplevel_image_capture_source_manager_v1_create_source(
            impl_->toplevel_source_manager, toplevel);
    CapturedImage img = impl_->capture_source(source);
    ext_image_capture_source_v1_destroy(source);
    return img;
}

// Find a window inside a screenshot.
//
// Neither foreign-toplevel protocol reports where a window is, so its position
// has to be recovered from pixels. This is a template match, done on heavily
// downscaled greyscale copies: a 1920x1080 screen against a 1200x800 window
// becomes 240x135 against 150x100, which is about 25,000 positions to score
// instead of 400,000,000. That is the difference between a technique and a
// stall at the exact moment somebody clicked something.
//
// Windows are rectangular and opaque, so the match is nearly exact where it is
// right and obviously wrong everywhere else. Confidence reports that gap, and a
// caller that gets a low one should decline to animate rather than animate from
// the wrong place.
namespace {

struct Small {
    int w = 0, h = 0;
    std::vector<std::uint8_t> v;
};

Small shrink(const CapturedImage& img, int target_w) {
    Small out;
    if (!img.ok()) return out;
    const int step = std::max(1, img.width / std::max(1, target_w));
    out.w = img.width / step;
    out.h = img.height / step;
    out.v.resize(static_cast<std::size_t>(out.w) * out.h);
    for (int y = 0; y < out.h; ++y) {
        for (int x = 0; x < out.w; ++x) {
            const std::uint8_t* px = &img.argb[static_cast<std::size_t>(y * step) * img.stride +
                                               static_cast<std::size_t>(x * step) * 4];
            // BGRA in memory on little-endian, and the exact weights do not
            // matter for matching -- only that both sides use the same ones.
            out.v[static_cast<std::size_t>(y) * out.w + x] =
                static_cast<std::uint8_t>((px[0] + px[1] * 2 + px[2]) / 4);
        }
    }
    return out;
}

}  // namespace

FoundRect locate_window(const CapturedImage& screen, const CapturedImage& window) {
    FoundRect best;
    if (!screen.ok() || !window.ok()) return best;
    if (window.width > screen.width || window.height > screen.height) return best;

    const Small s = shrink(screen, 240);
    if (s.w == 0) return best;
    const int step = std::max(1, screen.width / 240);
    const Small w = shrink(window, std::max(8, window.width / step));
    if (w.w == 0 || w.h == 0 || w.w > s.w || w.h > s.h) return best;

    // Scores are negative sums of absolute differences, so this has to start
    // below every possible one. It used to start at -1, which meant only a
    // pixel-exact match was ever recorded and every real one -- where
    // downscaling alone shifts a few values -- was discarded, leaving the
    // caller an empty rectangle and no clue why.
    long best_score = std::numeric_limits<long>::min();
    int best_x = 0, best_y = 0;
    for (int oy = 0; oy + w.h <= s.h; ++oy) {
        for (int ox = 0; ox + w.w <= s.w; ++ox) {
            long score = 0;
            // Sampled rather than exhaustive: every third row and column is
            // enough to separate a window from the desktop behind it, and costs
            // a ninth as much.
            for (int y = 0; y < w.h; y += 3) {
                const std::uint8_t* srow = &s.v[static_cast<std::size_t>(oy + y) * s.w + ox];
                const std::uint8_t* wrow = &w.v[static_cast<std::size_t>(y) * w.w];
                for (int x = 0; x < w.w; x += 3) {
                    score += std::abs(static_cast<int>(srow[x]) - static_cast<int>(wrow[x]));
                }
            }
            score = -score;   // smaller difference is a better match
            if (score > best_score) {
                best_score = score;
                best_x = ox; best_y = oy;
            }
        }
    }
    if (best_score == std::numeric_limits<long>::min()) return best;

    // The runner-up, found in a second pass and only from positions well away
    // from the winner.
    //
    // Taking it during the first pass was wrong and made this useless: the
    // second best score is always the position one cell from the best, which
    // scores almost identically, so the gap between them was near zero and
    // every match came back at confidence 0.00 however obviously right it was.
    // What the confidence is meant to express is "does this window appear
    // somewhere else too", and that question is about distant positions.
    long rival = std::numeric_limits<long>::min();
    const int keep_out_x = std::max(2, w.w / 3);
    const int keep_out_y = std::max(2, w.h / 3);
    for (int oy = 0; oy + w.h <= s.h; ++oy) {
        for (int ox = 0; ox + w.w <= s.w; ++ox) {
            if (std::abs(ox - best_x) < keep_out_x && std::abs(oy - best_y) < keep_out_y) {
                continue;
            }
            long score = 0;
            for (int y = 0; y < w.h; y += 3) {
                const std::uint8_t* srow = &s.v[static_cast<std::size_t>(oy + y) * s.w + ox];
                const std::uint8_t* wrow = &w.v[static_cast<std::size_t>(y) * w.w];
                for (int x = 0; x < w.w; x += 3) {
                    score += std::abs(static_cast<int>(srow[x]) - static_cast<int>(wrow[x]));
                }
            }
            if (-score > rival) rival = -score;
        }
    }
    if (rival == std::numeric_limits<long>::min()) rival = best_score;   // nowhere else to stand

    // Refine to the pixel.
    //
    // The coarse search works on copies downscaled by `step`, so its answer is
    // quantised to that -- 8px on a 1920-wide screen. That is fine for "where
    // is it" and not fine for an animation that opens by drawing the window
    // over itself: eight pixels out and the window appears to jump before it
    // starts moving, which reads as a wobble at the start of every minimise.
    // A short full-resolution search around the coarse answer costs almost
    // nothing and removes it.
    int fine_x = best_x * step, fine_y = best_y * step;
    {
        long fine_best = std::numeric_limits<long>::min();
        for (int dy = -step; dy <= step; ++dy) {
            for (int dx = -step; dx <= step; ++dx) {
                const int ox = best_x * step + dx, oy = best_y * step + dy;
                if (ox < 0 || oy < 0 || ox + window.width > screen.width ||
                    oy + window.height > screen.height) {
                    continue;
                }
                long score = 0;
                for (int y = 0; y < window.height; y += 7) {
                    const std::uint8_t* srow =
                        &screen.argb[static_cast<std::size_t>(oy + y) * screen.stride + ox * 4];
                    const std::uint8_t* wrow =
                        &window.argb[static_cast<std::size_t>(y) * window.stride];
                    for (int x = 0; x < window.width; x += 7) {
                        score += std::abs(static_cast<int>(srow[x * 4]) - wrow[x * 4]);
                        score += std::abs(static_cast<int>(srow[x * 4 + 1]) - wrow[x * 4 + 1]);
                    }
                }
                if (-score > fine_best) { fine_best = -score; fine_x = ox; fine_y = oy; }
            }
        }
    }

    best.x = fine_x;
    best.y = fine_y;
    best.width = window.width;
    best.height = window.height;

    // How much better the winner is than the runner-up, normalised. A window
    // against a plain wallpaper scores near 1; a window against a screenshot of
    // itself scores near 0, and near 0 is exactly when the answer should not be
    // trusted.
    const double gap = static_cast<double>(best_score - rival);
    const double scale = static_cast<double>(std::max(1L, -rival));
    best.confidence = std::min(1.0, gap / scale * 2.0);
    return best;
}



FoundRect changed_rect(const CapturedImage& before, const CapturedImage& after,
                       const FoundRect* ignore) {
    FoundRect out;
    if (!before.ok() || !after.ok()) return out;
    if (before.width != after.width || before.height != after.height) return out;

    // Sampled on a grid rather than per pixel: a window is thousands of pixels
    // across and does not need every one of them counted to be found, and this
    // runs on the click.
    const int step = std::max(1, before.width / 480);
    const int cols = before.width / step, rows = before.height / step;
    if (cols < 4 || rows < 4) return out;

    std::vector<std::uint8_t> mask(static_cast<std::size_t>(cols) * rows, 0);
    auto differs = [&](int rx, int ry) {
        const std::size_t px = static_cast<std::size_t>(ry * step) * before.stride +
                               static_cast<std::size_t>(rx * step) * 4;
        const int d = std::abs(static_cast<int>(before.argb[px]) - after.argb[px]) +
                      std::abs(static_cast<int>(before.argb[px + 1]) - after.argb[px + 1]) +
                      std::abs(static_cast<int>(before.argb[px + 2]) - after.argb[px + 2]);
        return d > 24;
    };
    long total = 0;
    for (int ry = 0; ry < rows; ++ry) {
        for (int rx = 0; rx < cols; ++rx) {
            if (ignore != nullptr) {
                const int px = rx * step, py = ry * step;
                if (px >= ignore->x && px < ignore->x + ignore->width &&
                    py >= ignore->y && py < ignore->y + ignore->height) {
                    continue;
                }
            }
            if (differs(rx, ry)) { mask[static_cast<std::size_t>(ry) * cols + rx] = 1; ++total; }
        }
    }
    if (total < (static_cast<long>(cols) * rows) / 200) return out;   // nothing meaningful moved

    // Join the pieces of one window before labelling.
    //
    // A window is not uniformly different from what was behind it: a dark
    // terminal over a dark desktop matches almost everywhere except its
    // titlebar, its border and its text, which come out as a scatter of small
    // regions rather than one. Growing the mask by a couple of cells closes
    // those gaps so the pieces are recognised as the one window they are. It is
    // done on a copy, so the confidence below is still measured against the
    // pixels that really changed.
    constexpr int kGrow = 2;
    std::vector<std::uint8_t> grown = mask;
    for (int ry = 0; ry < rows; ++ry) {
        for (int rx = 0; rx < cols; ++rx) {
            if (mask[static_cast<std::size_t>(ry) * cols + rx] == 0) continue;
            for (int dy = -kGrow; dy <= kGrow; ++dy) {
                for (int dx = -kGrow; dx <= kGrow; ++dx) {
                    const int nx = rx + dx, ny = ry + dy;
                    if (nx >= 0 && nx < cols && ny >= 0 && ny < rows)
                        grown[static_cast<std::size_t>(ny) * cols + nx] = 1;
                }
            }
        }
    }

    // The biggest connected region, not the bounding box of everything that
    // changed.
    //
    // Taking the whole bounding box was tried and it does not survive contact
    // with a compositor: when a window goes away the focus moves, and the
    // window that receives it redraws its border somewhere else entirely. The
    // box then spans both and is right about nothing. A window that vanished
    // is one large solid blob; a border that changed colour is a thin one, and
    // a clock is a tiny one, so taking the largest by area picks the window and
    // ignores both.
    std::vector<int> label(grown.size(), 0);
    std::vector<int> stack;
    int best_area = 0;
    int bx0 = 0, bx1 = 0, by0 = 0, by1 = 0;
    int current = 0;
    for (int seed = 0; seed < static_cast<int>(grown.size()); ++seed) {
        if (grown[seed] == 0 || label[seed] != 0) continue;
        ++current;
        int area = 0, x0 = cols, x1 = -1, y0 = rows, y1 = -1;
        stack.clear();
        stack.push_back(seed);
        label[seed] = current;
        while (!stack.empty()) {
            const int at = stack.back();
            stack.pop_back();
            const int x = at % cols, y = at / cols;
            ++area;
            x0 = std::min(x0, x); x1 = std::max(x1, x);
            y0 = std::min(y0, y); y1 = std::max(y1, y);
            const int neighbours[4] = {x > 0 ? at - 1 : -1,
                                       x + 1 < cols ? at + 1 : -1,
                                       y > 0 ? at - cols : -1,
                                       y + 1 < rows ? at + cols : -1};
            for (int n : neighbours) {
                if (n >= 0 && grown[n] != 0 && label[n] == 0) {
                    label[n] = current;
                    stack.push_back(n);
                }
            }
        }
        if (area > best_area) { best_area = area; bx0 = x0; bx1 = x1; by0 = y0; by1 = y1; }
    }
    if (best_area == 0) return out;

    // Take the growth back off. Dilating to join a window's pieces also pads
    // its bounding box by the same amount in every direction, and a rectangle
    // reported two cells too large in each direction is a rectangle the
    // animation starts from in the wrong place.
    if (bx1 - bx0 > 2 * kGrow) { bx0 += kGrow; bx1 -= kGrow; }
    if (by1 - by0 > 2 * kGrow) { by0 += kGrow; by1 -= kGrow; }

    out.x = bx0 * step;
    out.y = by0 * step;
    out.width = std::min(before.width - out.x, (bx1 - bx0 + 1) * step);
    out.height = std::min(before.height - out.y, (by1 - by0 + 1) * step);

    // How solid the blob is inside its own box. A window leaves a filled
    // rectangle and scores near 1; an L-shaped smear of unrelated changes fills
    // its box poorly and is refused.
    long real_hits = 0;
    for (int ry = by0; ry <= by1; ++ry)
        for (int rx = bx0; rx <= bx1; ++rx)
            if (mask[static_cast<std::size_t>(ry) * cols + rx] != 0) ++real_hits;
    const long area_box = static_cast<long>(bx1 - bx0 + 1) * (by1 - by0 + 1);
    out.confidence = area_box > 0 ? static_cast<double>(real_hits) / static_cast<double>(area_box) : 0.0;
    return out;
}

}  // namespace lucid
