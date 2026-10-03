// firefox: provides web.runtime@1 for Facet app modules. An app module asks
// for a page (client/webview.h) and passes a connected socket of its own
// Wayland display; this module runs Firefox ESR in kiosk mode with that
// socket, so the window appears on the app's screen with the app's Wayland
// scopes, while the browser runs in this module's container. Firefox is
// downloaded from Mozilla on the device. No tile: a page under Settings >
// Modules shows the browser and the pages that are open.
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "browser.h"
#include "facet/plugin.h"
#include "i18n/i18n.h"
#include "installer.h"
#include "package.h"
#include "webview.h"

using facet::Json;
using facet::sdk::Consumer;
using facet::sdk::Plugin;
using facet::sdk::Screen;
using facet::webview::Channel;
using namespace ffmod;

namespace {

#ifndef FIREFOX_MODULE_VERSION
#define FIREFOX_MODULE_VERSION "dev"  // set by CMake
#endif

constexpr const char* kCapability = "web.runtime";
constexpr double kCheckEvery = 12 * 3600;

double now_s() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

std::string exe_dir() {
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    std::string p = n > 0 ? std::string(buf, size_t(n)) : std::string(".");
    return p.substr(0, p.rfind('/'));
}

int listen_unix(const std::string& path) {
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof addr.sun_path) return -1;
    std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
    ::unlink(path.c_str());
    int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) return -1;
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0 || ::listen(fd, 8) != 0) {
        ::close(fd);
        return -1;
    }
    ::chmod(path.c_str(), 0666);  // the app runs as its own user; only it sees this directory
    return fd;
}

class FirefoxModule {
public:
    explicit FirefoxModule(Plugin& plugin) : plugin_(plugin), installer_(plugin.data_dir()) {}

    void hello() {
        installer_.start(false, lang());  // look for updates
        last_check_ = now_s();
        refresh();
    }

    void shutdown() {
        installer_.cancel();
        for (auto& [key, v] : views_) stop(v);
    }

    void consumer(const Consumer& c) {
        if (c.capability != kCapability) return;
        if (!c.running) {
            drop(c.module);
            refresh();
            return;
        }
        Client& cl = clients_[c.module];
        if (cl.listen_fd >= 0) return;
        if (c.dir.empty()) {
            Plugin::log("firefox: %s has no endpoint directory", c.module.c_str());
            return;
        }
        cl.listen_fd = listen_unix(c.dir + "/web.sock");
        if (cl.listen_fd < 0) {
            Plugin::log("firefox: cannot listen for %s: %s", c.module.c_str(), std::strerror(errno));
            return;
        }
        std::string module = c.module;
        plugin_.watch_fd(cl.listen_fd, [this, module] { accept(module); });
    }

    void tick() {
        double t = now_s();
        reap();
        // Connections the app never answered (it may be paused).
        for (auto it = pending_.begin(); it != pending_.end();) {
            if (t - it->second.since < 10) {
                ++it;
                continue;
            }
            ::close(it->second.fd);
            it = pending_.erase(it);
        }
        poll_installer();
        if (!installer_.busy() && t - last_check_ > kCheckEvery) {
            installer_.start(false, lang());
            last_check_ = t;
        }
        if (installer_.busy() && t - last_progress_ > 0.5) {
            last_progress_ = t;
            for (auto& [key, v] : views_)
                if (v.pid < 0 && v.wayland_fd >= 0) send_state(v, "installing");
        }
        refresh();
    }

    void on_event(const std::string& id) {
        if (id == "install" || id == "update") {
            if (id == "update") {
                // Running pages restart in the new version: the apps reopen them.
                for (auto& [key, v] : views_) {
                    stop(v);
                    send_state(v, "closed");
                }
                views_.clear();
            }
            installer_.start(true, lang());
            error_.clear();
        } else if (id == "cancel") {
            installer_.cancel();
        } else if (id == "remove") {
            for (auto& [key, v] : views_) {
                stop(v);
                send_state(v, "closed");
            }
            views_.clear();
            installer_.remove_app();
        }
        refresh();
    }

