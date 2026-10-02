#include "mime.hpp"

#include <algorithm>
#include <cctype>
#include <unordered_map>

namespace {

const std::unordered_map<std::string, std::string>& table() {
    static const std::unordered_map<std::string, std::string> types = {
        {"html", "text/html; charset=utf-8"},
        {"htm", "text/html; charset=utf-8"},
        {"css", "text/css; charset=utf-8"},
        {"js", "text/javascript; charset=utf-8"},
        {"mjs", "text/javascript; charset=utf-8"},
        {"json", "application/json"},
        {"xml", "application/xml"},
        {"txt", "text/plain; charset=utf-8"},
        {"md", "text/plain; charset=utf-8"},
        {"csv", "text/csv; charset=utf-8"},
        {"png", "image/png"},
        {"jpg", "image/jpeg"},
        {"jpeg", "image/jpeg"},
        {"gif", "image/gif"},
        {"svg", "image/svg+xml"},
        {"ico", "image/x-icon"},
        {"webp", "image/webp"},
        {"pdf", "application/pdf"},
        {"woff", "font/woff"},
        {"woff2", "font/woff2"},
        {"ttf", "font/ttf"},
        {"mp4", "video/mp4"},
        {"webm", "video/webm"},
        {"mp3", "audio/mpeg"},
        {"wav", "audio/wav"},
        {"zip", "application/zip"},
    };
    return types;
}

}  // namespace

std::string mime_type_for(const std::string& path) {
    std::string ext;
    std::size_t dot = path.rfind('.');
    std::size_t slash = path.rfind('/');
    if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) {
        ext = path.substr(dot + 1);
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    }
    auto it = table().find(ext);
    if (it != table().end()) {
        return it->second;
    }
    return "application/octet-stream";
}
