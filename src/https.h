// A small HTTPS client (OpenSSL, certificate and host name verified against
// the system CA store) for Mozilla's servers: GET into memory and resumable
// downloads to a file.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

namespace ffmod {

struct Url {
    std::string host, path;
    int port = 443;
    static bool parse(const std::string& url, Url& out);  // https:// only
};

// Small responses (indexes). English error on failure.
bool https_get(const std::string& url, std::string& body, std::string& error, size_t max_size = 16 << 20);

// Downloads `url` to `path`, resuming from what `path` already holds and
// retrying broken connections. progress(done, total) is called now and then;
// `cancel` stops it.
bool https_download(const std::string& url, const std::string& path,
                    const std::function<void(uint64_t done, uint64_t total)>& progress,
                    const std::atomic<bool>& cancel, std::string& error);

// SHA-512 of a file as lowercase hex, "" if unreadable.
std::string sha512_file(const std::string& path, const std::atomic<bool>& cancel);

}  // namespace ffmod