    void refresh() {
        if (plugin_.visible()) plugin_.set_ui(build());
    }

private:
    struct Client {
        int listen_fd = -1;
        std::vector<std::unique_ptr<Channel>> conns;
    };
    struct View {
        std::string module, id, url, state;
        pid_t pid = -1;
        int wayland_fd = -1;
        int relay_listen = -1;  // <profile>/run/wayland-0: more connections to the app's display
    };
    // A Firefox process connected by name, waiting for the app's connection.
    struct Pending {
        std::string key;
        int fd = -1;
        double since = 0;
    };
    // Firefox side and app display side of a relayed connection.
    struct Relay {
        std::string key;
        int a = -1, b = -1;
    };

    std::string tr(std::string_view k) const { return plugin_.tr(k); }
    std::string tr(std::string_view k, const std::vector<std::string>& a) const { return plugin_.tr(k, a); }
    std::string lang() const { return firefox_lang(plugin_.catalog().language()); }

    void accept(const std::string& module) {
        Client& cl = clients_[module];
        int fd = ::accept4(cl.listen_fd, nullptr, nullptr, SOCK_CLOEXEC);
        if (fd < 0) return;
        if (cl.conns.size() >= 4) {  // one per app is the normal case
            ::close(fd);
            return;
        }
        auto ch = std::make_unique<Channel>();
        ch->adopt(fd);
        Channel* raw = ch.get();
        cl.conns.push_back(std::move(ch));
        plugin_.watch_fd(fd, [this, module, raw] { read(module, raw); });
    }

    void read(const std::string& module, Channel* ch) {
        std::vector<std::pair<Json, int>> msgs;
        int fd = ch->fd();
        bool alive = ch->read(msgs);
        for (auto& [msg, f] : msgs) handle(module, msg, f);
        if (!alive) {
            plugin_.unwatch_fd(fd);
            auto& conns = clients_[module].conns;
            for (auto it = conns.begin(); it != conns.end(); ++it)
                if (it->get() == ch) {
                    conns.erase(it);
                    break;
                }
        }
    }

    void handle(const std::string& module, const Json& msg, int fd) {
        const std::string& t = msg["t"].str();
        const std::string& id = msg["id"].str();
        if (!acceptable_id(id)) {
            if (fd >= 0) ::close(fd);
            return;
        }
        std::string key = module + "/" + id;
        if (t == "open") {
            const std::string& url = msg["url"].str();
            if (fd < 0 || !acceptable_url(url)) {
                if (fd >= 0) ::close(fd);
                View bad{module, id, url, {}};
                send_state(bad, "error", tr("Not a web address: {}", {url}));
                return;
            }
            if (views_.count(key)) stop(views_[key]);
            View& v = views_[key];
            v = View{module, id, url, {}};
            v.wayland_fd = fd;
            if (installer_.installed_version().empty()) {
                if (!installer_.busy()) installer_.start(true, lang());
                send_state(v, "installing");
            } else {
                launch(v);
            }
        } else if (t == "wayland") {
            // The app's answer to a connection request: relay it.
            auto p = pending_.find(msg["req"].as_int(-1));
            if (fd < 0 || p == pending_.end() || p->second.key != key) {
                if (fd >= 0) ::close(fd);
                return;
            }
            start_relay(key, p->second.fd, fd);
            pending_.erase(p);
            return;
        } else if (t == "close") {
            if (fd >= 0) ::close(fd);
            auto it = views_.find(key);
            if (it != views_.end()) {
                stop(it->second);
                send_state(it->second, "closed");
                views_.erase(it);
            }
        } else if (fd >= 0) {
            ::close(fd);
        }
        refresh();
    }

