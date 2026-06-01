#include "util/UrlUtil.h"

#include <algorithm>
#include <cctype>
#include <filesystem>

namespace autoupdater::util {

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

std::string fileUrlToPath(const std::string& url) {
    if (!isFileUrl(url)) {
        return url;
    }
    std::string path = url.substr(7);
#ifdef _WIN32
    if (path.size() >= 3 && path[0] == '/' && std::isalpha(static_cast<unsigned char>(path[1])) && path[2] == ':') {
        path.erase(path.begin());
    }
#endif
#ifdef _WIN32
    std::replace(path.begin(), path.end(), '/', '\\');
#else
    std::replace(path.begin(), path.end(), '/', '/');
#endif
    return path;
}

} // namespace autoupdater::util
