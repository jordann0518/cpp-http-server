#include "http.hpp"

#include <cerrno>
#include <cctype>
#include <climits>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iostream>
#include <mutex>
#include <vector>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "mime.hpp"

namespace {

// Files larger than this are refused with 500 instead of being read into RAM.
constexpr long long kMaxFileBytes = 100LL * 1024 * 1024;

int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::string trim(const std::string& s) {
    std::size_t b = s.find_first_not_of(" \t");
    if (b == std::string::npos) return "";
    std::size_t e = s.find_last_not_of(" \t");
    return s.substr(b, e - b + 1);
}

std::string lower(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

const char* reason_for(int status) {
    switch (status) {
        case 200: return "OK";
        case 400: return "Bad Request";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 431: return "Request Header Fields Too Large";
        case 500: return "Internal Server Error";
        case 505: return "HTTP Version Not Supported";
        default: return "Error";
    }
}

std::string http_date() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[64];
    std::strftime(buf, sizeof buf, "%a, %d %b %Y %H:%M:%S GMT", &tm);
    return std::string(buf);
}

std::vector<std::string> split_segments(const std::string& path) {
    std::vector<std::string> segs;
    std::size_t i = 0;
    while (i < path.size()) {
        std::size_t j = path.find('/', i);
        if (j == std::string::npos) j = path.size();
        if (j > i) segs.push_back(path.substr(i, j - i));
        i = j + 1;
    }
    return segs;
}

bool read_file(const std::string& fs_path, long long size, std::string& out) {
    out.clear();
    out.resize(static_cast<std::size_t>(size));
    std::ifstream f(fs_path, std::ios::binary);
    if (!f) return false;
    f.read(out.data(), static_cast<std::streamsize>(size));
    return f.gcount() == static_cast<std::streamsize>(size);
}

}  // namespace

bool url_decode(const std::string& in, std::string& out) {
    out.clear();
    out.reserve(in.size());
    for (std::size_t i = 0; i < in.size(); ++i) {
        char c = in[i];
        if (c == '%') {
            if (i + 2 >= in.size()) return false;
            int hi = hex_val(in[i + 1]);
            int lo = hex_val(in[i + 2]);
            if (hi < 0 || lo < 0) return false;
            out.push_back(static_cast<char>((hi << 4) | lo));
            i += 2;
        } else {
            out.push_back(c);
        }
    }
    return true;
}

bool read_request_head(int fd, std::string& out_head, int& error_status) {
    char buf[4096];
    out_head.clear();
    for (;;) {
        ssize_t n = recv(fd, buf, sizeof buf, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            error_status = 400;
            return false;
        }
        if (n == 0) {  // peer closed before sending a full head
            error_status = 400;
            return false;
        }
        out_head.append(buf, static_cast<std::size_t>(n));
        if (out_head.size() > kMaxRequestHeadBytes) {
            error_status = 431;
            return false;
        }
        if (out_head.find("\r\n\r\n") != std::string::npos) {
            return true;
        }
    }
}

bool parse_request(const std::string& head, HttpRequest& req, int& error_status) {
    std::size_t eol = head.find("\r\n");
    if (eol == std::string::npos) {
        error_status = 400;
        return false;
    }
    std::string line = head.substr(0, eol);
    std::size_t s1 = line.find(' ');
    std::size_t s2 = (s1 == std::string::npos) ? std::string::npos : line.find(' ', s1 + 1);
    // Exactly three space-separated parts, no trailing junk.
    if (s1 == std::string::npos || s2 == std::string::npos ||
        line.find(' ', s2 + 1) != std::string::npos) {
        error_status = 400;
        return false;
    }
    req.method = line.substr(0, s1);
    req.target = line.substr(s1 + 1, s2 - s1 - 1);
    req.version = line.substr(s2 + 1);
    if (req.method.empty() || req.target.empty() || req.version.empty()) {
        error_status = 400;
        return false;
    }
    if (req.version != "HTTP/1.1" && req.version != "HTTP/1.0") {
        error_status = 505;
        return false;
    }

    req.headers.clear();
    std::size_t pos = eol + 2;
    while (pos < head.size()) {
        std::size_t e = head.find("\r\n", pos);
        if (e == std::string::npos) break;
        if (e == pos) break;  // blank line: end of headers
        std::string h = head.substr(pos, e - pos);
        std::size_t colon = h.find(':');
        if (colon == std::string::npos) {
            error_status = 400;
            return false;
        }
        std::string name = lower(trim(h.substr(0, colon)));
        std::string value = trim(h.substr(colon + 1));
        if (name.empty()) {
            error_status = 400;
            return false;
        }
        req.headers[name] = value;
        pos = e + 2;
    }
    return true;
}

