#include "lucid/toplevel_source.h"

#include <glib.h>
#include <glib-unix.h>
#include <wayland-client.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "ext-foreign-toplevel-list-v1-client-protocol.h"
#include "wlr-foreign-toplevel-management-unstable-v1-client-protocol.h"

namespace lucid {

const char* toplevel_source_token(ToplevelSourceKind kind) {
    switch (kind) {
        case ToplevelSourceKind::ExtForeignToplevel: return "ext";
        case ToplevelSourceKind::WlrForeignToplevel: return "wlr";
        case ToplevelSourceKind::Proc: return "proc";
    }
    return "proc";
}

const char* toplevel_source_description(ToplevelSourceKind kind) {
    switch (kind) {
        case ToplevelSourceKind::ExtForeignToplevel:
            return "ext-foreign-toplevel-list-v1 (event-driven, windows)";
        case ToplevelSourceKind::WlrForeignToplevel:
            return "zwlr-foreign-toplevel-management-v1 (event-driven, windows)";
        case ToplevelSourceKind::Proc:
            return "/proc scan every 4 s (polled, processes)";
    }
    return "unknown";
}

std::string normalise_match_key(std::string key) {
    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    // Some compositors report the full desktop file id as the app_id. Strip the
    // suffix so it lands on the same key as everything else rather than being a
    // near-miss that silently never matches.
    constexpr const char* kSuffix = ".desktop";
    constexpr std::size_t kSuffixLen = 8;
    if (key.size() > kSuffixLen && key.compare(key.size() - kSuffixLen, kSuffixLen, kSuffix) == 0) {
        key.erase(key.size() - kSuffixLen);
    }
    return key;
}

// ---------------------------------------------------------------------------
// /proc
// ---------------------------------------------------------------------------

namespace {

class ProcToplevelSource final : public ToplevelSource {
  public:
    ToplevelSourceKind kind() const override { return ToplevelSourceKind::Proc; }
    bool is_event_driven() const override { return false; }
    bool reports_windows() const override { return false; }
    const std::unordered_set<std::string>& running_keys() const override { return keys_; }

    // A process is not a window and /proc cannot be made to say otherwise.
    const std::vector<ToplevelInfo>& toplevels() const override {
        static const std::vector<ToplevelInfo> kNone;
        return kNone;
    }

    void refresh() override {
        std::unordered_set<std::string> names;

        const std::filesystem::path proc_root("/proc");
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(proc_root, ec)) {
            if (ec) {
                break;
            }
            if (!entry.is_directory(ec)) {
                continue;
            }

            const std::string pid = entry.path().filename().string();
            if (pid.empty() || !std::all_of(pid.begin(), pid.end(), [](unsigned char ch) {
                    return std::isdigit(ch) != 0;
                })) {
                continue;
            }

            {
                std::ifstream comm(entry.path() / "comm");
                std::string name;
                if (std::getline(comm, name) && !name.empty()) {
                    names.insert(normalise_match_key(std::move(name)));
                }
            }

            {
                std::ifstream cmdline(entry.path() / "cmdline", std::ios::binary);
                std::string raw((std::istreambuf_iterator<char>(cmdline)),
                                std::istreambuf_iterator<char>());
                if (!raw.empty()) {
                    const std::size_t nul = raw.find('\0');
                    std::string first = raw.substr(0, nul);
                    // Electron rewrites its argv into a single space-separated
                    // blob, so "argv[0]" for those processes is a couple of
                    // kilobytes of command line. It can never match a binary
                    // name, but it does bloat the set and makes the running
                    // keys unreadable when dumped, so drop it on length.
                    constexpr std::size_t kMaxProcessNameLen = 64;
                    if (!first.empty() && first.size() <= kMaxProcessNameLen) {
                        names.insert(
                            normalise_match_key(std::filesystem::path(first).filename().string()));
                    }
                }
            }
        }

        keys_ = std::move(names);
    }

