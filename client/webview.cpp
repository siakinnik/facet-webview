#include "webview.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <chrono>
#include <cstring>

namespace facet::webview {

namespace {
double now_s() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

int connect_unix(const std::string& path) {
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (path.empty() || path.size() >= sizeof addr.sun_path) return -1;
    std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
    int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd >= 0 && ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
        ::close(fd);
        fd = -1;
    }
    return fd;
}
}  // namespace

// ---------------------------------------------------------------- Channel

bool Channel::connect(const std::string& path) {
    close();
    int fd = connect_unix(path);
    if (fd < 0) return false;
    adopt(fd);
    return true;
}

void Channel::adopt(int fd) {
    close();
    fd_ = fd;
    fcntl(fd_, F_SETFL, fcntl(fd_, F_GETFL) | O_NONBLOCK);
}

void Channel::close() {
    if (fd_ >= 0) ::close(fd_);
    fd_ = -1;
    in_.clear();
    for (int f : fds_) ::close(f);
    fds_.clear();
}

bool Channel::send(const Json& msg, int pass_fd) {
    if (fd_ < 0) return false;
    std::string line = msg.dump() + "\n";
    iovec iov{line.data(), line.size()};
    msghdr mh{};
    mh.msg_iov = &iov;
    mh.msg_iovlen = 1;
    alignas(cmsghdr) char ctrl[CMSG_SPACE(sizeof(int))] = {};
    if (pass_fd >= 0) {
        mh.msg_control = ctrl;
        mh.msg_controllen = sizeof ctrl;
        cmsghdr* c = CMSG_FIRSTHDR(&mh);
        c->cmsg_level = SOL_SOCKET;
        c->cmsg_type = SCM_RIGHTS;
        c->cmsg_len = CMSG_LEN(sizeof(int));
        std::memcpy(CMSG_DATA(c), &pass_fd, sizeof(int));
    }
    ssize_t n = ::sendmsg(fd_, &mh, MSG_NOSIGNAL);
    if (n < 0) {
        close();
        return false;
    }
    if (size_t(n) < line.size()) {  // rare: a full socket buffer; finish the line without the descriptor
        size_t off = size_t(n);
        int fl = fcntl(fd_, F_GETFL);
        fcntl(fd_, F_SETFL, fl & ~O_NONBLOCK);
        while (off < line.size()) {
            ssize_t m = ::send(fd_, line.data() + off, line.size() - off, MSG_NOSIGNAL);
            if (m <= 0) {
                close();
                return false;
            }
            off += size_t(m);
        }
        fcntl(fd_, F_SETFL, fl);
    }
    return true;
}

bool Channel::read(std::vector<std::pair<Json, int>>& out) {
    if (fd_ < 0) return false;
    bool alive = true;
    for (;;) {
        char buf[16384];
        iovec iov{buf, sizeof buf};
        alignas(cmsghdr) char ctrl[CMSG_SPACE(sizeof(int) * 8)];
        msghdr mh{};
        mh.msg_iov = &iov;
        mh.msg_iovlen = 1;
        mh.msg_control = ctrl;
        mh.msg_controllen = sizeof ctrl;
        ssize_t n = ::recvmsg(fd_, &mh, MSG_CMSG_CLOEXEC);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno != EAGAIN && errno != EWOULDBLOCK) alive = false;
            break;
        }
        for (cmsghdr* c = CMSG_FIRSTHDR(&mh); c; c = CMSG_NXTHDR(&mh, c)) {
            if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS) continue;
            size_t count = (c->cmsg_len - CMSG_LEN(0)) / sizeof(int);
            for (size_t i = 0; i < count; ++i) {
                int f;
                std::memcpy(&f, CMSG_DATA(c) + i * sizeof(int), sizeof(int));
                fds_.push_back(f);
            }
        }
        if (n == 0) {
            alive = false;
            break;
        }
        in_.append(buf, size_t(n));
        if (in_.size() > (1 << 20)) {
            alive = false;
            break;
        }
    }
    size_t nl;
    while ((nl = in_.find('\n')) != std::string::npos) {
        Json msg;
        bool ok = Json::parse(in_.substr(0, nl), msg);
        in_.erase(0, nl + 1);
        if (!ok) continue;
        int f = -1;
        if (msg["fd"].as_bool() && !fds_.empty()) {
            f = fds_.front();
            fds_.pop_front();
        }
        out.emplace_back(std::move(msg), f);
    }
    if (!alive) close();
    return alive;
}

