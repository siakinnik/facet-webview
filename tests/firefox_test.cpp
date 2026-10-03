// Unit tests of the parts that need neither the network nor Firefox.
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "browser.h"
#include "installer.h"
#include "package.h"
#include "webview.h"

using namespace ffmod;

namespace {
int failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
}

// One ustar header block (plus data, padded to 512).
std::string tar_entry(const std::string& name, char type, const std::string& data, const std::string& link = {}) {
    char h[512] = {};
    std::memcpy(h, name.c_str(), std::min<size_t>(name.size(), 100));
    std::snprintf(h + 100, 8, "%07o", 0644);
    std::snprintf(h + 108, 8, "%07o", 0);
    std::snprintf(h + 116, 8, "%07o", 0);
    std::snprintf(h + 124, 12, "%011o", unsigned(data.size()));
    std::snprintf(h + 136, 12, "%011o", 0);
    h[156] = type;
    std::memcpy(h + 157, link.c_str(), std::min<size_t>(link.size(), 100));
    std::memcpy(h + 257, "ustar", 6);
    std::memcpy(h + 263, "00", 2);
    std::memset(h + 148, ' ', 8);
    unsigned sum = 0;
    for (unsigned char c : h) sum += c;
    std::snprintf(h + 148, 8, "%06o", sum);
    std::string out(h, 512);
    out += data;
    out.append((512 - data.size() % 512) % 512, '\0');
    return out;
}

std::string temp_dir() {
    char tmpl[] = "/tmp/facet-webview-test-XXXXXX";
    return mkdtemp(tmpl) ? tmpl : "";
}

std::string read_file(const std::string& path) {
    std::ifstream f(path);
    return std::string(std::istreambuf_iterator<char>(f), {});
}

bool exists(const std::string& path) {
    struct stat st;
    return ::lstat(path.c_str(), &st) == 0;
}
}  // namespace

int main() {
    // Versions as Mozilla numbers ESR releases.
    check(compare_versions("140.17.0esr", "140.3.1esr") > 0, "140.17 is newer than 140.3.1");
    check(compare_versions("128.9.0esr", "140.0esr") < 0, "128 is older than 140");
    check(compare_versions("140.3.1esr", "140.3.1esr") == 0, "equal versions");

    // SHA512SUMS lines: 128 hex digits, two spaces, path.
    std::string hash(128, 'a');
    std::string sums = std::string(128, 'b') + "  linux-x86_64/en-US/firefox-1.tar.xz\n" + hash +
                       "  linux-x86_64/ru/firefox-1.tar.xz\n";
    check(find_checksum(sums, "linux-x86_64/ru/firefox-1.tar.xz") == hash, "checksum found");
    check(find_checksum(sums, "linux-x86_64/de/firefox-1.tar.xz").empty(), "no checksum for another file");
    check(firefox_lang("ru") == "ru" && firefox_lang("en") == "en-US" && firefox_lang("de") == "en-US",
          "Firefox languages");

    check(acceptable_url("https://web.max.ru/") && acceptable_url("http://192.168.0.1:8080/x?y=1"), "web addresses");
    check(!acceptable_url("file:///etc/passwd") && !acceptable_url("javascript:alert(1)") &&
              !acceptable_url("https://a b") && !acceptable_url("about:config"),
          "other schemes and spaces refused");
    check(acceptable_id("main") && acceptable_id("chat-2") && !acceptable_id("") && !acceptable_id("../x") &&
              !acceptable_id("A") && !acceptable_id(std::string(33, 'a')),
          "view ids");

    // The window goes to the app's display: its socket, never a display name of ours.
    LaunchSpec s;
    s.app = "/app", s.runtime = "/rt", s.profile = "/p", s.lang = "ru", s.run_dir = "/p/run";
    auto env = firefox_environment(s, 3);
    auto has = [&](const std::string& v) { return std::find(env.begin(), env.end(), v) != env.end(); };
    check(has("WAYLAND_SOCKET=3") && has("WAYLAND_DISPLAY=wayland-0") && has("XDG_RUNTIME_DIR=/p/run") &&
              has("GDK_BACKEND=wayland") && has("LANGUAGE=ru") && has("LD_LIBRARY_PATH=/rt/lib"),
          "Firefox environment");
    s.gl = "/run/facet/gl";
    env = firefox_environment(s, 3);
    check(has("LD_LIBRARY_PATH=/rt/lib:/run/facet/gl/lib") && has("LIBGL_DRIVERS_PATH=/run/facet/gl/lib/dri"),
          "Firefox environment with the GPU");
    check(firefox_prefs().find("app.update.auto\", false") != std::string::npos, "updates are the module's job");

    // Unpacking: the prefix is stripped, links stay links, escapes are refused.
    {
        std::string dir = temp_dir();
        std::string tar = tar_entry("firefox/", '5', "") + tar_entry("firefox/application.ini", '0', "[App]\n") +
                          tar_entry("firefox/browser/", '5', "") +
                          tar_entry("firefox/browser/omni.ja", '0', std::string(1000, 'x')) +
                          tar_entry("firefox/libxul-link.so", '2', "", "browser/omni.ja") +
                          tar_entry("other/ignored", '0', "no") + std::string(1024, '\0');
        TarReader r("firefox/", dir);
        bool ok = r.feed(tar.data(), tar.size());
        check(ok && r.finished(), "archive unpacked");
        check(read_file(dir + "/application.ini") == "[App]\n", "file content");
        check(read_file(dir + "/browser/omni.ja").size() == 1000, "file in a directory");
        check(exists(dir + "/libxul-link.so"), "symlink created");
        check(!exists(dir + "/other") && !exists(dir + "/ignored"), "entries outside the prefix skipped");
        remove_tree(dir);
    }
    {
        std::string dir = temp_dir();
        std::string tar = tar_entry("firefox/../../evil", '0', "x") + std::string(1024, '\0');
        TarReader r("firefox/", dir);
        check(!r.feed(tar.data(), tar.size()), "path leaving the destination refused");
        remove_tree(dir);
    }
    {
        std::string dir = temp_dir();
        std::string tar = tar_entry("firefox/out", '2', "", "/etc/passwd") + std::string(1024, '\0');
        TarReader r("firefox/", dir);
        check(!r.feed(tar.data(), tar.size()), "absolute symlink refused");
        remove_tree(dir);
    }

    // The client channel carries a descriptor with its message.
    {
        int sv[2];
        check(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0, "socketpair");
        facet::webview::Channel a, b;
        a.adopt(sv[0]);
        b.adopt(sv[1]);
        int p[2];
        check(pipe(p) == 0, "pipe");
        facet::Json msg = facet::Json::object();
        msg["t"] = "open";
        msg["id"] = "main";
        msg["fd"] = true;
        check(a.send(msg, p[1]), "send with fd");
        ::close(p[1]);
        std::vector<std::pair<facet::Json, int>> got;
        b.read(got);
        check(got.size() == 1 && got[0].first["id"].str() == "main" && got[0].second >= 0, "message with fd");
        if (!got.empty() && got[0].second >= 0) {
            check(::write(got[0].second, "x", 1) == 1, "received fd works");
            char c = 0;
            check(::read(p[0], &c, 1) == 1 && c == 'x', "same pipe");
            ::close(got[0].second);
        }
        ::close(p[0]);
    }

    if (failures) return 1;
    std::printf("ok\n");
    return 0;
}
