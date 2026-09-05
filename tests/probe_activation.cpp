// Does the wlr source actually see which window is focused?
//
// Prints the active window whenever it changes. Run under a compositor with
// something focused; the point is to confirm the state event is parsed, which
// no amount of reading the protocol XML establishes.
#include "lucid/toplevel_source.h"

#include <cstdio>
#include <glib.h>

int main() {
    GMainLoop* loop = g_main_loop_new(nullptr, FALSE);

    lucid::ToplevelSourceNeeds needs;
    needs.activation = true;
    auto source = lucid::make_toplevel_source([] {}, needs);

    std::printf("source: %s\n", lucid::toplevel_source_token(source->kind()));
    std::printf("reports activation: %s\n", source->reports_activation() ? "yes" : "no");

    source->set_activation_callback([&source] {
        const lucid::ToplevelInfo* t = source->active_toplevel();
        std::printf("ACTIVE: %s -- \"%s\"\n", t ? t->app_id.c_str() : "(none)",
                    t ? t->title.c_str() : "");
        std::fflush(stdout);
    });

    // Print the initial state too: a window focused before we connected fires
    // no change callback, and a panel that only reacted to changes would start
    // blank until you clicked something.
    g_timeout_add(1500, [](gpointer data) -> gboolean {
        auto* src = static_cast<lucid::ToplevelSource*>(data);
        const lucid::ToplevelInfo* t = src->active_toplevel();
        std::printf("INITIAL: %s -- \"%s\"\n", t ? t->app_id.c_str() : "(none)",
                    t ? t->title.c_str() : "");
        std::printf("windows seen: %zu\n", src->toplevels().size());
        std::fflush(stdout);
        return G_SOURCE_REMOVE;
    }, source.get());

    g_timeout_add_seconds(8, [](gpointer d) -> gboolean {
        g_main_loop_quit(static_cast<GMainLoop*>(d));
        return G_SOURCE_REMOVE;
    }, loop);

    g_main_loop_run(loop);
    return 0;
}
