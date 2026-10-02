#pragma once

#include <cstddef>
#include <map>
#include <string>

// Parsed HTTP request (request line + headers). Only what a static file
// server needs: method, target, version, and the header map.
struct HttpRequest {
    std::string method;
    std::string target;
    std::string version;
    std::map<std::string, std::string> headers;  // lower-cased names
};

// An HTTP response under construction. `body` may hold binary data.
struct HttpResponse {
    int status = 200;
    std::string reason = "OK";
    std::map<std::string, std::string> headers;
    std::string body;
};

// Maximum accepted size of the request head (request line + headers).
inline constexpr std::size_t kMaxRequestHeadBytes = 8192;

// Reads from fd until the end of the HTTP request head ("\r\n\r\n").
// Returns true on success. On failure returns false and sets error_status
// to the status code the server should answer with (431 when the head
// exceeds kMaxRequestHeadBytes, 400 otherwise).
bool read_request_head(int fd, std::string& out_head, int& error_status);

// Parses a request head into req. Returns true on success; on failure
// returns false and sets error_status (400 malformed, 505 bad version).
bool parse_request(const std::string& head, HttpRequest& req, int& error_status);

// Routes a parsed request against docroot_real (a canonical absolute path)
// and builds the response, enforcing the security policy.
HttpResponse handle_request(const HttpRequest& req, const std::string& docroot_real);

// Builds a plain-text error response with the given status.
HttpResponse make_error_response(int status);

// Serializes a response to wire format. When head_only is true (HEAD
// requests) the headers are sent with the real Content-Length but the
// body is omitted.
std::string serialize_response(const HttpResponse& res, bool head_only);

// Percent-decodes `in` into `out`. Returns false on invalid encodings.
// Note: '+' is left as-is; it only means space in query strings, not paths.
bool url_decode(const std::string& in, std::string& out);

// Sends exactly len bytes unless the peer goes away. Returns false on error.
bool send_all(int fd, const char* data, std::size_t len);

// Thread-safe access log line to stdout:
//   127.0.0.1 "GET /index.html" 200 1234 2ms
void log_access(const std::string& peer, const std::string& method,
                const std::string& target, int status, std::size_t bytes,
                long long elapsed_ms);