  private:
    std::unordered_set<std::string> keys_;
};

// ---------------------------------------------------------------------------
// The two Wayland sources
//
// They differ only in which global they bind and which listener structs they
// fill in: both deliver a per-toplevel handle that reports an app_id, commits
// its state with `done`, and disappears with `closed`. Everything that follows
// from that -- the connection, the main-loop integration, the reference counts
// that let two windows of one application share a key, and the coalescing of a
// burst of events into one callback -- is shared here.
// ---------------------------------------------------------------------------

class WaylandToplevelSource : public ToplevelSource {
  public:
    explicit WaylandToplevelSource(ChangedCallback on_changed)
        : on_changed_(std::move(on_changed)) {}

    // The derived destructor runs first and releases the manager proxy; this
    // one then takes down the main-loop source and the connection. Doing the
    // unbind from here instead would call a pure virtual on a half-destroyed
    // object -- the handle proxies themselves need no unwinding, because
    // wl_display_disconnect() frees every proxy on the connection.
    ~WaylandToplevelSource() override {
        // Order matters: drop the main-loop source before the connection, or a
        // queued dispatch can run against a freed display.
        if (fd_source_id_ != 0) {
            g_source_remove(fd_source_id_);
            fd_source_id_ = 0;
        }
        if (registry_ != nullptr) {
            wl_registry_destroy(registry_);
            registry_ = nullptr;
        }
        if (display_ != nullptr) {
            wl_display_disconnect(display_);
            display_ = nullptr;
        }
    }

    bool is_event_driven() const override { return true; }

    void set_activation_callback(ActivationCallback cb) override {
        on_activation_ = std::move(cb);
    }

    const ToplevelInfo* active_toplevel() const override {
        for (const ToplevelInfo& t : toplevels_) {
            if (t.activated) {
                return &t;
            }
        }
        return nullptr;
    }
    bool reports_windows() const override { return true; }
    const std::unordered_set<std::string>& running_keys() const override { return keys_; }
    const std::vector<ToplevelInfo>& toplevels() const override { return toplevels_; }
    void refresh() override {}   // already current: that is the entire point

    // A separate connection to the compositor rather than GTK's own. The
    // protocol has nothing to do with our surfaces, so it needs nothing from
    // GDK's display -- and sharing GDK's connection would mean sharing its
    // event queue, which means getting the prepare_read/read_events handshake
    // right against a reader we do not control. One extra socket is cheaper
    // than that class of bug, and it keeps this file testable against a
    // compositor that does nothing but implement the protocol.
    bool connect() {
        display_ = wl_display_connect(nullptr);
        if (display_ == nullptr) {
            return false;   // not a Wayland session, or no compositor
        }

        registry_ = wl_display_get_registry(display_);
        static const wl_registry_listener kRegistryListener = {
            &WaylandToplevelSource::handle_global,
            &WaylandToplevelSource::handle_global_remove,
        };
        wl_registry_add_listener(registry_, &kRegistryListener, this);

        // First roundtrip delivers the globals, which is where the bind
        // happens; the second delivers the initial burst of toplevels the
        // compositor sends immediately after it.
        if (wl_display_roundtrip(display_) < 0) {
            return false;
        }
        if (!bound_) {
            return false;   // compositor does not implement this protocol
        }
        if (wl_display_roundtrip(display_) < 0) {
            return false;
        }

        dirty_ = false;   // the initial set is not a "change"
        fd_source_id_ = g_unix_fd_add(wl_display_get_fd(display_),
                                      static_cast<GIOCondition>(G_IO_IN | G_IO_ERR | G_IO_HUP),
                                      &WaylandToplevelSource::on_fd_ready, this);
        return true;
    }