// ---------------------------------------------------------------- WebView

WebView::~WebView() {
    if (channel_.valid()) plugin_.unwatch_fd(channel_.fd());
}

bool WebView::ensure_channel() {
    if (channel_.valid()) return true;
    std::string dir = plugin_.endpoint("web.runtime");
    if (dir.empty() || !channel_.connect(dir + "/web.sock")) return false;
    plugin_.watch_fd(channel_.fd(), [this] { on_readable(); });
    return true;
}

bool WebView::send_open(const std::string& id, const std::string& url) {
    if (!ensure_channel()) return false;
    std::string dir = plugin_.endpoint("display.wayland");
    int wl = dir.empty() ? -1 : connect_unix(dir + "/wayland-0");
    if (wl < 0) return false;  // the compositor is not there yet
    Json msg = Json::object();
    msg["t"] = "open";
    msg["id"] = id;
    msg["url"] = url;
    msg["fd"] = true;
    bool ok = channel_.send(msg, wl);
    ::close(wl);  // the browser module has its own copy now
    return ok;
}

bool WebView::open(const std::string& id, const std::string& url) {
    wanted_[id] = url;
    reopen_at_.erase(id);
    bool ok = send_open(id, url);
    if (!ok) reopen_at_[id] = now_s() + 1;
    return ok;
}

void WebView::close(const std::string& id) {
    wanted_.erase(id);
    reopen_at_.erase(id);
    if (!channel_.valid()) return;
    Json msg = Json::object();
    msg["t"] = "close";
    msg["id"] = id;
    channel_.send(msg);
}

void WebView::tick() {
    double t = now_s();
    for (auto it = reopen_at_.begin(); it != reopen_at_.end();) {
        auto w = wanted_.find(it->first);
        if (w == wanted_.end()) {
            it = reopen_at_.erase(it);
        } else if (t >= it->second) {
            if (send_open(w->first, w->second)) it = reopen_at_.erase(it);
            else it++->second = t + 2;
        } else {
            ++it;
        }
    }
}

void WebView::on_readable() {
    std::vector<std::pair<Json, int>> msgs;
    int fd = channel_.fd();
    bool alive = channel_.read(msgs);
    for (auto& [msg, f] : msgs) {
        if (f >= 0) ::close(f);
        if (msg["t"].str() == "connect") {
            // Another process of the browser wants this module's display.
            std::string dir = plugin_.endpoint("display.wayland");
            int wl = dir.empty() ? -1 : connect_unix(dir + "/wayland-0");
            if (wl < 0) continue;  // it gives up after a while
            Json reply = Json::object();
            reply["t"] = "wayland";
            reply["id"] = msg["id"];
            reply["req"] = msg["req"];
            reply["fd"] = true;
            channel_.send(reply, wl);
            ::close(wl);
            continue;
        }
        if (msg["t"].str() != "state") continue;
        const std::string& id = msg["id"].str();
        const std::string& state = msg["state"].str();
        // The browser went away (crash, update): open the page again soon.
        if (state == "closed" && wanted_.count(id)) reopen_at_[id] = now_s() + 2;
        if (on_state) on_state(id, state, msg["progress"].as_number(), msg["error"].str());
    }
    if (!alive) {
        plugin_.unwatch_fd(fd);
        for (const auto& [id, url] : wanted_) reopen_at_[id] = now_s() + 2;
    }
}

}  // namespace facet::webview
