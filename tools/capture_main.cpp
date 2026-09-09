// The C++ half of the capture test: does the capture, writes the PNG.
#include "lucid/screen_capture.h"

#include <cairo.h>
#include <cstdio>
#include <wayland-client.h>

extern "C" int lucid_capture_main(wl_display* display, wl_output* output, const char* path) {
    lucid::ScreenCapture capture(display);
    std::printf("  can capture a screen : %s\n", capture.can_capture_output() ? "yes" : "no");
    std::printf("  can capture a window : %s\n", capture.can_capture_toplevel() ? "yes" : "no");
    if (!capture.can_capture_toplevel()) {
        std::printf("    (expected until labwc creates the wlroots source it already has)\n");
    }
    if (!capture.can_capture_output()) return 2;

    const lucid::CapturedImage img = capture.capture_output(output);
    if (!img.ok()) {
        std::printf("  capture failed: %s\n", capture.last_error().c_str());
        return 3;
    }
    std::printf("  captured %dx%d\n", img.width, img.height);

    cairo_surface_t* s = cairo_image_surface_create_for_data(
        const_cast<unsigned char*>(img.argb.data()), CAIRO_FORMAT_ARGB32,
        img.width, img.height, img.stride);
    const cairo_status_t st = cairo_surface_write_to_png(s, path);
    cairo_surface_destroy(s);
    std::printf("  wrote %s (%s)\n", path, cairo_status_to_string(st));
    return st == CAIRO_STATUS_SUCCESS ? 0 : 4;
}
