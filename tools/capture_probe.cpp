// Does the capture path actually work here?
//
// Run inside a compositor. Reports which protocol it got, captures the screen,
// minimises a window, captures again, and prints where changed_rect() says the
// window was. That is the whole genie plumbing, exercised without a dock.
#include "lucid/screen_capture.h"
#include "lucid/toplevel_source.h"

#include <wayland-client.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

wl_output* g_output = nullptr;

void on_global(void*, wl_registry* reg, std::uint32_t name, const char* iface, std::uint32_t) {
    if (std::string(iface) == "wl_output" && g_output == nullptr) {
        g_output = static_cast<wl_output*>(wl_registry_bind(reg, name, &wl_output_interface, 1));
    }
}
void on_global_remove(void*, wl_registry*, std::uint32_t) {}

void write_ppm(const char* path, const lucid::CapturedImage& img) {
    std::FILE* f = std::fopen(path, "wb");
    if (f == nullptr) return;
    std::fprintf(f, "P6\n%d %d\n255\n", img.width, img.height);
    for (int y = 0; y < img.height; ++y) {
        for (int x = 0; x < img.width; ++x) {
            const std::uint8_t* p = &img.argb[static_cast<std::size_t>(y) * img.stride + x * 4];
            const unsigned char rgb[3] = {p[2], p[1], p[0]};   // BGRA in memory
            std::fwrite(rgb, 1, 3, f);
        }
    }
    std::fclose(f);
}

}  // namespace

int main(int argc, char** argv) {
    const std::string out_dir = argc > 1 ? argv[1] : ".";
    const std::string want = argc > 2 ? argv[2] : "";   // which app to act on

    wl_display* display = wl_display_connect(nullptr);
    if (display == nullptr) { std::puts("FAIL: no compositor"); return 1; }
    static const wl_registry_listener rl = {on_global, on_global_remove};
    wl_registry_add_listener(wl_display_get_registry(display), &rl, nullptr);
    wl_display_roundtrip(display);
    if (g_output == nullptr) { std::puts("FAIL: no wl_output"); return 1; }

    lucid::ScreenCapture capture(display);
    std::printf("capture protocol: %s\n", capture.output_capture_protocol());
    std::printf("can capture output: %s, toplevel: %s\n",
                capture.can_capture_output() ? "yes" : "no",
                capture.can_capture_toplevel() ? "yes" : "no");
    if (!capture.can_capture_output()) { std::puts("FAIL: no output capture"); return 1; }

    const lucid::CapturedImage before = capture.capture_output(g_output);
    if (!before.ok()) {
        std::printf("FAIL: capture: %s\n", capture.last_error().c_str());
        return 1;
    }
    std::printf("captured %dx%d stride %d\n", before.width, before.height, before.stride);
    write_ppm((out_dir + "/before.ppm").c_str(), before);

    // Is it a real picture, or a black rectangle that merely has the right
    // dimensions? A capture that silently returns zeros would pass every check
    // above.
    long lit = 0;
    for (std::size_t i = 0; i < before.argb.size(); i += 4)
        if (before.argb[i] > 8 || before.argb[i + 1] > 8 || before.argb[i + 2] > 8) ++lit;
    const double lit_share = static_cast<double>(lit) / (before.argb.size() / 4);
    std::printf("non-black pixels: %.1f%%\n", 100.0 * lit_share);
    if (lit_share < 0.01) { std::puts("FAIL: the capture is blank"); return 1; }

    auto source = lucid::make_toplevel_source([] {}, {true, true});
    std::printf("toplevel source: %s, windows %zu, can minimise: %s\n",
                lucid::toplevel_source_token(source->kind()),
                source->toplevels().size(),
                source->can_minimize() ? "yes" : "no");
    if (source->toplevels().empty()) { std::puts("no window open; stopping after capture"); return 0; }

    for (const auto& t : source->toplevels())
        std::printf("  window: app_id=%s title=%s\n", t.app_id.c_str(), t.title.c_str());

    auto settle = [&] {
        for (int i = 0; i < 10; ++i) { wl_display_roundtrip(display); usleep(20000); }
    };
    auto shot = [&](const char* name) {
        const lucid::CapturedImage img = capture.capture_output(g_output);
        if (img.ok()) write_ppm((out_dir + "/" + name + ".ppm").c_str(), img);
        return img;
    };

    std::size_t target = 0;
    if (!want.empty()) {
        for (std::size_t i = 0; i < source->toplevels().size(); ++i)
            if (source->toplevels()[i].app_id == want) target = i;
        std::printf("acting on window %zu (%s)\n", target, want.c_str());
    }

    if (source->can_minimize()) {
        source->minimize(target);
        wl_display_roundtrip(display);
        settle();
        const lucid::CapturedImage after = shot("after_minimize");
        if (!after.ok()) { std::printf("FAIL: capture: %s\n", capture.last_error().c_str()); return 1; }
        const lucid::FoundRect r = lucid::changed_rect(before, after);
        std::printf("after set_minimized: %dx%d at (%d,%d) confidence %.2f -> %s\n",
                    r.width, r.height, r.x, r.y, r.confidence, r.found() ? "FOUND" : "nothing moved");
        if (r.found()) { std::puts("PASS: the compositor minimised it and the rect was recovered"); return 0; }
        std::puts("NOTE: this compositor ignored set_minimized (sway has no minimised state");
        std::puts("      for Wayland toplevels). Falling back to a close, which removes a");
        std::puts("      window the same way and exercises the same geometry path.");
    }

    if (!source->can_close()) { std::puts("SKIP: cannot close either"); return 0; }
    source->close_app(source->toplevels()[target].app_id);
    wl_display_roundtrip(display);
    settle();
    const lucid::CapturedImage after = shot("after_close");
    if (!after.ok()) { std::printf("FAIL: capture: %s\n", capture.last_error().c_str()); return 1; }
    const lucid::FoundRect r = lucid::changed_rect(before, after);
    std::printf("after close: %dx%d at (%d,%d) confidence %.2f -> %s\n",
                r.width, r.height, r.x, r.y, r.confidence, r.found() ? "FOUND" : "not trusted");
    return r.found() ? 0 : 2;
}
