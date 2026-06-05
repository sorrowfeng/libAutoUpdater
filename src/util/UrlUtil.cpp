#include "util/UrlUtil.h"

#include "util/PathUtil.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>

namespace autoupdater::util {

namespace {

int hexValue(char ch) {
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return 10 + (ch - 'a');
    }
    if (ch >= 'A' && ch <= 'F') {
        return 10 + (ch - 'A');
    }
    return -1;
}

std::string percentDecodePath(std::string value) {
    std::string decoded;
    decoded.reserve(value.size());
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '%' && i + 2 < value.size()) {
            const int high = hexValue(value[i + 1]);
            const int low = hexValue(value[i + 2]);
            if (high >= 0 && low >= 0) {
                decoded.push_back(static_cast<char>((high << 4) | low));
                i += 2;
                continue;
            }
        }
        decoded.push_back(value[i]);
    }
    return decoded;
}

} // namespace

std::string joinUrl(const std::string& baseUrl, const std::string& relativePath) {
    if (relativePath.find("://") != std::string::npos || isFileUrl(relativePath)) {
        return relativePath;
    }
    if (baseUrl.empty()) {
        return relativePath;
    }
    if (baseUrl.back() == '/') {
        return baseUrl + relativePath;
    }
    return baseUrl + "/" + relativePath;
}

bool urlStartsWithAny(const std::string& url, const std::vector<std::string>& allowed) {
    if (allowed.empty()) {
        return true;
    }
    return std::any_of(allowed.begin(), allowed.end(), [&](const std::string& prefix) {
        if (prefix.empty() || url.compare(0, prefix.size(), prefix) != 0) {
            return false;
        }
        if (prefix.back() == '/' || url.size() == prefix.size()) {
            return true;
        }
        const auto next = url[prefix.size()];
        return next == '/' || next == '?' || next == '#';
    });
}

bool isFileUrl(const std::string& url) {
    return url.compare(0, 7, "file://") == 0;
}

std::filesystem::path fileUrlToPath(const std::string& url) {
    if (!isFileUrl(url)) {
        return pathFromUtf8(url);
    }
    std::string path = url.substr(7);
    if (path.rfind("localhost/", 0) == 0) {
        path.erase(0, std::string("localhost").size());
    }
    path = percentDecodePath(std::move(path));
#ifdef _WIN32
    if (path.size() >= 3 && path[0] == '/' && std::isalpha(static_cast<unsigned char>(path[1])) && path[2] == ':') {
        path.erase(path.begin());
    }
#endif
#ifdef _WIN32
    std::replace(path.begin(), path.end(), '/', '\\');
#endif
    return pathFromUtf8(path);
}

} // namespace autoupdater::util
