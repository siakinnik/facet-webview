// Client of the web.runtime@1 capability (provided by facet-webview) for
// Facet app modules: shows a web page in a browser window on the module's
// own Wayland display. Copy client/webview.h and client/webview.cpp into
// your module.
//
// The module's manifest requires both capabilities and the window scope:
//   "requires": [{ "name": "display.wayland@1", "install": "siakinnik/facet-wayland" },
//                { "name": "web.runtime@1", "install": "siakinnik/facet-webview" }],
//   "permissions": ["wayland.window"]
// The window appears on the module's display like any of its Wayland
// windows: show the surface the compositor lends (Plugin::on_surface_lent)
// with Screen::fullscreen(). The module itself needs no network permission:
// the browser module fetches the pages.
//
// Protocol (lines of JSON over <endpoint("web.runtime")>/web.sock):
//   -> {"t": "open", "id": "main", "url": "https://…", "fd": true}  + a connected
//      Wayland client socket of this module (SCM_RIGHTS)
//   -> {"t": "close", "id": "main"}
//   <- {"t": "state", "id": "main", "state": "installing" | "starting" | "running" |
//       "closed" | "error", "progress": 0.4, "error": "…"}
//   <- {"t": "connect", "id": "main", "req": 3}   another browser process (the GPU
//      check) needs the display: answer with a new connection to it
//   -> {"t": "wayland", "id": "main", "req": 3, "fd": true}  + the connected socket
#pragma once

#include <deque>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "facet/json.h"
#include "facet/plugin.h"

namespace facet::webview {

// A line-based JSON channel over a Unix stream socket that can carry file
// descriptors (one per message, flagged with "fd": true).
class Channel {
public:
    ~Channel() { close(); }
    bool connect(const std::string& path);
    void adopt(int fd);  // an accepted connection
    void close();
    int fd() const { return fd_; }
    bool valid() const { return fd_ >= 0; }
    bool send(const Json& msg, int pass_fd = -1);
    // Reads what is available: messages with the descriptor they carried (-1
    // if none). False when the peer went away.
    bool read(std::vector<std::pair<Json, int>>& out);

private:
    int fd_ = -1;
    std::string in_;
    std::deque<int> fds_;
};

class WebView {
public:
    explicit WebView(sdk::Plugin& plugin) : plugin_(plugin) {}
    ~WebView();

    // Keeps `url` open as view `id` (reopened if the browser restarts or is
    // updated). Returns false while the providers are not reachable yet;
    // calling tick() retries.
    bool open(const std::string& id, const std::string& url);
    void close(const std::string& id);
    // Call from Plugin::on_tick: connects, retries and reopens.
    void tick();

    // State of a view: "installing" (progress 0..1), "starting", "running",
    // "closed", "error" (with a message from the browser module).
    std::function<void(const std::string& id, const std::string& state, double progress, const std::string& error)>
        on_state;

private:
    bool ensure_channel();
    bool send_open(const std::string& id, const std::string& url);
    void on_readable();

    sdk::Plugin& plugin_;
    Channel channel_;
    std::map<std::string, std::string> wanted_;  // id -> url
    std::map<std::string, double> reopen_at_;
    double next_try_ = 0;
};

}  // namespace facet::webview
