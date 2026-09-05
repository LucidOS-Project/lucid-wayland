# lucid-wayland

The Wayland client code shared by LucidOS shell components: one answer to
"which applications are running, and which window is focused".

**Status: `lucid-panel` consumes this. `lucid-dock` does not yet** -- it still
carries the copy this was extracted from, so at the moment there are two, which
is the thing this repository exists to prevent. Migrating the dock means moving
its build, its CMake build, and the fake-compositor tests that exercise the
`ext` path, so it is a change of its own rather than a footnote to this one. It
is the next thing.

It exists because the alternative was two copies. The dock's toplevel source is
not a thin wrapper -- it prefers one protocol, falls back to a second, falls
back again to `/proc`, reference-counts app_ids so two windows of one
application are one key, and deliberately does *not* fire its callback for a
second window or a retitle. Copying that into the panel would have meant two
implementations of the same protocol drifting apart, and protocol bugs are the
hardest kind to find: they look like the compositor's fault.

## Three sources, and they are not equal

| Source | Protocol | Updates | Reports |
|---|---|---|---|
| `ext` | `ext-foreign-toplevel-list-v1` | event-driven | windows |
| `wlr` | `zwlr-foreign-toplevel-management-v1` | event-driven | windows **+ focus** |
| `proc` | none | polled, 4 s | processes |

`ext` is the standard and is preferred -- except by a consumer that needs to
know which window is focused.

## Why the panel and the dock end up on different protocols

**`ext-foreign-toplevel-list-v1` cannot say which window is focused, and that is
by design rather than by omission.** It is a *list* protocol, split out so that
enumerating windows does not require the privilege to observe or manipulate
their state. It carries no state event at all.

So a consumer says what it needs:

```cpp
lucid::ToplevelSourceNeeds needs;
needs.activation = true;                       // I need to know what is focused
auto source = lucid::make_toplevel_source(on_changed, needs);
```

and asking for activation moves the preference to `wlr`. The dock does not ask,
so it keeps `ext`. On the same machine the two components can be on different
protocols, for reasons that are correct in both cases.

## Closing a window is the same story again

`ext-` has no close request either, and for the same reason: it is a *list*
protocol, split out so that a taskbar which only wants to **display** windows
does not have to be trusted to **act** on them. So a consumer that offers a
Close item has stopped being a display client and become a management one --
which is a change in what it is, not a feature flag:

```cpp
needs.management = true;      // I intend to close windows
```

`close_app(app_id)` closes every window of an application, because that is what
a dock icon means: one icon stands for one application, so its Close closes the
application the way the macOS dock's Quit does -- not the most recently focused
window, which would leave the icon lit and the user pressing it again.

It is a **request**. The application may put up an unsaved-changes dialog, or
ignore it. There is deliberately no forced kill: a dock that could destroy an
unsaved document from a context menu would be a worse dock, and the protocol is
right not to offer one. Nothing is marked closed locally either -- windows go
away when the compositor says they have, not when we asked.

`reports_activation()` says whether the answer is available. Always check it
rather than testing `active_toplevel()` for null: "nothing is focused" and
"this source cannot tell" are different answers.

`LUCID_DOCK_TOPLEVEL_SOURCE=ext|wlr|proc` still overrides everything, including
a stated need, because it exists to reproduce one source's behaviour and
silently ignoring it would make it useless for that.

## Focus is a separate signal

The key-set callback deliberately does not fire when focus moves, and the
original header says why: a second window of a running application does not move
the key set, and neither does a retitle, and *the dock must not rebuild for
either* or a browser would rebuild it on every page it navigated to.

Focus moves far more often than the set of running applications does. So it has
its own callback, and the dock does not subscribe:

```cpp
source->set_activation_callback([&] { /* panel updates its label */ });
```

**The initial state is readable without waiting for a change.** A window focused
before the panel connected fires no change callback, and a panel that only
reacted to changes would start blank until you clicked something. Verified under
labwc with a window already focused: `active_toplevel()` returns it immediately.

## Building

    make          # liblucidwayland.a

Needs `libwayland-dev`, `libwayland-bin` (for `wayland-scanner`) and
`libglib2.0-dev`. The protocol XML is vendored in `protocols/` -- `wlr-protocols`
is not packaged on Debian or Ubuntu at all, and pinning it means the generated
marshalling cannot change underneath a build.

Static, deliberately: it is small, it is versioned with its consumers as a
submodule, and a shared object would add an soname and an ABI to maintain for
two programs that are always built together.

## Licence

LGPL-2.1-or-later, matching `lucid-tokens`. It is a library, and a library's
value is being adopted.
