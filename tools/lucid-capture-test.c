// Prove the capture works against a real compositor, before anything depends on it.
//
// Captures the first output and writes a PNG. That is the whole program: if
// this produces a picture of the screen, the session, format negotiation, shm
// buffer and frame protocol are all correct, and the only thing between here
// and capturing a single window is which source object gets created.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-client.h>

struct state { struct wl_output *output; };

static void handle_global(void *data, struct wl_registry *reg, uint32_t name,
                          const char *iface, uint32_t version) {
    struct state *s = data;
    if (strcmp(iface, "wl_output") == 0 && s->output == NULL) {
        s->output = wl_registry_bind(reg, name, &wl_output_interface, version < 4 ? version : 4);
    }
}
static void handle_remove(void *data, struct wl_registry *reg, uint32_t name) {
    (void)data; (void)reg; (void)name;
}

int lucid_capture_main(struct wl_display *display, struct wl_output *output, const char *path);

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "/tmp/lucid-capture.png";
    struct wl_display *display = wl_display_connect(NULL);
    if (display == NULL) { fprintf(stderr, "no wayland display\n"); return 1; }

    struct state s = {0};
    static const struct wl_registry_listener listener = {handle_global, handle_remove};
    struct wl_registry *reg = wl_display_get_registry(display);
    wl_registry_add_listener(reg, &listener, &s);
    wl_display_roundtrip(display);
    if (s.output == NULL) { fprintf(stderr, "no output\n"); return 1; }

    return lucid_capture_main(display, s.output, path);
}