HttpResponse make_error_response(int status) {
    HttpResponse res;
    res.status = status;
    res.reason = reason_for(status);
    res.body = std::to_string(status) + " " + res.reason + "\n";
    res.headers["Content-Type"] = "text/plain; charset=utf-8";
    res.headers["Content-Length"] = std::to_string(res.body.size());
    return res;
}

HttpResponse handle_request(const HttpRequest& req, const std::string& docroot_real) {
    if (req.method != "GET" && req.method != "HEAD") {
        HttpResponse res = make_error_response(405);
        res.headers["Allow"] = "GET, HEAD";
        return res;
    }
    // Origin-form targets only.
    if (req.target.empty() || req.target[0] != '/') {
        return make_error_response(400);
    }

    // Strip any query string; this server ignores it.
    std::string path = req.target;
    std::size_t q = path.find('?');
    if (q != std::string::npos) path = path.substr(0, q);

    std::string decoded;
    if (!url_decode(path, decoded)) {
        return make_error_response(400);
    }
    if (decoded.find('\0') != std::string::npos) {
        return make_error_response(400);
    }

    // Path traversal defense, applied to the decoded path: any ".."
    // segment is rejected before touching the filesystem. This catches
    // /../, %2e%2e, %2E%2E, ..%2f and friends in a single check.
    for (const std::string& seg : split_segments(decoded)) {
        if (seg == "..") {
            return make_error_response(403);
        }
    }

    std::string fs_path = docroot_real;
    for (const std::string& seg : split_segments(decoded)) {
        fs_path += "/" + seg;
    }

    struct stat st{};
    if (lstat(fs_path.c_str(), &st) != 0) {
        return make_error_response(404);
    }
    if (S_ISDIR(st.st_mode)) {
        // Directories serve their index.html; there is no directory listing.
        std::string index = fs_path + "/index.html";
        struct stat ist{};
        if (lstat(index.c_str(), &ist) != 0 || !S_ISREG(ist.st_mode)) {
            return make_error_response(403);
        }
        fs_path = index;
        st = ist;
    } else if (!S_ISREG(st.st_mode)) {
        return make_error_response(403);
    }

    // Symlink jail: even without "..", a symlink inside the docroot could
    // point outside it. Resolve and re-check the prefix.
    char resolved[PATH_MAX];
    if (realpath(fs_path.c_str(), resolved) == nullptr) {
        return make_error_response(404);
    }
    std::string real = resolved;
    if (real != docroot_real && real.rfind(docroot_real + "/", 0) != 0) {
        return make_error_response(403);
    }

    long long size = static_cast<long long>(st.st_size);
    if (size > kMaxFileBytes) {
        return make_error_response(500);
    }
    std::string body;
    if (!read_file(fs_path, size, body)) {
        return make_error_response(500);
    }

    HttpResponse res;
    res.status = 200;
    res.reason = reason_for(200);
    res.headers["Content-Type"] = mime_type_for(fs_path);
    res.headers["Content-Length"] = std::to_string(body.size());
    res.body = std::move(body);
    return res;
}

std::string serialize_response(const HttpResponse& res, bool head_only) {
    std::string out;
    out += "HTTP/1.1 " + std::to_string(res.status) + " " + res.reason + "\r\n";
    for (const auto& kv : res.headers) {
        out += kv.first + ": " + kv.second + "\r\n";
    }
    out += "Connection: close\r\n";
    out += "Date: " + http_date() + "\r\n";
    out += "\r\n";
    if (!head_only) {
        out += res.body;
    }
    return out;
}

bool send_all(int fd, const char* data, std::size_t len) {
    std::size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, data + sent, len - sent, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (n == 0) return false;
        sent += static_cast<std::size_t>(n);
    }
    return true;
}

void log_access(const std::string& peer, const std::string& method,
                const std::string& target, int status, std::size_t bytes,
                long long elapsed_ms) {
    static std::mutex log_mutex;  // keep one request per line across threads
    std::lock_guard<std::mutex> lock(log_mutex);
    std::cout << peer << " \"" << method << " " << target << "\" " << status
              << " " << bytes << " " << elapsed_ms << "ms" << std::endl;
}
