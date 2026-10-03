// Example app module: one web page (the address in the file "url" next to
// the executable) full screen on its own screen, through the web.runtime
// capability. A template for web apps such as a web messenger.
#include <unistd.h>

#include <fstream>
#include <string>

#include "facet/plugin.h"
#include "i18n/i18n.h"
#include "webview.h"

using facet::Json;
using facet::sdk::LentSurface;
using facet::sdk::Plugin;
using facet::sdk::Screen;

namespace {

std::string read_url() {
    char exe[4096] = {};
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    std::string dir = n > 0 ? std::string(exe, size_t(n)) : std::string();
    std::ifstream f(dir.substr(0, dir.rfind('/')) + "/url");
    std::string line;
    std::getline(f, line);
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    return line;
}

class WebApp {
public:
    explicit WebApp(Plugin& plugin) : plugin_(plugin), view_(plugin), url_(read_url()) {
        view_.on_state = [this](const std::string&, const std::string& state, double progress,
                                const std::string& error) {
            state_ = state;
            progress_ = progress;
            error_ = error;
            refresh();
        };
    }

    void start() {
        if (!url_.empty()) view_.open("main", url_);
        refresh();
    }

    void on_lent(const std::string& id, bool available) {
        if (available) surface_ = id;
        else if (surface_ == id) surface_.clear();
        refresh();
    }

    void tick() { view_.tick(); }

    void refresh() {
        plugin_.set_tile(state_ == "running" ? "" : state_ == "installing" ? tr("Preparing the browser…") : "");
        if (!plugin_.visible()) return;
        Screen ui(tr("Web app"));
        if (!surface_.empty() && state_ == "running") ui.fullscreen(surface_);  // the page itself
        ui.info(tr("Address"), url_.empty() ? "—" : url_);
        if (state_ == "installing")
            ui.level(tr("Installing Firefox"), progress_, std::to_string(int(progress_ * 100)) + "%");
        else
            ui.info(tr("State"), state_.empty() ? tr("connecting…") : state_);
        if (!error_.empty()) ui.info(tr("Error"), error_, "bad");
        plugin_.set_ui(ui);
    }

private:
    std::string tr(std::string_view k) const { return plugin_.tr(k); }

    Plugin& plugin_;
    facet::webview::WebView view_;
    std::string url_, surface_, state_, error_;
    double progress_ = 0;
};

}  // namespace

int main() {
    Plugin plugin("web-app", "0.1.0");  // keep in sync with manifest.json
    web_app::register_translations(plugin.catalog());
    WebApp app(plugin);
    plugin.on_hello = [&](const Json&) { app.start(); };
    plugin.on_surface_lent = [&](const std::string& id, const LentSurface&, bool available) {
        app.on_lent(id, available);
    };
    plugin.on_visible = [&](bool) { app.refresh(); };
    plugin.on_locale = [&](const std::string&) { app.refresh(); };
    plugin.on_tick = [&] { app.tick(); };
    return plugin.run(250);
}