  protected:
    // Per-toplevel state. The protocols are double-buffered: app_id is pending
    // until `done` commits it, so a compositor that changes an app_id cannot be
    // observed mid-change.
    struct HandleState {
        WaylandToplevelSource* owner = nullptr;
        // Everything the protocols double-buffer: pending until `done` commits
        // it, so a toplevel is never observed halfway through a change.
        std::string pending_app_id;
        std::string pending_title;
        std::string committed_app_id;
        std::string committed_title;
        // Sent once and never changed, so it needs no pending half.
        std::string identifier;
        // Double-buffered with the rest: `state` is pending until `done`.
        bool pending_activated = false;
        bool committed_activated = false;
        bool committed = false;
        // Set only by the wlr source, which is the only one with anything to
        // send a request to. ext- handles have no requests worth keeping.
        zwlr_foreign_toplevel_handle_v1* wlr_handle = nullptr;
    };

    virtual const wl_interface* manager_interface() const = 0;
    virtual uint32_t manager_version() const = 0;
    virtual void bind_manager(wl_registry* registry, uint32_t name, uint32_t version) = 0;

    void mark_bound() { bound_ = true; }

    // Fired from handle_done() when the focused window changed. Separate from
    // the key-set callback on purpose: focus moves far more often than the set
    // of running applications does, and the dock must not hear about it.
    void note_activation_committed() {
        const std::string now = active_key();
        if (now != last_active_key_) {
            last_active_key_ = now;
            if (on_activation_) {
                on_activation_();
            }
        }
    }

    std::string active_key() const {
        for (const ToplevelInfo& t : toplevels_) {
            if (t.activated) {
                // Identifier where there is one, so two windows of one
                // application are distinguishable; app_id plus title otherwise.
                return t.identifier.empty() ? t.app_id + "\x1f" + t.title : t.identifier;
            }
        }
        return {};
    }

    ActivationCallback on_activation_;
    std::string last_active_key_;

  public:
    // Reached from each backend's per-handle listeners, which are static and so
    // hold only a base pointer. Public rather than protected for that reason;
    // the whole class is file-local.
    void handle_app_id(HandleState* state, const char* app_id) {
        state->pending_app_id = app_id != nullptr ? normalise_match_key(app_id) : std::string();
    }

    void handle_title(HandleState* state, const char* title) {
        state->pending_title = title != nullptr ? title : "";
    }

    void handle_identifier(HandleState* state, const char* identifier) {
        state->identifier = identifier != nullptr ? identifier : "";
    }

    void handle_activated(HandleState* state, bool activated) {
        state->pending_activated = activated;
    }

    void handle_done(HandleState* state) {
        const bool app_id_changed =
            !state->committed || state->committed_app_id != state->pending_app_id;
        const bool title_changed =
            !state->committed || state->committed_title != state->pending_title;
        const bool activation_changed =
            !state->committed || state->committed_activated != state->pending_activated;
        if (!app_id_changed && !title_changed && !activation_changed) {
            return;
        }

        // Only the app_id participates in the key set, so only it moves the
        // reference counts. A title change republishes the window list without
        // touching them -- and without setting dirty_, so the dock is not
        // rebuilt every time a browser navigates.
        if (app_id_changed) {
            if (state->committed) {
                release_key(state->committed_app_id);
            }
            state->committed_app_id = state->pending_app_id;
        }
        state->committed_title = state->pending_title;
        state->committed_activated = state->pending_activated;
        state->committed = true;
        if (app_id_changed) {
            acquire_key(state->committed_app_id);
        }
        republish_toplevels();
        // After republish, so active_toplevel() is already correct when a
        // subscriber is called back and can read it without a second event.
        if (activation_changed) {
            note_activation_committed();
        }
    }

    void handle_closed(HandleState* state) {
        if (state->committed) {
            release_key(state->committed_app_id);
            state->committed = false;
        }
    }

    // The compositor is done with us. Nothing further will arrive, so the
    // honest report is that we no longer know what is running -- an empty set
    // rather than a stale one frozen at whatever was open at the time.
    //
    // Only a change if there was something to clear: a compositor shutting down
    // with nothing open must not look like a change to the dock.
    void handle_finished() {
        if (!keys_.empty()) {
            dirty_ = true;
        }
        keys_.clear();
        counts_.clear();
        handles_.clear();
        toplevels_.clear();
    }

