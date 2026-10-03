// Version order and unpacking one directory of a .tar.xz (Firefox's
// release archives) with checks against paths leaving the destination.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

namespace ffmod {

// dpkg's version order: < 0, 0, > 0.
int compare_versions(const std::string& a, const std::string& b);

// Unpacks the entries of a .tar.xz below `prefix` (e.g. "firefox/") into
// `dest`, without the prefix. Paths that would leave `dest` are refused.
// progress(done, total) counts compressed bytes.
bool extract_tar_xz(const std::string& archive, const std::string& prefix, const std::string& dest,
                 const std::function<void(uint64_t done, uint64_t total)>& progress,
                 const std::atomic<bool>& cancel, std::string& error);

// Unpacks a tar stream (used by extract_tar_xz; separate for tests).
class TarReader {
public:
    TarReader(std::string prefix, std::string dest) : prefix_(std::move(prefix)), dest_(std::move(dest)) {}
    ~TarReader();
    // Feeds the next bytes of the archive. False on an error (see error()).
    bool feed(const char* data, size_t n);
    bool finished() const { return finished_; }
    const std::string& error() const { return error_; }
    size_t files() const { return files_; }

private:
    bool header(const char* block);
    bool begin_entry(char type, const std::string& name, const std::string& link, uint64_t size, unsigned mode);
    bool fail(const std::string& e) {
        error_ = e;
        return false;
    }

    std::string prefix_, dest_;
    std::string block_;  // partial 512-byte block
    uint64_t left_ = 0, pad_ = 0;  // data bytes of the current entry, padding after it
    char kind_ = 0;  // what the current data is for: 'f' file, 'L' long name, 'K' long link, 'x' pax, 's' skip
    std::string meta_;  // long name / pax data being collected
    std::string long_name_, long_link_;
    int fd_ = -1;
    bool finished_ = false;
    size_t files_ = 0;
    std::string error_;
};

}  // namespace ffmod
