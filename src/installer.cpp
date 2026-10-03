#include "installer.h"

#include <dirent.h>
#include <ftw.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <sstream>
#include <vector>

#include "facet/json.h"
#include "https.h"
#include "package.h"

namespace ffmod {

namespace {
int remove_entry(const char* path, const struct stat*, int, struct FTW*) {
    ::remove(path);
    return 0;
}

std::string read_link(const std::string& path) {
    char buf[4096];
    ssize_t n = ::readlink(path.c_str(), buf, sizeof buf - 1);
    return n > 0 ? std::string(buf, size_t(n)) : std::string();
}

bool valid_version(const std::string& v) {
    if (v.empty() || v.size() > 32) return false;
    for (char c : v)
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '.')) return false;
    return true;
}

// Enterprise policies: the module updates Firefox itself; no telemetry, no
// first-run pages, no default-browser nagging.
const char* kPolicies = R"({
  "policies": {
    "DisableAppUpdate": true,
    "DisableTelemetry": true,
    "DisableFirefoxStudies": true,
    "DisablePocket": true,
    "DisableProfileImport": true,
    "DontCheckDefaultBrowser": true,
    "EncryptedMediaExtensions": { "Enabled": true, "Locked": true },
    "NoDefaultBookmarks": true,
    "OverrideFirstRunPage": "",
    "OverridePostUpdatePage": "",
    "UserMessaging": { "WhatsNew": false, "ExtensionRecommendations": false, "FeatureRecommendations": false,
                       "UrlbarInterventions": false, "SkipOnboarding": true, "MoreFromMozilla": false }
  }
}
)";
}  // namespace

void remove_tree(const std::string& path) { nftw(path.c_str(), remove_entry, 32, FTW_DEPTH | FTW_PHYS); }

std::string firefox_lang(const std::string& facet_lang) { return facet_lang == "ru" ? "ru" : "en-US"; }

std::string find_checksum(const std::string& sums, const std::string& file) {
    std::istringstream in(sums);
    std::string line;
    while (std::getline(in, line)) {
        size_t sp = line.find("  ");
        if (sp == 128 && line.compare(sp + 2, std::string::npos, file) == 0) return line.substr(0, 128);
    }
    return {};
}

Installer::Installer(std::string data_dir) : data_(std::move(data_dir)) {}

Installer::~Installer() {
    cancel();
    if (worker_.joinable()) worker_.join();
}

std::string Installer::app_dir() const { return data_ + "/app/current"; }

std::string Installer::installed_version() const {
    std::string v = read_link(app_dir());
    struct stat st;
    return !v.empty() && ::stat((app_dir() + "/firefox").c_str(), &st) == 0 ? v : std::string();
}

Installer::Status Installer::status() const {
    std::lock_guard<std::mutex> lock(mu_);
    return status_;
}

void Installer::acknowledge() {
    std::lock_guard<std::mutex> lock(mu_);
    if (status_.phase == Phase::Done || status_.phase == Phase::Failed) status_.phase = Phase::Idle;
}

void Installer::set(Phase p, double progress) {
    std::lock_guard<std::mutex> lock(mu_);
    status_.phase = p;
    status_.progress = progress;
}

void Installer::start(bool install, const std::string& lang) {
    if (running_) return;
    if (worker_.joinable()) worker_.join();
    cancel_ = false;
    running_ = true;
    {
        std::lock_guard<std::mutex> lock(mu_);
        status_ = Status{};
        status_.phase = Phase::Checking;
    }
    worker_ = std::thread([this, install, lang] {
        run(install, lang);
        running_ = false;
    });
}

void Installer::cancel() { cancel_ = true; }

void Installer::remove_app() {
    if (running_) return;
    remove_tree(data_ + "/app");
    remove_tree(data_ + "/download");
}

void Installer::run(bool install, std::string lang) {
    auto fail = [this](const std::string& e) {
        std::lock_guard<std::mutex> lock(mu_);
        status_.phase = Phase::Failed;
        status_.error = e;
    };
    std::string body, error;
    if (!https_get(kVersions, body, error)) return fail(error);
    facet::Json versions;
    if (!facet::Json::parse(body, versions)) return fail("unexpected answer from Mozilla");
    std::string latest = versions["FIREFOX_ESR"].str();
    if (!valid_version(latest)) return fail("unexpected Firefox version: " + latest);
    {
        std::lock_guard<std::mutex> lock(mu_);
        status_.latest = latest;
    }
    if (!install || latest == installed_version()) return set(Phase::Done, 1);

    std::string base = std::string(kReleases) + latest + "/";
    std::string file = "linux-x86_64/" + lang + "/firefox-" + latest + ".tar.xz";
    std::string sums;
    if (!https_get(base + "SHA512SUMS", sums, error)) return fail(error);
    std::string expected = find_checksum(sums, file);
    if (expected.empty()) return fail("no checksum for " + file);

    std::string dl = data_ + "/download";
    ::mkdir(dl.c_str(), 0700);
    std::string archive = dl + "/firefox-" + latest + "-" + lang + ".tar.xz";
    set(Phase::Downloading);
    auto progress = [this](uint64_t done, uint64_t total) {
        std::lock_guard<std::mutex> lock(mu_);
        if (total) status_.total = total;
        status_.progress = status_.total ? double(done) / double(status_.total) : 0;
    };
    if (!https_download(base + file, archive, progress, cancel_, error)) return fail(error);

    set(Phase::Verifying);
    std::string sum = sha512_file(archive, cancel_);
    if (cancel_) return fail("cancelled");
    if (sum != expected) {
        ::remove(archive.c_str());  // never install what does not match Mozilla's checksum
        return fail("the download is damaged (checksum mismatch), try again");
    }

    set(Phase::Extracting);
    std::string apps = data_ + "/app";
    ::mkdir(apps.c_str(), 0755);
    std::string target = apps + "/" + latest, tmp = target + ".new";
    remove_tree(tmp);
    if (!extract_tar_xz(archive, "firefox/", tmp, [this](uint64_t done, uint64_t total) {
            std::lock_guard<std::mutex> lock(mu_);
            status_.progress = total ? double(done) / double(total) : 0;
        }, cancel_, error)) {
        remove_tree(tmp);
        return fail(error);
    }
    ::mkdir((tmp + "/distribution").c_str(), 0755);
    if (FILE* f = std::fopen((tmp + "/distribution/policies.json").c_str(), "w")) {
        std::fputs(kPolicies, f);
        std::fclose(f);
    }
    remove_tree(target);
    if (::rename(tmp.c_str(), target.c_str()) != 0) return fail("cannot install into " + target);
    std::string link = apps + "/current.new";
    ::unlink(link.c_str());
    if (::symlink(latest.c_str(), link.c_str()) != 0 || ::rename(link.c_str(), app_dir().c_str()) != 0)
        return fail("cannot activate the new version");
    if (DIR* d = opendir(apps.c_str())) {
        std::vector<std::string> old;
        while (dirent* e = readdir(d)) {
            std::string n = e->d_name;
            if (n != "." && n != ".." && n != "current" && n != latest) old.push_back(apps + "/" + n);
        }
        closedir(d);
        for (const auto& o : old) remove_tree(o);
    }
    remove_tree(dl);
    std::lock_guard<std::mutex> lock(mu_);
    status_.phase = Phase::Done;
    status_.progress = 1;
    status_.installed_now = true;
}

}  // namespace ffmod