  protected:
    // A vector rather than a map, so the published list keeps the order the
    // compositor announced windows in -- oldest first, which is the order a
    // taskbar would lay buttons out in and the one thing a hash container
    // cannot promise. There are as many entries as there are open windows, so
    // the linear erase below is not worth avoiding.
    HandleState* new_handle_state() {
        handles_.push_back(std::make_unique<HandleState>());
        handles_.back()->owner = this;
        return handles_.back().get();
    }

    void drop_handle_state(HandleState* state) {
        const auto it = std::find_if(
            handles_.begin(), handles_.end(),
            [state](const std::unique_ptr<HandleState>& held) { return held.get() == state; });
        if (it != handles_.end()) {
            handles_.erase(it);
        }
        republish_toplevels();
    }

    // Rebuilt whole rather than patched. The list is one entry per open window,
    // so this is a handful of small strings, and a rebuild cannot drift out of
    // step with the handles the way incremental edits can.
    void republish_toplevels() {
        toplevels_.clear();
        toplevels_.reserve(handles_.size());
        for (const std::unique_ptr<HandleState>& held : handles_) {
            if (!held->committed) {
                continue;   // announced but not yet committed by `done`
            }
            toplevels_.push_back(ToplevelInfo{held->committed_app_id, held->committed_title,
                                              held->identifier, held->committed_activated});
        }
    }

  private:
    void acquire_key(const std::string& key) {
        if (key.empty()) {
            return;   // a toplevel with no app_id matches nothing
        }
        if (++counts_[key] == 1) {
            keys_.insert(key);
            dirty_ = true;
        }
    }

    void release_key(const std::string& key) {
        if (key.empty()) {
            return;
        }
        const auto it = counts_.find(key);
        if (it == counts_.end()) {
            return;
        }
        // Reference counted, so closing one of two Firefox windows does not
        // put the dot out while a window is still open.
        if (--it->second <= 0) {
            counts_.erase(it);
            keys_.erase(key);
            dirty_ = true;
        }
    }

    static void handle_global(void* data, wl_registry* registry, uint32_t name,
                              const char* interface, uint32_t version) {
        auto* self = static_cast<WaylandToplevelSource*>(data);
        if (self->bound_ || std::strcmp(interface, self->manager_interface()->name) != 0) {
            return;
        }
        self->bind_manager(registry, name, std::min(version, self->manager_version()));
    }

    static void handle_global_remove(void*, wl_registry*, uint32_t) {}

    static gboolean on_fd_ready(gint, GIOCondition condition, gpointer data) {
        auto* self = static_cast<WaylandToplevelSource*>(data);

        if ((condition & (G_IO_ERR | G_IO_HUP)) != 0) {
            return self->fail();
        }

        // The documented non-blocking read handshake. wl_display_dispatch()
        // would be shorter and would block the UI thread on a spurious wakeup
        // with an empty queue, which is not a trade a dock gets to make.
        while (wl_display_prepare_read(self->display_) != 0) {
            if (wl_display_dispatch_pending(self->display_) < 0) {
                return self->fail();
            }
        }
        if (wl_display_flush(self->display_) < 0 && errno != EAGAIN) {
            wl_display_cancel_read(self->display_);
            return self->fail();
        }
        if (wl_display_read_events(self->display_) < 0) {
            return self->fail();
        }
        if (wl_display_dispatch_pending(self->display_) < 0) {
            return self->fail();
        }

        self->emit_if_dirty();
        return G_SOURCE_CONTINUE;
    }

    // One callback per batch. Opening a window delivers toplevel, app_id, title
    // and done together; rebuilding the dock once per event would restart every
    // animation four times for one window.
    void emit_if_dirty() {
        if (!dirty_) {
            return;
        }
        dirty_ = false;
        if (on_changed_) {
            on_changed_();
        }
    }

    gboolean fail() {
        g_warning("lucid-dock: %s connection lost; running indicators are now frozen",
                  toplevel_source_token(kind()));
        fd_source_id_ = 0;
        handle_finished();
        emit_if_dirty();
        return G_SOURCE_REMOVE;
    }

