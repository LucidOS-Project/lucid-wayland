// Grab the screen repeatedly, as fast as the compositor will give it.
//
// For watching an animation that lasts 400ms on a machine with no way to
// record one: run it beside the thing being animated and look at the frames.
#include "lucid/screen_capture.h"

#include <wayland-client.h>
#include <unistd.h>

#include <cstdio>
#include <string>

namespace {
wl_output* g_output = nullptr;
void on_global(void*, wl_registry* reg, std::uint32_t name, const char* iface, std::uint32_t) {
    if (std::string(iface) == "wl_output" && g_output == nullptr)
        g_output = static_cast<wl_output*>(wl_registry_bind(reg, name, &wl_output_interface, 1));
}
void on_global_remove(void*, wl_registry*, std::uint32_t) {}
}  // namespace

int main(int argc, char** argv) {
    const std::string dir = argc > 1 ? argv[1] : ".";
    const int frames = argc > 2 ? std::atoi(argv[2]) : 30;
    const int delay_us = argc > 3 ? std::atoi(argv[3]) : 16000;

    wl_display* d = wl_display_connect(nullptr);
    if (d == nullptr) { std::puts("no compositor"); return 1; }
    static const wl_registry_listener rl = {on_global, on_global_remove};
    wl_registry_add_listener(wl_display_get_registry(d), &rl, nullptr);
    wl_display_roundtrip(d);
    if (g_output == nullptr) { std::puts("no output"); return 1; }

    lucid::ScreenCapture cap(d);
    if (!cap.can_capture_output()) { std::puts("no capture"); return 1; }

    for (int i = 0; i < frames; ++i) {
        const lucid::CapturedImage img = cap.capture_output(g_output);
        if (!img.ok()) { std::printf("frame %d failed: %s\n", i, cap.last_error().c_str()); break; }
        char path[512];
        std::snprintf(path, sizeof path, "%s/f%03d.ppm", dir.c_str(), i);
        std::FILE* f = std::fopen(path, "wb");
        if (f != nullptr) {
            std::fprintf(f, "P6\n%d %d\n255\n", img.width, img.height);
            for (int y = 0; y < img.height; ++y)
                for (int x = 0; x < img.width; ++x) {
                    const std::uint8_t* p = &img.argb[static_cast<std::size_t>(y) * img.stride + x * 4];
                    const unsigned char rgb[3] = {p[2], p[1], p[0]};
                    std::fwrite(rgb, 1, 3, f);
                }
            std::fclose(f);
        }
        usleep(delay_us);
    }
    std::printf("wrote %d frames to %s\n", frames, dir.c_str());
    return 0;
}
