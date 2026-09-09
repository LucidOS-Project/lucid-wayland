// Capturing pixels that belong to someone else.
//
// This exists for the genie: to animate a window shrinking into the dock, the
// dock needs that window's pixels, and a layer-shell client has none of its
// own business knowing them. ext-image-copy-capture is the protocol that grants
// it, and the compositor decides whether to allow it.
//
// TWO SOURCES, and the difference is the whole state of this work.
//
//   Output    an entire screen. labwc advertises this today, so everything
//             below can be built and tested against it now.
//   Toplevel  one window. wlroots implements
//             ext_foreign_toplevel_image_capture_source_manager_v1 already --
//             labwc simply never creates it, which is one call in its server
//             setup. Until that patch lands this path cannot be exercised.
//
// The machinery either side of the source is identical, which is why it is
// worth building against the source that works rather than waiting.
#ifndef LUCID_SCREEN_CAPTURE_H
#define LUCID_SCREEN_CAPTURE_H

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

struct wl_display;
struct wl_output;
struct ext_foreign_toplevel_handle_v1;

namespace lucid {

// A captured image, in memory, owned by the caller.
struct CapturedImage {
    int width = 0;
    int height = 0;
    int stride = 0;
    // Always 32-bit, premultiplied, in the order cairo wants -- so it can be
    // handed straight to cairo_image_surface_create_for_data without a pass to
    // reshuffle channels. Format conversion happens once, here, rather than in
    // every consumer.
    std::vector<std::uint8_t> argb;

    bool ok() const { return width > 0 && height > 0 && !argb.empty(); }
};

class ScreenCapture {
  public:
    // Nothing is captured until capture_output() or capture_toplevel() is
    // called; constructing this only binds the globals.
    explicit ScreenCapture(wl_display* display);
    ~ScreenCapture();

    ScreenCapture(const ScreenCapture&) = delete;
    ScreenCapture& operator=(const ScreenCapture&) = delete;

    // Whether the compositor offers each source. Both are checked separately,
    // because a compositor offering one and not the other is exactly the
    // situation this is written in.
    bool can_capture_output() const;
    bool can_capture_toplevel() const;

    // Blocking, and deliberately so: a capture happens at one identifiable
    // moment -- just before a window is minimised -- and an asynchronous
    // version would only push the sequencing into the caller. Returns an image
    // with ok() false on failure, never throws.
    CapturedImage capture_output(wl_output* output);
    CapturedImage capture_toplevel(ext_foreign_toplevel_handle_v1* toplevel);

    // Why the last capture failed, for a log. Empty after a success.
    const std::string& last_error() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Where a window sits on a screen.
//
// Neither foreign-toplevel protocol reports geometry -- wlr's set_rectangle is
// the client telling the compositor where its own icon is, which is the
// opposite direction -- so a window's position has to be found rather than
// asked for. Matching its captured pixels against a capture of the whole screen
// is the way, and doing it on heavily downscaled copies makes it cheap enough
// to run at the moment somebody clicks minimise.
struct FoundRect {
    int x = 0, y = 0, width = 0, height = 0;
    double confidence = 0.0;   // 0..1; below ~0.6 the answer is a guess
    bool found() const { return width > 0 && height > 0 && confidence > 0.6; }
};

FoundRect locate_window(const CapturedImage& screen, const CapturedImage& window);

}  // namespace lucid

#endif
