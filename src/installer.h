// Downloads and installs Firefox ESR from Mozilla on the device (the module
// does not redistribute Firefox). Runs in a background thread; the plugin
// thread polls status().
//
// Layout under the data directory: app/<version>/ (the unpacked browser),
// app/current -> <version>, download/ (the archive while it is fetched).
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

namespace ffmod {

constexpr const char* kVersions = "https://product-details.mozilla.org/1.0/firefox_versions.json";
constexpr const char* kReleases = "https://archive.mozilla.org/pub/firefox/releases/";

class Installer {
public:
    enum class Phase { Idle, Checking, Downloading, Verifying, Extracting, Done, Failed };
    struct Status {
        Phase phase = Phase::Idle;
        double progress = 0;  // 0..1 within the phase
        uint64_t total = 0;   // bytes to download
        std::string error;    // English, when Failed
        std::string latest;   // newest ESR after a check
        bool installed_now = false;
    };

    explicit Installer(std::string data_dir);
    ~Installer();

    // Looks up the newest ESR; with `install`, also installs it (in `lang`,
    // e.g. "ru" or "en-US") unless it is current.
    void start(bool install, const std::string& lang);
    void cancel();
    bool busy() const { return running_; }
    Status status() const;
    void acknowledge();  // Done/Failed -> Idle

    std::string installed_version() const;  // "" if none
    std::string app_dir() const;            // data/app/current
    void remove_app();

private:
    void run(bool install, std::string lang);
    void set(Phase p, double progress = 0);

    std::string data_;
    mutable std::mutex mu_;
    Status status_;
    std::thread worker_;
    std::atomic<bool> cancel_{false}, running_{false};
};

// Firefox's language for Facet's ("ru" -> "ru", anything else -> "en-US").
std::string firefox_lang(const std::string& facet_lang);
// The SHA-512 of `file` in a SHA512SUMS listing, "" if absent.
std::string find_checksum(const std::string& sums, const std::string& file);
// rm -rf
void remove_tree(const std::string& path);

}  // namespace ffmod
