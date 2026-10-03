#include "package.h"

#include <fcntl.h>
#include <lzma.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <sstream>
#include <vector>

namespace ffmod {

namespace {
int order(char c) {
    if (std::isdigit(static_cast<unsigned char>(c))) return 0;
    if (std::isalpha(static_cast<unsigned char>(c))) return c;
    if (c == '~') return -1;
    if (c) return c + 256;
    return 0;
}

// dpkg's verrevcmp for one part (upstream version or revision).
int compare_part(const char* a, const char* b) {
    while (*a || *b) {
        int diff = 0;
        while ((*a && !std::isdigit(static_cast<unsigned char>(*a))) ||
               (*b && !std::isdigit(static_cast<unsigned char>(*b)))) {
            int ac = order(*a), bc = order(*b);
            if (ac != bc) return ac - bc;
            ++a, ++b;
        }
        while (*a == '0') ++a;
        while (*b == '0') ++b;
        while (std::isdigit(static_cast<unsigned char>(*a)) && std::isdigit(static_cast<unsigned char>(*b))) {
            if (!diff) diff = *a - *b;
            ++a, ++b;
        }
        if (std::isdigit(static_cast<unsigned char>(*a))) return 1;
        if (std::isdigit(static_cast<unsigned char>(*b))) return -1;
        if (diff) return diff;
    }
    return 0;
}

void split(const std::string& v, long& epoch, std::string& upstream, std::string& revision) {
    size_t colon = v.find(':');
    epoch = colon == std::string::npos ? 0 : std::atol(v.c_str());
    std::string rest = colon == std::string::npos ? v : v.substr(colon + 1);
    size_t dash = rest.rfind('-');
    upstream = dash == std::string::npos ? rest : rest.substr(0, dash);
    revision = dash == std::string::npos ? "" : rest.substr(dash + 1);
}
}  // namespace

int compare_versions(const std::string& a, const std::string& b) {
    long ea, eb;
    std::string ua, ub, ra, rb;
    split(a, ea, ua, ra);
    split(b, eb, ub, rb);
    if (ea != eb) return ea < eb ? -1 : 1;
    int c = compare_part(ua.c_str(), ub.c_str());
    if (c) return c < 0 ? -1 : 1;
    c = compare_part(ra.c_str(), rb.c_str());
    return c < 0 ? -1 : c > 0 ? 1 : 0;
}

// ---------------------------------------------------------------- tar

namespace {
uint64_t tar_number(const char* p, size_t n) {
    if (uint8_t(p[0]) & 0x80) {  // base-256 (GNU, big files)
        uint64_t v = uint8_t(p[0]) & 0x7f;
        for (size_t i = 1; i < n; ++i) v = (v << 8) | uint8_t(p[i]);
        return v;
    }
    uint64_t v = 0;
    for (size_t i = 0; i < n && p[i]; ++i) {
        if (p[i] == ' ') continue;
        if (p[i] < '0' || p[i] > '7') break;
        v = v * 8 + uint64_t(p[i] - '0');
    }
    return v;
}

std::string field(const char* p, size_t n) { return std::string(p, strnlen(p, n)); }

// "./usr/share/max/bin/max" -> "usr/share/max/bin/max"; "" if unsafe.
std::string clean(std::string path) {
    while (path.compare(0, 2, "./") == 0) path.erase(0, 2);
    if (path.empty() || path[0] == '/') return {};
    std::stringstream in(path);
    std::string part, out;
    while (std::getline(in, part, '/')) {
        if (part.empty() || part == ".") continue;
        if (part == "..") return {};
        out += (out.empty() ? "" : "/") + part;
    }
    return out;
}

// Resolves "." and ".." within a relative path; "" if it would go above the root.
std::string normalize(const std::string& path) {
    std::vector<std::string> parts;
    std::stringstream in(path);
    std::string part;
    while (std::getline(in, part, '/')) {
        if (part.empty() || part == ".") continue;
        if (part == "..") {
            if (parts.empty()) return {};
            parts.pop_back();
        } else {
            parts.push_back(part);
        }
    }
    std::string out;
    for (const auto& x : parts) out += (out.empty() ? "" : "/") + x;
    return out.empty() ? "." : out;
}

bool mkdirs(const std::string& dir) {
    std::string cur;
    std::stringstream in(dir);
    std::string part;
    if (!dir.empty() && dir[0] == '/') cur = "";
    while (std::getline(in, part, '/')) {
        if (part.empty()) continue;
        cur += "/" + part;
        struct stat st;
        if (::lstat(cur.c_str(), &st) == 0) {
            if (!S_ISDIR(st.st_mode)) return false;  // never through a symlink
            continue;
        }
        if (::mkdir(cur.c_str(), 0755) != 0 && errno != EEXIST) return false;
    }
    return true;
}
}  // namespace

TarReader::~TarReader() {
    if (fd_ >= 0) ::close(fd_);
}

bool TarReader::begin_entry(char type, const std::string& raw, const std::string& link, uint64_t size, unsigned mode) {
    std::string name = clean(raw);
    kind_ = 's';
    left_ = size;
    pad_ = (512 - size % 512) % 512;
    if (raw != "./" && raw != "." && name.empty()) return fail("unsafe path in the archive: " + raw);
    std::string pre = prefix_;
    if (!pre.empty() && pre.back() == '/') pre.pop_back();
    if (name.compare(0, pre.size(), pre) != 0 || (name.size() > pre.size() && name[pre.size()] != '/')) return true;
    std::string rel = name.size() > pre.size() ? name.substr(pre.size() + 1) : "";
    std::string target = rel.empty() ? dest_ : dest_ + "/" + rel;
    std::string parent = target.substr(0, target.rfind('/'));
    if (type == '5') return mkdirs(target) || fail("cannot create " + target);
    if (!mkdirs(parent)) return fail("cannot create " + parent);
    ::unlink(target.c_str());
    if (type == '2') {
        // Links stay inside the app: no absolute targets, no way up past dest.
        std::string resolved = normalize(rel.substr(0, rel.rfind('/') == std::string::npos ? 0 : rel.rfind('/') + 1) + link);
        if (link.empty() || link[0] == '/' || resolved.empty())
            return fail("unsafe link in the archive: " + raw + " -> " + link);
        return ::symlink(link.c_str(), target.c_str()) == 0 || fail("cannot link " + target);
    }
    if (type == '1') {
        std::string from = clean(link);
        if (from.compare(0, pre.size() + 1, pre + "/") != 0) return fail("unsafe hard link: " + link);
        return ::link((dest_ + "/" + from.substr(pre.size() + 1)).c_str(), target.c_str()) == 0 ||
               fail("cannot link " + target);
    }
    if (type != '0' && type != '\0' && type != '7') return true;  // devices, fifos: not in an app
    fd_ = ::open(target.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, (mode & 0111) ? 0755 : 0644);
    if (fd_ < 0) return fail("cannot write " + target);
    kind_ = 'f';
    ++files_;
    return true;
}

bool TarReader::header(const char* b) {
    if (std::all_of(b, b + 512, [](char c) { return c == 0; })) {
        finished_ = true;  // end of archive (two zero blocks; one is enough)
        return true;
    }
    unsigned sum = 0;
    for (int i = 0; i < 512; ++i) sum += (i >= 148 && i < 156) ? ' ' : uint8_t(b[i]);
    if (sum != tar_number(b + 148, 8)) return fail("damaged archive (tar checksum)");
    char type = b[156];
    uint64_t size = tar_number(b + 124, 12);
    unsigned mode = unsigned(tar_number(b + 100, 8));
    std::string name = field(b, 100), link = field(b + 157, 100);
    if (std::memcmp(b + 257, "ustar", 5) == 0 && b[345]) name = field(b + 345, 155) + "/" + name;
    if (type == 'L' || type == 'K' || type == 'x') {
        if (size > (1 << 20)) return fail("damaged archive (header too long)");
        kind_ = type;
        left_ = size;
        pad_ = (512 - size % 512) % 512;
        meta_.clear();
        return true;
    }
    if (type == 'g') {  // global pax header: nothing we need
        kind_ = 's';
        left_ = size;
        pad_ = (512 - size % 512) % 512;
        return true;
    }
    if (!long_name_.empty()) name = long_name_;
    if (!long_link_.empty()) link = long_link_;
    long_name_.clear();
    long_link_.clear();
    return begin_entry(type, name, link, size, mode);
}

bool TarReader::feed(const char* data, size_t n) {
    while (n > 0 && !finished_) {
        if (left_ > 0) {
            size_t take = size_t(std::min<uint64_t>(left_, n));
            if (kind_ == 'f') {
                if (::write(fd_, data, take) != ssize_t(take)) return fail("cannot write (disk full?)");
            } else if (kind_ == 'L' || kind_ == 'K' || kind_ == 'x') {
                meta_.append(data, take);
            }
            data += take, n -= take, left_ -= take;
            if (left_ == 0) {
                if (kind_ == 'f') {
                    ::close(fd_);
                    fd_ = -1;
                } else if (kind_ == 'L') {
                    long_name_ = field(meta_.data(), meta_.size());
                } else if (kind_ == 'K') {
                    long_link_ = field(meta_.data(), meta_.size());
                } else if (kind_ == 'x') {
                    // "len key=value\n" records: path / linkpath override the next header.
                    size_t p = 0;
                    while (p < meta_.size()) {
                        size_t sp = meta_.find(' ', p);
                        size_t len = std::strtoul(meta_.c_str() + p, nullptr, 10);
                        if (sp == std::string::npos || len == 0 || p + len > meta_.size()) break;
                        std::string rec = meta_.substr(sp + 1, p + len - sp - 2);
                        size_t eq = rec.find('=');
                        if (eq != std::string::npos) {
                            if (rec.compare(0, eq, "path") == 0) long_name_ = rec.substr(eq + 1);
                            if (rec.compare(0, eq, "linkpath") == 0) long_link_ = rec.substr(eq + 1);
                        }
                        p += len;
                    }
                }
            }
            continue;
        }
        if (pad_ > 0) {
            size_t take = size_t(std::min<uint64_t>(pad_, n));
            data += take, n -= take, pad_ -= take;
            continue;
        }
        size_t take = std::min(n, 512 - block_.size());
        block_.append(data, take);
        data += take, n -= take;
        if (block_.size() == 512) {
            std::string b;
            b.swap(block_);
            if (!header(b.data())) return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------- archives

bool extract_tar_xz(const std::string& archive, const std::string& prefix, const std::string& dest,
                    const std::function<void(uint64_t, uint64_t)>& progress, const std::atomic<bool>& cancel,
                    std::string& error) {
    FILE* f = std::fopen(archive.c_str(), "rb");
    if (!f) {
        error = "cannot open the archive";
        return false;
    }
    std::fseek(f, 0, SEEK_END);
    uint64_t size = uint64_t(std::ftell(f));
    std::fseek(f, 0, SEEK_SET);
    lzma_stream xz = LZMA_STREAM_INIT;
    if (lzma_stream_decoder(&xz, UINT64_MAX, 0) != LZMA_OK) {
        std::fclose(f);
        error = "xz unavailable";
        return false;
    }
    TarReader tar(prefix, dest);
    std::vector<uint8_t> in(1 << 20), out(4 << 20);
    uint64_t done = 0;
    lzma_ret ret = LZMA_OK;
    bool ok = true;
    while (ok && ret != LZMA_STREAM_END && !tar.finished()) {
        if (cancel) {
            error = "cancelled";
            ok = false;
            break;
        }
        if (xz.avail_in == 0 && done < size) {
            size_t want = size_t(std::min<uint64_t>(in.size(), size - done));
            size_t got = std::fread(in.data(), 1, want, f);
            if (got == 0) {
                error = "the archive is truncated";
                ok = false;
                break;
            }
            done += got;
            xz.next_in = in.data();
            xz.avail_in = got;
            progress(done, size);
        }
        xz.next_out = out.data();
        xz.avail_out = out.size();
        ret = lzma_code(&xz, done >= size && xz.avail_in == 0 ? LZMA_FINISH : LZMA_RUN);
        if (ret != LZMA_OK && ret != LZMA_STREAM_END) {
            error = "the archive is damaged (xz)";
            ok = false;
            break;
        }
        size_t produced = out.size() - xz.avail_out;
        if (produced && !tar.feed(reinterpret_cast<const char*>(out.data()), produced)) {
            error = tar.error();
            ok = false;
        }
    }
    lzma_end(&xz);
    std::fclose(f);
    if (ok && tar.files() == 0) {
        error = "nothing to install in the archive";
        ok = false;
    }
    return ok;
}

}  // namespace ffmod
