#include "lucid/screen_capture.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <ctime>

#include "ext-image-capture-source-v1-client-protocol.h"
#include "ext-image-copy-capture-v1-client-protocol.h"

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

}  // namespace

struct ScreenCapture::Impl {
    wl_display* display = nullptr;
    wl_registry* registry = nullptr;
    wl_shm* shm = nullptr;
    ext_image_copy_capture_manager_v1* copy_manager = nullptr;
    ext_output_image_capture_source_manager_v1* output_source_manager = nullptr;
    ext_foreign_toplevel_image_capture_source_manager_v1* toplevel_source_manager = nullptr;

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
        if (!self->have_format &&
            (format == WL_SHM_FORMAT_ARGB8888 || format == WL_SHM_FORMAT_XRGB8888)) {
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
};

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
        if (shm_format == WL_SHM_FORMAT_XRGB8888) {
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
    return impl_->copy_manager != nullptr && impl_->output_source_manager != nullptr &&
           impl_->shm != nullptr;
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

    long best_score = -1, second_best = -1;
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
                second_best = best_score;
                best_score = score;
                best_x = ox; best_y = oy;
            } else if (score > second_best) {
                second_best = score;
            }
        }
    }
    if (best_score < 0 && second_best == -1) return best;

    best.x = best_x * step;
    best.y = best_y * step;
    best.width = window.width;
    best.height = window.height;

    // How much better the winner is than the runner-up, normalised. A window
    // against a plain wallpaper scores near 1; a window against a screenshot of
    // itself scores near 0, and near 0 is exactly when the answer should not be
    // trusted.
    const double gap = static_cast<double>(best_score - second_best);
    const double scale = static_cast<double>(std::max(1L, -second_best));
    best.confidence = std::min(1.0, gap / scale * 8.0);
    return best;
}

}  // namespace lucid