    ChangedCallback on_changed_;

  protected:
    // Protected rather than private because the wlr source sends requests to
    // the handles it opened, which is the one thing a subclass needs that the
    // shared bookkeeping does not do for it.
    std::vector<std::unique_ptr<HandleState>> handles_;

  private:
    std::vector<ToplevelInfo> toplevels_;
    std::unordered_map<std::string, int> counts_;
    std::unordered_set<std::string> keys_;
    bool dirty_ = false;
    bool bound_ = false;

  protected:
    wl_display* display_ = nullptr;
    wl_registry* registry_ = nullptr;

  private:
    guint fd_source_id_ = 0;
};

// --- ext-foreign-toplevel-list-v1 ------------------------------------------

class ExtToplevelSource final : public WaylandToplevelSource {
  public:
    using WaylandToplevelSource::WaylandToplevelSource;

    ~ExtToplevelSource() override {
        if (list_ != nullptr) {
            ext_foreign_toplevel_list_v1_destroy(list_);
            list_ = nullptr;
        }
    }

    ToplevelSourceKind kind() const override { return ToplevelSourceKind::ExtForeignToplevel; }

  protected:
    const wl_interface* manager_interface() const override {
        return &ext_foreign_toplevel_list_v1_interface;
    }
    uint32_t manager_version() const override { return 1; }

    void bind_manager(wl_registry* registry, uint32_t name, uint32_t version) override {
        list_ = static_cast<ext_foreign_toplevel_list_v1*>(
            wl_registry_bind(registry, name, &ext_foreign_toplevel_list_v1_interface, version));
        static const ext_foreign_toplevel_list_v1_listener kListener = {
            &ExtToplevelSource::on_toplevel,
            &ExtToplevelSource::on_finished,
        };
        ext_foreign_toplevel_list_v1_add_listener(list_, &kListener, this);
        mark_bound();
    }

  private:
    static void on_toplevel(void* data, ext_foreign_toplevel_list_v1*,
                            ext_foreign_toplevel_handle_v1* handle) {
        auto* self = static_cast<ExtToplevelSource*>(data);
        static const ext_foreign_toplevel_handle_v1_listener kListener = {
            &ExtToplevelSource::on_closed,
            &ExtToplevelSource::on_done,
            &ExtToplevelSource::on_title,
            &ExtToplevelSource::on_app_id,
            &ExtToplevelSource::on_identifier,
        };
        ext_foreign_toplevel_handle_v1_add_listener(handle, &kListener, self->new_handle_state());
    }

    static void on_finished(void* data, ext_foreign_toplevel_list_v1*) {
        static_cast<ExtToplevelSource*>(data)->handle_finished();
    }

    static void on_closed(void* data, ext_foreign_toplevel_handle_v1* handle) {
        auto* state = static_cast<HandleState*>(data);
        auto* self = static_cast<ExtToplevelSource*>(state->owner);
        self->handle_closed(state);
        ext_foreign_toplevel_handle_v1_destroy(handle);
        self->drop_handle_state(state);
    }

    static void on_done(void* data, ext_foreign_toplevel_handle_v1*) {
        auto* state = static_cast<HandleState*>(data);
        state->owner->handle_done(state);
    }

    static void on_title(void* data, ext_foreign_toplevel_handle_v1*, const char* title) {
        auto* state = static_cast<HandleState*>(data);
        state->owner->handle_title(state, title);
    }

    static void on_app_id(void* data, ext_foreign_toplevel_handle_v1*, const char* app_id) {
        auto* state = static_cast<HandleState*>(data);
        state->owner->handle_app_id(state, app_id);
    }

    static void on_identifier(void* data, ext_foreign_toplevel_handle_v1*,
                              const char* identifier) {
        auto* state = static_cast<HandleState*>(data);
        state->owner->handle_identifier(state, identifier);
    }

