#include "https.h"

#include <netdb.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/ssl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <thread>

namespace ffmod {

bool Url::parse(const std::string& url, Url& out) {
    const std::string scheme = "https://";
    if (url.compare(0, scheme.size(), scheme) != 0) return false;
    size_t slash = url.find('/', scheme.size());
    std::string hostport = url.substr(scheme.size(), slash == std::string::npos ? std::string::npos : slash - scheme.size());
    out.path = slash == std::string::npos ? "/" : url.substr(slash);
    size_t colon = hostport.find(':');
    out.host = hostport.substr(0, colon);
    out.port = colon == std::string::npos ? 443 : std::atoi(hostport.c_str() + colon + 1);
    return !out.host.empty() && out.port > 0 && out.port < 65536;
}

namespace {

constexpr int kTimeoutS = 30;

class Tls {
public:
    ~Tls() {
        if (ssl_) SSL_free(ssl_);
        if (ctx_) SSL_CTX_free(ctx_);
        if (fd_ >= 0) ::close(fd_);
    }

    bool open(const Url& u, std::string& error) {
        addrinfo hints{}, *res = nullptr;
        hints.ai_socktype = SOCK_STREAM;
        int rc = getaddrinfo(u.host.c_str(), std::to_string(u.port).c_str(), &hints, &res);
        if (rc != 0) {
            error = "cannot resolve " + u.host + ": " + gai_strerror(rc);
            return false;
        }
        for (addrinfo* a = res; a; a = a->ai_next) {
            fd_ = ::socket(a->ai_family, a->ai_socktype | SOCK_CLOEXEC, a->ai_protocol);
            if (fd_ < 0) continue;
            timeval tv{kTimeoutS, 0};
            setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
            setsockopt(fd_, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
            if (::connect(fd_, a->ai_addr, a->ai_addrlen) == 0) break;
            ::close(fd_);
            fd_ = -1;
        }
        freeaddrinfo(res);
        if (fd_ < 0) {
            error = "cannot connect to " + u.host;
            return false;
        }
        ctx_ = SSL_CTX_new(TLS_client_method());
        if (!ctx_) {
            error = "TLS unavailable";
            return false;
        }
        SSL_CTX_set_min_proto_version(ctx_, TLS1_2_VERSION);
        SSL_CTX_set_default_verify_paths(ctx_);
        SSL_CTX_load_verify_locations(ctx_, "/etc/ssl/certs/ca-certificates.crt", "/etc/ssl/certs");
        SSL_CTX_set_verify(ctx_, SSL_VERIFY_PEER, nullptr);
        ssl_ = SSL_new(ctx_);
        SSL_set_fd(ssl_, fd_);
        SSL_set_tlsext_host_name(ssl_, u.host.c_str());
        SSL_set1_host(ssl_, u.host.c_str());
        if (SSL_connect(ssl_) != 1) {
            char buf[256];
            ERR_error_string_n(ERR_get_error(), buf, sizeof buf);
            error = "TLS with " + u.host + " failed: " + buf;
            return false;
        }
        return true;
    }

    bool write(const std::string& s) {
        size_t off = 0;
        while (off < s.size()) {
            int n = SSL_write(ssl_, s.data() + off, int(s.size() - off));
            if (n <= 0) return false;
            off += size_t(n);
        }
        return true;
    }

    // Buffered reading.
    bool fill() {
        char tmp[65536];
        int n = SSL_read(ssl_, tmp, sizeof tmp);
        if (n <= 0) return false;
        buf_.append(tmp, size_t(n));
        return true;
    }

    bool line(std::string& out) {
        for (;;) {
            size_t nl = buf_.find("\r\n");
            if (nl != std::string::npos) {
                out = buf_.substr(0, nl);
                buf_.erase(0, nl + 2);
                return true;
            }
            if (buf_.size() > 65536 || !fill()) return false;
        }
    }

    // Up to `max` bytes; 0 at the end of the stream.
    size_t some(std::string& out, size_t max) {
        if (buf_.empty() && !fill()) return 0;
        size_t n = std::min(max, buf_.size());
        out.assign(buf_, 0, n);
        buf_.erase(0, n);
        return n;
    }

private:
    int fd_ = -1;
    SSL_CTX* ctx_ = nullptr;
    SSL* ssl_ = nullptr;
    std::string buf_;
};

using Headers = std::map<std::string, std::string>;
using Sink = std::function<bool(const std::string& chunk)>;

// One GET. Returns the HTTP status (0 on a connection error) and streams the
// body of a 2xx response into `sink`.
int get_once(const Url& u, uint64_t from, Headers& headers, const Sink& sink, std::string& error) {
    Tls tls;
    if (!tls.open(u, error)) return 0;
    std::string req = "GET " + u.path + " HTTP/1.1\r\nHost: " + u.host +
                      "\r\nUser-Agent: facet-max\r\nAccept-Encoding: identity\r\nConnection: close\r\n";
    if (from > 0) req += "Range: bytes=" + std::to_string(from) + "-\r\n";
    req += "\r\n";
    std::string line;
    if (!tls.write(req) || !tls.line(line)) {
        error = "no response from " + u.host;
        return 0;
    }
    int status = 0;
    if (std::sscanf(line.c_str(), "HTTP/%*s %d", &status) != 1) {
        error = "bad response from " + u.host;
        return 0;
    }
    while (tls.line(line) && !line.empty()) {
        size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string key = line.substr(0, colon);
        std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        size_t v = line.find_first_not_of(" \t", colon + 1);
        headers[key] = v == std::string::npos ? "" : line.substr(v);
    }
    if (status < 200 || status >= 300) return status;
    std::string chunk;
    if (headers["transfer-encoding"].find("chunked") != std::string::npos) {
        for (;;) {
            if (!tls.line(line)) {
                error = "connection lost";
                return 0;
            }
            size_t size = std::strtoul(line.c_str(), nullptr, 16);
            if (size == 0) break;
            while (size > 0) {
                size_t n = tls.some(chunk, size);
                if (n == 0) {
                    error = "connection lost";
                    return 0;
                }
                size -= n;
                if (!sink(chunk)) return -1;
            }
            tls.line(line);  // CRLF after the chunk
        }
        return status;
    }
    bool has_length = headers.count("content-length") > 0;
    uint64_t left = has_length ? std::strtoull(headers["content-length"].c_str(), nullptr, 10) : UINT64_MAX;
    while (left > 0) {
        size_t n = tls.some(chunk, size_t(std::min<uint64_t>(left, 1 << 20)));
        if (n == 0) {
            if (has_length) {
                error = "connection lost";
                return 0;
            }
            break;
        }
        left -= n;
        if (!sink(chunk)) return -1;
    }
    return status;
}

// Absolute target of a redirect.
std::string resolve(const Url& base, const std::string& location) {
    if (location.compare(0, 8, "https://") == 0) return location;
    if (!location.empty() && location[0] == '/') return "https://" + base.host + location;
    return {};
}

}  // namespace

bool https_get(const std::string& url, std::string& body, std::string& error, size_t max_size) {
    std::string current = url;
    for (int hop = 0; hop < 5; ++hop) {
        Url u;
        if (!Url::parse(current, u)) {
            error = "bad URL " + current;
            return false;
        }
        Headers headers;
        body.clear();
        int status = get_once(u, 0, headers, [&](const std::string& c) {
            body += c;
            return body.size() <= max_size;
        }, error);
        if (status >= 200 && status < 300) return true;
        if (status >= 300 && status < 400) {
            current = resolve(u, headers["location"]);
            if (current.empty()) break;
            continue;
        }
        if (status > 0) error = "HTTP " + std::to_string(status) + " for " + url;
        if (status < 0) error = "response too large";
        return false;
    }
    error = "too many redirects for " + url;
    return false;
}

bool https_download(const std::string& url, const std::string& path,
                    const std::function<void(uint64_t, uint64_t)>& progress, const std::atomic<bool>& cancel,
                    std::string& error) {
    std::string current = url;
    int failures = 0;
    for (int hops = 0; hops < 5 && failures < 20;) {
        if (cancel) {
            error = "cancelled";
            return false;
        }
        Url u;
        if (!Url::parse(current, u)) {
            error = "bad URL " + current;
            return false;
        }
        struct stat st;
        uint64_t have = ::stat(path.c_str(), &st) == 0 ? uint64_t(st.st_size) : 0;
        FILE* f = nullptr;
        uint64_t total = 0, done = have;
        Headers headers;
        auto last = std::chrono::steady_clock::now();
        int status = get_once(u, have, headers, [&](const std::string& c) {
            if (!f) {
                // 206: the server continues where we stopped; 200: from the start.
                bool resume = headers.count("content-range") > 0;
                if (!resume) done = 0;
                f = std::fopen(path.c_str(), resume ? "ab" : "wb");
                if (!f) return false;
                std::string range = headers["content-range"];
                size_t slash = range.rfind('/');
                total = slash != std::string::npos ? std::strtoull(range.c_str() + slash + 1, nullptr, 10)
                                                   : std::strtoull(headers["content-length"].c_str(), nullptr, 10);
            }
            if (std::fwrite(c.data(), 1, c.size(), f) != c.size()) return false;
            done += c.size();
            auto now = std::chrono::steady_clock::now();
            if (now - last > std::chrono::milliseconds(300)) {
                last = now;
                progress(done, total);
            }
            return !cancel.load();
        }, error);
        if (f) std::fclose(f);
        if (status >= 200 && status < 300) {
            progress(done, total ? total : done);
            return true;
        }
        if (status == 416) return true;  // nothing left to fetch; the checksum decides
        if (status >= 300 && status < 400) {
            current = resolve(u, headers["location"]);
            ++hops;
            if (current.empty()) break;
            continue;
        }
        if (cancel) {
            error = "cancelled";
            return false;
        }
        if (status > 0 && status != 408 && status < 500) {
            error = "HTTP " + std::to_string(status);
            return false;
        }
        ++failures;  // a dropped connection: resume after a pause
        std::this_thread::sleep_for(std::chrono::seconds(std::min(failures * 2, 30)));
    }
    if (error.empty()) error = "download failed";
    return false;
}

std::string sha512_file(const std::string& path, const std::atomic<bool>& cancel) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return {};
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha512(), nullptr);
    std::string buf(1 << 20, '\0');
    size_t n;
    while ((n = std::fread(buf.data(), 1, buf.size(), f)) > 0 && !cancel) EVP_DigestUpdate(ctx, buf.data(), n);
    std::fclose(f);
    unsigned char md[64];
    unsigned int len = 0;
    EVP_DigestFinal_ex(ctx, md, &len);
    EVP_MD_CTX_free(ctx);
    if (cancel) return {};
    std::string hex;
    char b[3];
    for (unsigned i = 0; i < len; ++i) {
        std::snprintf(b, sizeof b, "%02x", md[i]);
        hex += b;
    }
    return hex;
}

}  // namespace ffmod
