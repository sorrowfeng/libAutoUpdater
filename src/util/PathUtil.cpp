#include "util/PathUtil.h"

#include <algorithm>
#include <cctype>

namespace autoupdater::util {

namespace {

bool hasDrivePrefix(const std::string& path) {
    return path.size() >= 2 && std::isalpha(static_cast<unsigned char>(path[0])) && path[1] == ':';
}

bool startsWith(const std::string& value, const std::string& prefix) {
    return value.size() >= prefix.size() && value.compare(0, prefix.size(), prefix) == 0;
}

bool isWindowsDeviceName(const std::string& segment) {
    const auto dot = segment.find('.');
    auto base = segment.substr(0, dot);
    std::transform(base.begin(), base.end(), base.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
    if (base == "CON" || base == "PRN" || base == "AUX" || base == "NUL" || base == "CLOCK$") {
        return true;
    }
    if (base.size() == 4 && base[3] >= '1' && base[3] <= '9') {
        return base.compare(0, 3, "COM") == 0 || base.compare(0, 3, "LPT") == 0;
    }
    return false;
}

bool isUnsafePortableSegment(const std::string& segment) {
    if (segment.empty() || segment == "." || segment == ".." || segment.back() == '.' || segment.back() == ' ' ||
        segment.find(':') != std::string::npos || isWindowsDeviceName(segment)) {
        return true;
    }
    return std::any_of(segment.begin(), segment.end(), [](unsigned char ch) { return ch < 0x20 || ch == 0x7f; });
}

} // namespace

std::filesystem::path pathFromUtf8(const std::string& utf8Path) noexcept {
    try {
        return std::filesystem::u8path(utf8Path);
    } catch (...) {
        return {};
    }
}

std::string pathToUtf8(const std::filesystem::path& path) noexcept {
    try {
        const auto text = path.generic_u8string();
        return std::string(reinterpret_cast<const char*>(text.data()), text.size());
    } catch (...) {
        return {};
    }
}

Result<void> validateManagedPath(const std::string& path) noexcept {
    if (path.empty()) {
        return Result<void>::fail({ErrorCode::PathTraversalRejected, "Managed path is empty"});
    }
    if (path.front() == '/' || path.front() == '\\' || hasDrivePrefix(path)) {
        return Result<void>::fail({ErrorCode::PathTraversalRejected, "Managed path must be relative"});
    }
    if (path.find('\\') != std::string::npos) {
        return Result<void>::fail({ErrorCode::PathTraversalRejected, "Managed path must use forward slashes"});
    }

    std::size_t start = 0;
    while (start <= path.size()) {
        const auto end = path.find('/', start);
        const auto part = path.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (isUnsafePortableSegment(part)) {
            return Result<void>::fail({ErrorCode::PathTraversalRejected, "Managed path contains unsafe segment"});
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }

    return Result<void>::ok();
}

Result<std::filesystem::path> safeJoin(const std::filesystem::path& root, const std::string& relativePath) noexcept {
    const auto valid = validateManagedPath(relativePath);
    if (!valid) {
        return Result<std::filesystem::path>::fail(valid.error());
    }

    try {
        std::filesystem::path joined = root;
        std::size_t start = 0;
        while (start <= relativePath.size()) {
            const auto end = relativePath.find('/', start);
            const auto segment = relativePath.substr(start, end == std::string::npos ? std::string::npos : end - start);
            // relativePath is UTF-8; on Windows operator/=(std::string) would
            // decode it with the active code page (e.g. GBK), which throws for
            // non-ASCII bytes. u8path interprets the bytes as UTF-8 explicitly.
            joined /= std::filesystem::u8path(segment);
            if (end == std::string::npos) {
                break;
            }
            start = end + 1;
        }
        return Result<std::filesystem::path>::ok(joined);
    } catch (...) {
        return Result<std::filesystem::path>::fail({ErrorCode::FileSystemError, "Failed to join managed path"});
    }
}

bool pathAllowedByWhitelist(const std::string& path, const std::vector<std::string>& whitelist) noexcept {
    if (whitelist.empty()) {
        return true;
    }
    for (const auto& rule : whitelist) {
        if (rule.empty()) {
            continue;
        }
        if (rule.back() == '/') {
            if (startsWith(path, rule)) {
                return true;
            }
        } else if (path == rule || startsWith(path, rule + "/")) {
            return true;
        }
    }
    return false;
}

std::string normalizeManifestPath(std::string path) {
    std::replace(path.begin(), path.end(), '\\', '/');
    while (!path.empty() && path.front() == '/') {
        path.erase(path.begin());
    }
    return path;
}

std::filesystem::path defaultStagingRoot(const std::filesystem::path& installDir) {
    return installDir / ".autoupdater";
}

} // namespace autoupdater::util
