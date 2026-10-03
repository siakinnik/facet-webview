// App module for one web site: the page (the address in the file "url" next
// to the executable) full screen on its own screen, through the web.runtime
// capability. The executable is the same for every site: id, name and
// version come from the manifest.json next to it, so an app for another site
// is a copy of this directory with its own manifest and url.
#include <unistd.h>

#include <fstream>
#include <iterator>
#include <string>
#include <utility>

#include "facet/plugin.h"
#include "i18n/i18n.h"
#include "webview.h"

using facet::Json;
using facet::sdk::LentSurface;
using facet::sdk::Plugin;
using facet::sdk::Screen;

namespace {

std::string exe_dir() {
    char exe[4096] = {};
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    std::string dir = n > 0 ? std::string(exe, size_t(n)) : std::string();
    return dir.substr(0, dir.rfind('/'));
}

Json read_manifest() {
    std::ifstream f(exe_dir() + "/manifest.json");
    std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    Json m;
    if (!Json::parse(text, m)) m = Json::object();
    return m;
}

// The manifest's name in `lang` ({"en": ..., "ru": ...} or a plain string).
std::string localized(const Json& name, const std::string& lang) {
    if (name.is_string()) return name.str();
    if (name[lang].is_string()) return name[lang].str();
    return name["en"].str();
}

std::string read_url() {
    std::ifstream f(exe_dir() + "/url");
    std::string line;
    std::getline(f, line);
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    return line;
}

class WebApp {
public:
    WebApp(Plugin& plugin, Json name) : plugin_(plugin), view_(plugin), url_(read_url()), name_(std::move(name)) {
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
        plugin_.set_tile(state_ == "running" ? "" : state_ == "installing" ? tr("Getting ready…") : "");
        if (!plugin_.visible()) return;
        std::string title = localized(name_, plugin_.catalog().language());
        Screen ui(title.empty() ? tr("Web app") : title);
        if (!surface_.empty() && state_ == "running") ui.fullscreen(surface_);  // the page itself
        ui.info(tr("Address"), url_.empty() ? "—" : url_);
        // Pages come from the module that provides web.runtime; how it shows
        // them (and what it installs for that) is its business.
        if (state_ == "installing")
            ui.level(tr("The web view module is getting ready"), progress_, std::to_string(int(progress_ * 100)) + "%");
        else if (state_.empty())
            ui.info(tr("State"), tr("waiting for the web view module…"));
        else if (state_ == "closed")
            ui.info(tr("State"), tr("opening again…"));
        else if (state_ != "running" && state_ != "error")
            ui.info(tr("State"), tr("opening…"));
        if (!error_.empty()) ui.info(tr("Error"), error_, "bad");
        plugin_.set_ui(ui);
    }

private:
    std::string tr(std::string_view k) const { return plugin_.tr(k); }

    Plugin& plugin_;
    facet::webview::WebView view_;
    std::string url_, surface_, state_, error_;
    Json name_;
    double progress_ = 0;
};

}  // namespace

int main() {
    Json manifest = read_manifest();
    Plugin plugin(manifest["id"].as_string("web-app"), manifest["version"].as_string("0.1.0"));
    web_app::register_translations(plugin.catalog());
    WebApp app(plugin, manifest["name"]);
    plugin.on_hello = [&](const Json&) { app.start(); };
    plugin.on_surface_lent = [&](const std::string& id, const LentSurface&, bool available) {
        app.on_lent(id, available);
    };
    plugin.on_visible = [&](bool) { app.refresh(); };
    plugin.on_locale = [&](const std::string&) { app.refresh(); };
    plugin.on_tick = [&] { app.tick(); };
    return plugin.run(250);
}