    void launch(View& v) {
        LaunchSpec s;
        s.app = installer_.app_dir();
        s.runtime = exe_dir() + "/runtime";
        s.profile = plugin_.data_dir() + "/profiles/" + v.module + "/" + v.id;
        s.url = v.url;
        s.lang = lang();
        s.wayland_fd = v.wayland_fd;
        s.run_dir = s.profile + "/run";
        s.gl = plugin_.gl_dir();
        for (size_t i = 1; i <= s.run_dir.size(); ++i)  // data/profiles/<module>/<id>/run
            if (i == s.run_dir.size() || s.run_dir[i] == '/') ::mkdir(s.run_dir.substr(0, i).c_str(), 0700);
        // Firefox's helper processes connect by name: relayed to the app's display.
        v.relay_listen = listen_unix(s.run_dir + "/wayland-0");
        if (v.relay_listen >= 0) {
            std::string key = v.module + "/" + v.id;
            plugin_.watch_fd(v.relay_listen, [this, key] { accept_relay(key); });
        }
        v.pid = launch_firefox(s);
        ::close(v.wayland_fd);  // Firefox has it now
        v.wayland_fd = -1;
        if (v.pid < 0) {
            send_state(v, "error", tr("Could not start Firefox."));
            return;
        }
        Plugin::log("firefox: %s/%s: %s", v.module.c_str(), v.id.c_str(), v.url.c_str());
        send_state(v, "running");
    }

    void stop(View& v) {
        stop_process(v.pid);
        if (v.wayland_fd >= 0) ::close(v.wayland_fd);
        v.wayland_fd = -1;
        if (v.relay_listen >= 0) {
            plugin_.unwatch_fd(v.relay_listen);
            ::close(v.relay_listen);
            v.relay_listen = -1;
        }
        std::string key = v.module + "/" + v.id;
        for (auto it = relays_.begin(); it != relays_.end();) {
            if (it->second.key == key) {
                close_relay(it->second);
                it = relays_.erase(it);
            } else {
                ++it;
            }
        }
        for (auto it = pending_.begin(); it != pending_.end();) {
            if (it->second.key == key) {
                ::close(it->second.fd);
                it = pending_.erase(it);
            } else {
                ++it;
            }
        }
    }

    // ---- Relayed Wayland connections: Firefox's other processes reach the
    // app's display through a connection the app makes for each of them.

    void accept_relay(const std::string& key) {
        auto it = views_.find(key);
        if (it == views_.end() || it->second.relay_listen < 0) return;
        View& v = it->second;
        int fd = ::accept4(v.relay_listen, nullptr, nullptr, SOCK_CLOEXEC);
        if (fd < 0) return;
        if (pending_.size() >= 8) {
            ::close(fd);
            return;
        }
        int req = ++next_req_;
        pending_[req] = Pending{key, fd, now_s()};
        Json msg = Json::object();
        msg["t"] = "connect";
        msg["id"] = v.id;
        msg["req"] = req;
        auto c = clients_.find(v.module);
        if (c != clients_.end())
            for (auto& conn : c->second.conns) conn->send(msg);
    }