    ext_foreign_toplevel_list_v1* list_ = nullptr;
};

// --- zwlr-foreign-toplevel-management-v1 -----------------------------------

class WlrToplevelSource final : public WaylandToplevelSource {
  public:
    using WaylandToplevelSource::WaylandToplevelSource;

    // The only source that can. ext- is a list protocol with no state event,
    // and /proc has no windows to have state.
    bool reports_activation() const override { return true; }

    bool can_close() const override { return true; }

    // Every window of the application, because that is what a dock icon means.
    // One icon stands for one application, so its Close closes the application,
    // the way the macOS dock's Quit does -- not the most recently focused
    // window, which would leave the icon lit and the user pressing it again.
    void close_app(const std::string& app_id) override {
        if (app_id.empty()) {
            return;
        }
        for (const std::unique_ptr<HandleState>& held : handles_) {
            if (held->committed && held->committed_app_id == app_id &&
                held->wlr_handle != nullptr) {
                zwlr_foreign_toplevel_handle_v1_close(held->wlr_handle);
            }
        }
        // No local bookkeeping here on purpose. The windows go away when the
        // compositor says they have, via `closed`, not when we asked -- an
        // application showing an unsaved-changes dialog has not closed, and a
        // dock that dimmed its dot on the request would be lying about it.
        wl_display_flush(display_);
    }

    ~WlrToplevelSource() override {
        if (manager_ != nullptr) {
            // stop() asks the compositor to send `finished`; destroy() is what
            // actually releases the object, and is all that is wanted when the
            // whole process is going away.
            zwlr_foreign_toplevel_manager_v1_destroy(manager_);
            manager_ = nullptr;
        }
    }

    ToplevelSourceKind kind() const override { return ToplevelSourceKind::WlrForeignToplevel; }

  protected:
    const wl_interface* manager_interface() const override {
        return &zwlr_foreign_toplevel_manager_v1_interface;
    }
    uint32_t manager_version() const override { return 3; }

    void bind_manager(wl_registry* registry, uint32_t name, uint32_t version) override {
        manager_ = static_cast<zwlr_foreign_toplevel_manager_v1*>(
            wl_registry_bind(registry, name, &zwlr_foreign_toplevel_manager_v1_interface, version));
        static const zwlr_foreign_toplevel_manager_v1_listener kListener = {
            &WlrToplevelSource::on_toplevel,
            &WlrToplevelSource::on_finished,
        };
        zwlr_foreign_toplevel_manager_v1_add_listener(manager_, &kListener, this);
        mark_bound();
    }

  private:
    static void on_toplevel(void* data, zwlr_foreign_toplevel_manager_v1*,
                            zwlr_foreign_toplevel_handle_v1* handle) {
        auto* self = static_cast<WlrToplevelSource*>(data);
        static const zwlr_foreign_toplevel_handle_v1_listener kListener = {
            &WlrToplevelSource::on_title,
            &WlrToplevelSource::on_app_id,
            &WlrToplevelSource::on_output_enter,
            &WlrToplevelSource::on_output_leave,
            &WlrToplevelSource::on_state,
            &WlrToplevelSource::on_done,
            &WlrToplevelSource::on_closed,
            &WlrToplevelSource::on_parent,
        };
        HandleState* state = self->new_handle_state();
        state->wlr_handle = handle;
        zwlr_foreign_toplevel_handle_v1_add_listener(handle, &kListener, state);
    }

    static void on_finished(void* data, zwlr_foreign_toplevel_manager_v1*) {
        static_cast<WlrToplevelSource*>(data)->handle_finished();
    }

    // No identifier in this protocol -- it is one of the things
    // ext-foreign-toplevel-list-v1 added -- so ToplevelInfo::identifier stays
    // empty here. A consumer that needs stable window identity across handles
    // cannot get it from wlr.
    static void on_title(void* data, zwlr_foreign_toplevel_handle_v1*, const char* title) {
        auto* state = static_cast<HandleState*>(data);
        state->owner->handle_title(state, title);
    }

    static void on_app_id(void* data, zwlr_foreign_toplevel_handle_v1*, const char* app_id) {
        auto* state = static_cast<HandleState*>(data);
        state->owner->handle_app_id(state, app_id);
    }

