#include "browser.h"

#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

namespace ffmod {

namespace {
constexpr int kWaylandFd = 3;  // where the child finds the Wayland socket

void mkdirs(const std::string& path) {
    for (size_t i = 1; i <= path.size(); ++i)
        if (i == path.size() || path[i] == '/') ::mkdir(path.substr(0, i).c_str(), 0700);
}

std::string env_of(const char* name) {
    const char* v = std::getenv(name);
    return v ? v : "";
}
}  // namespace

bool acceptable_url(const std::string& url) {
    if (url.size() > 4096) return false;
    if (url.compare(0, 8, "https://") != 0 && url.compare(0, 7, "http://") != 0) return false;
    for (unsigned char c : url)
        if (c < 0x21 || c == 0x7f) return false;
    return true;
}

bool acceptable_id(const std::string& id) {
    if (id.empty() || id.size() > 32) return false;
    for (char c : id)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_')) return false;
    return true;
}

std::string firefox_prefs() {
    return R"(// Written by the Facet Firefox module on every start.
user_pref("browser.shell.checkDefaultBrowser", false);
user_pref("browser.startup.homepage_override.mstone", "ignore");
user_pref("browser.aboutwelcome.enabled", false);
user_pref("browser.sessionstore.resume_from_crash", false);
user_pref("browser.tabs.warnOnClose", false);
user_pref("datareporting.policy.dataSubmissionEnabled", false);
user_pref("toolkit.telemetry.reportingpolicy.firstRun", false);
user_pref("app.update.auto", false);
user_pref("dom.w3c_touch_events.enabled", 1);
user_pref("apz.allow_zooming", true);
user_pref("widget.use-xdg-desktop-portal.file-picker", 0);
user_pref("widget.use-xdg-desktop-portal.mime-handler", 0);
user_pref("widget.use-xdg-desktop-portal.settings", 0);
user_pref("media.hardware-video-decoding.enabled", false);
user_pref("media.eme.enabled", true);
user_pref("media.gmp-widevinecdm.enabled", true);
user_pref("media.gmp-widevinecdm.visible", true);
user_pref("media.gmp-manager.updateEnabled", true);
)";
}

std::vector<std::string> firefox_environment(const LaunchSpec& s, int fd) {
    std::vector<std::string> env = {
        "HOME=" + s.profile + "/home",
        "PATH=/usr/local/bin:/usr/bin:/bin",
        "LANG=C.UTF-8",  // the only locale sure to exist on the host
        "LANGUAGE=" + s.lang,
        "TZ=" + env_of("TZ"),
        "TMPDIR=/tmp",
        "XDG_RUNTIME_DIR=" + s.run_dir,
        "XDG_CACHE_HOME=" + s.profile + "/cache",
        "XDG_SESSION_TYPE=wayland",
        // The app module's own connection to its display: the window shows
        // up there, with that module's scopes. Helper processes (the GPU
        // check) connect by name and are relayed to the same display.
        "WAYLAND_SOCKET=" + std::to_string(fd),
        "WAYLAND_DISPLAY=wayland-0",
        "GDK_BACKEND=wayland",
        "MOZ_ENABLE_WAYLAND=1",
        "MOZ_DISABLE_WAYLAND_PROXY=1",  // its proxy wants a display name; the socket is enough
        "GTK_IM_MODULE=wayland",
        "GSETTINGS_BACKEND=memory",
        "DBUS_SESSION_BUS_ADDRESS=unix:path=/nonexistent",
        "FONTCONFIG_FILE=" + s.runtime + "/etc/fonts.conf",
        "MOZ_CRASHREPORTER_DISABLE=1",
        "NO_AT_BRIDGE=1",
        "MOZ_DISABLE_AUTO_SAFE_MODE=1",
    };
    if (!s.gl.empty()) {
        // The graphics card through Mesa from Facet's OpenGL package; the
        // compositor hands the GPU-drawn page to Facet without copies.
        // Firefox takes GL functions from libGL.so.1 first; the host's one
        // (another glvnd) returns nothing through the package's dispatcher,
        // so <profile>/gl points it at the package's libGLESv2.
        env.push_back("LD_LIBRARY_PATH=" + s.profile + "/gl:" + s.runtime + "/lib:" + s.gl + "/lib");
        env.push_back("LIBGL_DRIVERS_PATH=" + s.gl + "/lib/dri");
        env.push_back("__EGL_VENDOR_LIBRARY_FILENAMES=" + s.gl + "/share/glvnd/egl_vendor.d/50_mesa.json");
        env.push_back("MESA_SHADER_CACHE_DIR=" + s.profile + "/cache/mesa");
    } else {
        env.push_back("LD_LIBRARY_PATH=" + s.runtime + "/lib");
    }
    return env;
}

pid_t launch_firefox(const LaunchSpec& s) {
    mkdirs(s.profile + "/home");
    mkdirs(s.profile + "/cache");
    mkdirs(s.run_dir);
    if (!s.gl.empty()) {
        mkdirs(s.profile + "/gl");
        for (const char* name : {"/gl/libGL.so.1", "/gl/libGL.so"}) {
            std::string link = s.profile + name;
            ::unlink(link.c_str());
            if (::symlink((s.gl + "/lib/libGLESv2.so.2").c_str(), link.c_str()) != 0)
                std::perror(("symlink " + link).c_str());
        }
    }
    if (FILE* f = std::fopen((s.profile + "/user.js").c_str(), "w")) {
        std::fputs(firefox_prefs().c_str(), f);
        std::fclose(f);
    }
    std::vector<std::string> argv = {s.app + "/firefox", "--profile", s.profile, "--no-remote", "--kiosk", s.url};
    std::vector<std::string> env = firefox_environment(s, kWaylandFd);
    std::vector<char*> a, e;
    for (auto& x : argv) a.push_back(x.data());
    a.push_back(nullptr);
    for (auto& x : env) e.push_back(x.data());
    e.push_back(nullptr);
    pid_t pid = fork();
    if (pid == 0) {
        setpgid(0, 0);
        if (s.wayland_fd == kWaylandFd) fcntl(kWaylandFd, F_SETFD, 0);  // dup2 onto itself keeps CLOEXEC
        else if (dup2(s.wayland_fd, kWaylandFd) < 0) _exit(126);
        int null = open("/dev/null", O_RDWR);
        if (null >= 0) dup2(null, 0);  // stdin/stdout are Facet's protocol pipes
        dup2(2, 1);
        for (int fd = kWaylandFd + 1; fd < 1024; ++fd) ::close(fd);
        sigset_t none;
        sigemptyset(&none);
        sigprocmask(SIG_SETMASK, &none, nullptr);
        signal(SIGPIPE, SIG_DFL);
        if (chdir(s.profile.c_str()) != 0) _exit(126);
        execve(a[0], a.data(), e.data());
        _exit(127);
    }
    return pid;
}

void stop_process(pid_t& pid) {
    if (pid <= 0) return;
    ::kill(-pid, SIGTERM);
    for (int i = 0; i < 30; ++i) {
        if (waitpid(pid, nullptr, WNOHANG) == pid) {
            pid = -1;
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    ::kill(-pid, SIGKILL);
    waitpid(pid, nullptr, 0);
    pid = -1;
}

}  // namespace ffmod