    void start_relay(const std::string& key, int firefox_fd, int app_fd) {
        int id = ++next_relay_;
        Relay& r = relays_[id];
        r = Relay{key, firefox_fd, app_fd};
        // Writes block (a peer that stops reading stalls only this relay's
        // sender); reads never do.
        for (int fd : {firefox_fd, app_fd}) fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) & ~O_NONBLOCK);
        plugin_.watch_fd(firefox_fd, [this, id] { pump(id, true); });
        plugin_.watch_fd(app_fd, [this, id] { pump(id, false); });
    }

    void close_relay(Relay& r) {
        for (int* fd : {&r.a, &r.b}) {
            if (*fd < 0) continue;
            plugin_.unwatch_fd(*fd);
            ::close(*fd);
            *fd = -1;
        }
    }

    // Moves what arrived on one side to the other, with the descriptors
    // Wayland passes along (buffers, keymaps).
    void pump(int id, bool from_firefox) {
        auto it = relays_.find(id);
        if (it == relays_.end()) return;
        Relay& r = it->second;
        int src = from_firefox ? r.a : r.b, dst = from_firefox ? r.b : r.a;
        for (;;) {
            char buf[16384];
            alignas(cmsghdr) char ctrl[CMSG_SPACE(sizeof(int) * 28)];
            iovec iov{buf, sizeof buf};
            msghdr mh{};
            mh.msg_iov = &iov;
            mh.msg_iovlen = 1;
            mh.msg_control = ctrl;
            mh.msg_controllen = sizeof ctrl;
            ssize_t n = ::recvmsg(src, &mh, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
            if (n < 0 && (errno == EAGAIN || errno == EINTR)) return;
            std::vector<int> fds;
            for (cmsghdr* c = CMSG_FIRSTHDR(&mh); n > 0 && c; c = CMSG_NXTHDR(&mh, c)) {
                if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS) continue;
                size_t count = (c->cmsg_len - CMSG_LEN(0)) / sizeof(int);
                for (size_t i = 0; i < count; ++i) {
                    int f;
                    std::memcpy(&f, CMSG_DATA(c) + i * sizeof(int), sizeof f);
                    fds.push_back(f);
                }
            }
            bool ok = n > 0 && forward(dst, buf, size_t(n), fds);
            for (int f : fds) ::close(f);
            if (!ok) {
                close_relay(r);
                relays_.erase(it);
                return;
            }
        }
    }

    static bool forward(int dst, const char* data, size_t n, const std::vector<int>& fds) {
        alignas(cmsghdr) char ctrl[CMSG_SPACE(sizeof(int) * 28)] = {};
        size_t off = 0;
        bool first = true;
        while (off < n) {
            iovec iov{const_cast<char*>(data + off), n - off};
            msghdr mh{};
            mh.msg_iov = &iov;
            mh.msg_iovlen = 1;
            if (first && !fds.empty()) {
                mh.msg_control = ctrl;
                mh.msg_controllen = CMSG_SPACE(sizeof(int) * fds.size());
                cmsghdr* c = CMSG_FIRSTHDR(&mh);
                c->cmsg_level = SOL_SOCKET;
                c->cmsg_type = SCM_RIGHTS;
                c->cmsg_len = CMSG_LEN(sizeof(int) * fds.size());
                std::memcpy(CMSG_DATA(c), fds.data(), sizeof(int) * fds.size());
            }
            ssize_t m = ::sendmsg(dst, &mh, MSG_NOSIGNAL);
            if (m < 0) {
                if (errno == EINTR) continue;
                return false;
            }
            off += size_t(m);
            first = false;
        }
        return true;
    }

    // Firefox processes that ended; the app reopens its page if it still wants it.
    void reap() {
        int st;
        pid_t pid;
        while ((pid = waitpid(-1, &st, WNOHANG)) > 0) {
            for (auto it = views_.begin(); it != views_.end(); ++it) {
                if (it->second.pid != pid) continue;
                Plugin::log("firefox: %s exited (status %d)", it->first.c_str(), st);
                it->second.pid = -1;
                send_state(it->second, "closed");
                views_.erase(it);
                break;
            }
        }
    }

    void poll_installer() {
        Installer::Status st = installer_.status();
        if (st.phase == Installer::Phase::Done) {
            if (!st.latest.empty()) latest_ = st.latest;
            if (st.installed_now) Plugin::log("firefox: installed Firefox %s", latest_.c_str());
            for (auto& [key, v] : views_)
                if (v.pid < 0 && v.wayland_fd >= 0 && !installer_.installed_version().empty()) launch(v);
            installer_.acknowledge();
        } else if (st.phase == Installer::Phase::Failed) {
            if (st.error != "cancelled") error_ = st.error;
            Plugin::log("firefox: %s", st.error.c_str());
            for (auto it = views_.begin(); it != views_.end();) {
                if (it->second.pid < 0) {
                    send_state(it->second, "error", st.error);
                    stop(it->second);
                    it = views_.erase(it);
                } else {
                    ++it;
                }
            }
            installer_.acknowledge();
        }
    }

    void send_state(View& v, const std::string& state, const std::string& error = {}) {
        v.state = state;
        Json msg = Json::object();
        msg["t"] = "state";
        msg["id"] = v.id;
        msg["state"] = state;
        if (state == "installing") msg["progress"] = installer_.status().progress;
        if (!error.empty()) msg["error"] = error;
        auto it = clients_.find(v.module);
        if (it == clients_.end()) return;
        for (auto& c : it->second.conns) c->send(msg);
    }

    void drop(const std::string& module) {
        for (auto it = views_.begin(); it != views_.end();) {
            if (it->second.module == module) {
                stop(it->second);
                it = views_.erase(it);
            } else {
                ++it;
            }
        }
        auto it = clients_.find(module);
        if (it == clients_.end()) return;
        for (auto& c : it->second.conns) plugin_.unwatch_fd(c->fd());
        if (it->second.listen_fd >= 0) {
            plugin_.unwatch_fd(it->second.listen_fd);
            ::close(it->second.listen_fd);
        }
        clients_.erase(it);
    }

    Screen build() const {
        Screen ui(tr("Web pages for apps"));
        Installer::Status st = installer_.status();
        std::string installed = installer_.installed_version();
        bool busy = installer_.busy() && st.phase != Installer::Phase::Checking;
        ui.section(tr("Browser for apps"));
        ui.note(tr("Apps that show web pages use this Firefox. Each app has its own profile: logins and cookies "
                   "are not shared between apps."));
        ui.note(tr("Firefox is made by Mozilla and downloaded unchanged from mozilla.org; this module is not a "
                   "Mozilla product. Using Firefox means accepting Mozilla's terms: "
                   "mozilla.org/about/legal/terms/firefox"));
        if (busy) {
            const char* what = st.phase == Installer::Phase::Downloading ? "Downloading"
                               : st.phase == Installer::Phase::Verifying ? "Checking the download"
                                                                         : "Unpacking";
            ui.level(tr(what), st.progress, std::to_string(int(st.progress * 100)) + "%");
            ui.button("cancel", tr("Cancel"));
        } else if (installed.empty()) {
            ui.info(tr("Firefox"), tr("not installed"));
            ui.note(tr("It is downloaded from Mozilla (about 80 MB, 270 MB on disk) when an app first needs it, "
                       "or now:"));
            ui.button("install", tr("Install Firefox"), "primary");
        } else {
            ui.info(tr("Firefox"), installed + " ESR");
            if (!latest_.empty() && compare_versions(latest_, installed) > 0) {
                ui.info(tr("Available"), latest_, "warn");
                ui.button("update", tr("Update (open pages restart)"), "primary");
            }
        }
        if (!error_.empty()) ui.info(tr("Error"), error_, "bad");
        ui.section(tr("Open pages"));
        if (views_.empty()) ui.note(tr("None right now."));
        for (const auto& [key, v] : views_) ui.info(v.module, v.url);
        if (!installed.empty() && !busy) {
            ui.section(tr("Storage"));
            ui.button("remove", tr("Remove Firefox (apps download it again when needed)"));
        }
        return ui;
    }

    Plugin& plugin_;
    Installer installer_;
    std::map<std::string, Client> clients_;
    std::map<std::string, View> views_;
    std::map<int, Pending> pending_;  // by request number
    std::map<int, Relay> relays_;
    int next_req_ = 0, next_relay_ = 0;
    std::string latest_, error_;
    double last_check_ = 0, last_progress_ = 0;
};

}  // namespace

int main() {
    Plugin plugin("webview", FIREFOX_MODULE_VERSION);  // keep in sync with manifest.json
    firefox_module::register_translations(plugin.catalog());
    FirefoxModule app(plugin);
    plugin.on_hello = [&](const Json&) { app.hello(); };
    plugin.on_consumer = [&](const Consumer& c) { app.consumer(c); };
    plugin.on_event = [&](const std::string& id, const Json&) { app.on_event(id); };
    plugin.on_visible = [&](bool) { app.refresh(); };
    plugin.on_locale = [&](const std::string&) { app.refresh(); };
    plugin.on_tick = [&] { app.tick(); };
    plugin.on_shutdown = [&] { app.shutdown(); };
    return plugin.run(250);
}