    static void on_output_enter(void*, zwlr_foreign_toplevel_handle_v1*, wl_output*) {}
    static void on_output_leave(void*, zwlr_foreign_toplevel_handle_v1*, wl_output*) {}
    // The state array is a flat list of uint32 enum values. wl_array_for_each
    // is a C macro that does void* arithmetic, which C++ will not accept, so
    // the array is walked directly.
    static void on_state(void* data, zwlr_foreign_toplevel_handle_v1*, wl_array* array) {
        auto* state = static_cast<HandleState*>(data);
        bool activated = false;
        if (array != nullptr && array->data != nullptr) {
            const auto* values = static_cast<const uint32_t*>(array->data);
            const size_t count = array->size / sizeof(uint32_t);
            for (size_t i = 0; i < count; ++i) {
                if (values[i] == ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_ACTIVATED) {
                    activated = true;
                    break;
                }
            }
        }
        state->owner->handle_activated(state, activated);
    }

    static void on_done(void* data, zwlr_foreign_toplevel_handle_v1*) {
        auto* state = static_cast<HandleState*>(data);
        state->owner->handle_done(state);
    }

    static void on_closed(void* data, zwlr_foreign_toplevel_handle_v1* handle) {
        auto* state = static_cast<HandleState*>(data);
        auto* self = static_cast<WlrToplevelSource*>(state->owner);
        self->handle_closed(state);
        zwlr_foreign_toplevel_handle_v1_destroy(handle);
        self->drop_handle_state(state);
    }

    static void on_parent(void*, zwlr_foreign_toplevel_handle_v1*,
                          zwlr_foreign_toplevel_handle_v1*) {}

    zwlr_foreign_toplevel_manager_v1* manager_ = nullptr;
};

}  // namespace

std::unique_ptr<ToplevelSource> make_toplevel_source(ToplevelSource::ChangedCallback on_changed,
                                                    ToplevelSourceNeeds needs) {
    // An explicit request names exactly one source and does not fall through to
    // a better one: the point of the variable is to reproduce a specific
    // source's behaviour, and silently getting a different one would make it
    // useless for that. It does still fall back to /proc if the named protocol
    // is not there, because the alternative is no dock.
    const char* forced = g_getenv("LUCID_DOCK_TOPLEVEL_SOURCE");
    const std::string want = forced != nullptr ? forced : "auto";

    if (want != "auto" && want != "ext" && want != "wlr" && want != "proc") {
        g_warning("lucid-dock: LUCID_DOCK_TOPLEVEL_SOURCE='%s' is not one of "
                  "auto, ext, wlr, proc -- using auto",
                  want.c_str());
    }

    const bool try_ext = (want == "auto" || want == "ext");
    const bool try_wlr = (want == "auto" || want == "wlr");

    // A consumer that needs to know which window is focused cannot use ext-,
    // which is a list protocol and carries no state event at all -- so for that
    // consumer wlr is not the fallback, it is the only option, and preferring
    // ext would hand back a source that is structurally unable to answer.
    //
    // An explicit LUCID_DOCK_TOPLEVEL_SOURCE still wins: it exists to reproduce
    // one source's behaviour, and quietly overriding it would make it useless
    // for that. Asking for ext while needing activation is a legitimate way to
    // see what a consumer does without it.
    if ((needs.activation || needs.management) && want == "auto" && try_wlr) {
        auto source = std::make_unique<WlrToplevelSource>(on_changed);
        if (source->connect()) {
            return source;
        }
    }

    if (try_ext) {
        auto source = std::make_unique<ExtToplevelSource>(on_changed);
        if (source->connect()) {
            return source;
        }
    }
    if (try_wlr) {
        auto source = std::make_unique<WlrToplevelSource>(on_changed);
        if (source->connect()) {
            return source;
        }
    }

    auto source = std::make_unique<ProcToplevelSource>();
    source->refresh();
    return source;
}

}  // namespace lucid
