# facet-webview: web pages for apps

A [Facet](https://github.com/siakinnik/facet-core) module ("Web pages for
apps") that gives other modules web pages: it provides the `web.runtime@1`
capability, using Firefox ESR. An app module
asks for an address, and the page appears full screen on that app's own
screen, in Firefox ESR in kiosk mode (no tabs, no address bar). Touches and
Facet's keyboard work in the page like in any Wayland app.

Firefox is not part of this module: the first time an app needs it (or when
you press "Install Firefox" on the module's page under Settings > Modules),
the module downloads the current Firefox ESR from Mozilla
(archive.mozilla.org, about 80 MB, 270 MB on disk), checks it against
Mozilla's SHA-512 list and keeps it up to date. Firefox updates itself never;
telemetry, studies and first-run pages are switched off by enterprise policy.

## Install

Needs Facet 0.6+ and the [Wayland module](https://github.com/siakinnik/facet-wayland)
(x86_64):

```bash
curl -fsSL https://raw.githubusercontent.com/siakinnik/facet-core/main/scripts/get.sh | sudo bash -s -- --plugin siakinnik/facet-webview
```

Apps that require `web.runtime@1` offer to install it themselves.

## How it works

- Every app module that requires `web.runtime@1` gets a socket from this
  module in its endpoint directory (`web.sock`).
- To open a page, the app connects to its own Wayland display and passes that
  connection over the socket (the browser never sees other apps' displays).
- This module starts Firefox with that connection (`WAYLAND_SOCKET`), so the
  window lands on the app's screen with the app's Wayland permissions, while
  Firefox itself runs in this module's container with this module's network
  permission. The app needs no network permission of its own.
- Every page of every app has its own Firefox profile: logins and cookies are
  never shared between apps. A page that closes (crash, update) is reopened
  by the client library.

## Using it from an app module

Copy `client/webview.h` and `client/webview.cpp` into your module.

```json
"permissions": ["wayland.window"],
"requires": [
  { "name": "display.wayland@1", "install": "siakinnik/facet-wayland" },
  { "name": "web.runtime@1", "install": "siakinnik/facet-webview" }
]
```

```cpp
facet::webview::WebView view(plugin);
view.on_state = [](const std::string& id, const std::string& state, double progress, const std::string& error) {
    // "installing" (progress 0..1), "running", "closed", "error"
};
plugin.on_hello = [&](const Json&) { view.open("main", "https://example.org/"); };
plugin.on_tick = [&] { view.tick(); };
// The page is the surface the compositor lends: show it with Screen::fullscreen(id)
plugin.on_surface_lent = [&](const std::string& id, const LentSurface&, bool available) { /* ... */ };
```

`examples/web-app` is a complete module that shows one address (the `url`
file next to its executable).

Protocol, JSON lines over `<endpoint("web.runtime")>/web.sock`:

```
-> {"t": "open", "id": "main", "url": "https://…", "fd": true}   + a connected Wayland socket (SCM_RIGHTS)
-> {"t": "close", "id": "main"}
<- {"t": "state", "id": "main", "state": "installing" | "running" | "closed" | "error", "progress": 0.4, "error": "…"}
```

Only `http://` and `https://` addresses are opened; view ids are
`[a-z0-9_-]{1,32}`.

## Build

```bash
cmake -S . -B build -G Ninja && cmake --build build && ctest --test-dir build
```

Needs OpenSSL and liblzma (static libraries for `-DFIREFOX_STATIC=ON`), and
Docker or Ubuntu 22.04 for the runtime (`scripts/build-runtime.sh`: GTK 3 and
its libraries from Ubuntu 22.04, DejaVu fonts, with their licenses). The SDK
comes from `../facet-core` when it is there, else from GitHub.

To try it without a device: run Facet headless with this module, the Wayland
module and the example (`FACET_PLUGIN_PATH=…/wayland.plugin:build/webview.plugin:build/examples/web-app.plugin`).

## Graphics card

With the optional `gpu` permission and Facet's OpenGL drivers installed
(Settings > Graphics, Facet 0.7+), Firefox draws pages with the graphics card
through Mesa from that package, and the Wayland module passes the finished
frames to Facet without copies. Firefox's other processes (its GPU check)
reach the app's display through connections the app makes for each of them,
relayed by this module. Without the permission, pages are drawn in software.

## Licenses

- This module: GPL-3.0 (LICENSE). Its sources are this repository at the
  release tag.
- Firefox is made by Mozilla and is not part of this module or its releases:
  the module downloads it unchanged from mozilla.org on the device (MPL 2.0;
  using it means accepting [Mozilla's terms](https://www.mozilla.org/about/legal/terms/firefox/)).
  Only the enterprise policy file it reads (`distribution/policies.json`) is
  added. "Firefox" is a trademark of the Mozilla Foundation; this module is
  not a Mozilla product and is not endorsed by Mozilla.
- The runtime (`runtime/`): libraries and fonts from Ubuntu 22.04, unchanged,
  each with its license in `runtime/licenses/`, listed with exact versions in
  `runtime/licenses/SOURCES`.
- The executable is static: glibc, the GCC runtime, OpenSSL and liblzma are
  built into it; their licenses are in `licenses/`, the packages they come
  from in `licenses/STATIC`.
- Every release has `facet-webview-<version>-sources.tar` with the source
  packages of the runtime and of glibc, OpenSSL and liblzma (`scripts/ci/sources.sh`).
  GCC's runtime is under the GCC Runtime Library Exception, which needs no
  sources.
