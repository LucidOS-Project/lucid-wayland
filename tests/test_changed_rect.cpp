// changed_rect, against pictures made on purpose.
//
// The compositor probe proves it works on a real screen; this proves the cases
// a real screen will not reliably produce -- a focus border redrawing somewhere
// else, a clock ticking, nothing happening at all -- and it runs in
// milliseconds with no compositor.
#include "lucid/screen_capture.h"

#include <cstdio>
#include <cstdlib>

namespace {

int failures = 0;
void check(bool ok, const char* what) {
    std::printf("  %-64s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) ++failures;
}

lucid::CapturedImage canvas(int w, int h, std::uint8_t v = 40) {
    lucid::CapturedImage img;
    img.width = w; img.height = h; img.stride = w * 4;
    img.argb.assign(static_cast<std::size_t>(img.stride) * h, v);
    for (std::size_t i = 3; i < img.argb.size(); i += 4) img.argb[i] = 0xff;
    return img;
}

void fill(lucid::CapturedImage& img, int x, int y, int w, int h, std::uint8_t v) {
    for (int yy = y; yy < y + h && yy < img.height; ++yy)
        for (int xx = x; xx < x + w && xx < img.width; ++xx) {
            const std::size_t p = static_cast<std::size_t>(yy) * img.stride + xx * 4;
            img.argb[p] = img.argb[p + 1] = img.argb[p + 2] = v;
        }
}

// A window's pixels are not one flat colour, and a flat block would let a bug
// that keys on uniformity pass.
void texture(lucid::CapturedImage& img, int x, int y, int w, int h) {
    for (int yy = y; yy < y + h && yy < img.height; ++yy)
        for (int xx = x; xx < x + w && xx < img.width; ++xx) {
            const std::size_t p = static_cast<std::size_t>(yy) * img.stride + xx * 4;
            const std::uint8_t v = static_cast<std::uint8_t>(120 + ((xx * 7 + yy * 13) % 90));
            img.argb[p] = img.argb[p + 1] = img.argb[p + 2] = v;
        }
}

bool near(int a, int b, int slack) { return std::abs(a - b) <= slack; }

}  // namespace

int main() {
    const int W = 1920, H = 1080;
    const int slack = W / 480 + 1;   // the sampling grid's own resolution

    std::puts("a window that went away");
    {
        lucid::CapturedImage before = canvas(W, H);
        texture(before, 700, 320, 420, 300);
        lucid::CapturedImage after = canvas(W, H);
        const lucid::FoundRect r = lucid::changed_rect(before, after);
        std::printf("    %dx%d at (%d,%d) confidence %.2f\n", r.width, r.height, r.x, r.y, r.confidence);
        check(r.found(), "is found");
        check(near(r.x, 700, slack) && near(r.y, 320, slack), "at the right place");
        check(near(r.width, 420, slack) && near(r.height, 300, slack), "at the right size");
    }

    std::puts("and is still found when the focus border of another window redraws");
    {
        // The case the compositor probe actually hit: closing one window moved
        // focus, the survivor repainted its border, and a bounding box over
        // everything that changed spanned both and was right about nothing.
        lucid::CapturedImage before = canvas(W, H);
        texture(before, 700, 320, 420, 300);          // the window that goes
        fill(before, 40, 60, 500, 4, 90);             // the survivor's border, unfocused
        fill(before, 40, 60, 4, 400, 90);
        lucid::CapturedImage after = canvas(W, H);
        fill(after, 40, 60, 500, 4, 200);             // now focused, and brighter
        fill(after, 40, 60, 4, 400, 200);
        const lucid::FoundRect r = lucid::changed_rect(before, after);
        std::printf("    %dx%d at (%d,%d) confidence %.2f\n", r.width, r.height, r.x, r.y, r.confidence);
        check(r.found(), "is found");
        check(near(r.x, 700, slack) && near(r.width, 420, slack),
              "and the border is not mistaken for part of it");
    }

    std::puts("a clock ticking is not a window");
    {
        lucid::CapturedImage before = canvas(W, H);
        lucid::CapturedImage after = canvas(W, H);
        fill(after, 1800, 4, 60, 20, 220);
        const lucid::FoundRect r = lucid::changed_rect(before, after);
        std::printf("    %dx%d at (%d,%d) confidence %.2f\n", r.width, r.height, r.x, r.y, r.confidence);
        check(r.width < 200 && r.height < 100, "a small change stays small");
    }

    std::puts("nothing happened");
    {
        lucid::CapturedImage before = canvas(W, H);
        texture(before, 700, 320, 420, 300);
        lucid::CapturedImage after = before;
        const lucid::FoundRect r = lucid::changed_rect(before, after);
        check(!r.found(), "an unchanged screen reports no rectangle");
    }

    std::puts("mismatched or empty input");
    {
        check(!lucid::changed_rect(canvas(W, H), canvas(800, 600)).found(),
              "different sizes report nothing rather than reading past an edge");
        check(!lucid::changed_rect({}, {}).found(), "and so do empty images");
    }

    std::puts("finding a window that is still on screen");
    {
        // What the dock does to avoid waiting for a window to vanish before it
        // can draw it: match the pixels it kept against the screen as it is
        // now. It has to work when the window has moved, which is the case the
        // dock got wrong by assuming it had not.
        // Blocks rather than per-pixel noise: this is matched on heavily
        // downscaled copies, and a pattern that changes every pixel does not
        // survive being sampled every eighth one. Real windows are mostly
        // large flat regions, which is what this imitates.
        lucid::CapturedImage screen = canvas(W, H);
        for (int by = 0; by < 400; by += 50)
            for (int bx = 0; bx < 500; bx += 50)
                fill(screen, 300 + bx, 200 + by,
                     50, 50, static_cast<std::uint8_t>(90 + ((bx / 50 + by / 50) % 5) * 30));
        lucid::CapturedImage window = canvas(500, 400);
        for (int y = 0; y < 400; ++y)
            for (int x = 0; x < 500; ++x) {
                const std::size_t d = static_cast<std::size_t>(y) * window.stride + x * 4;
                const std::size_t src =
                    static_cast<std::size_t>(200 + y) * screen.stride + (300 + x) * 4;
                window.argb[d] = screen.argb[src];
                window.argb[d + 1] = screen.argb[src + 1];
                window.argb[d + 2] = screen.argb[src + 2];
            }
        const lucid::FoundRect r = lucid::locate_window(screen, window);
        std::printf("    found at (%d,%d) confidence %.2f, wanted (300,200)\n",
                    r.x, r.y, r.confidence);
        check(r.found(), "a window on a plain desktop is found");
        // To the pixel, not to the downscaling step. An animation that opens
        // by drawing the window over itself shows any error here as a jump.
        check(near(r.x, 300, 1) && near(r.y, 200, 1), "at the right place, to the pixel");
    }

    std::puts(failures == 0 ? "\nall checks passed" : "\nFAILURES");
    return failures == 0 ? 0 : 1;
}
