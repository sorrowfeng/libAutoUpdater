#include "util/PathUtil.h"

#include <cctype>

namespace autoupdater::util {

namespace {

bool hasDrivePrefix(const std::string& path) {
    return path.size() >= 2 && std::isalpha(static_cast<unsigned char>(path[0])) && path[1] == ':';
}

bool startsWith(const std::string& value, const std::string& prefix) {
    return value.size() >= prefix.size() && value.compare(0, prefix.size(), prefix) == 0;
}

std::string toLowerAscii(std::string value) {
    for (auto& c : value) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return value;
}

// Windows reserves these device names in every directory, with any extension.
bool isWindowsReservedName(const std::string& segment) {
    const auto dot = segment.find('.');
    const auto stem = toLowerAscii(segment.substr(0, dot));
    static const char* const kReserved[] = {"con",  "prn",  "aux",  "nul",  "com1", "com2", "com3", "com4",
                                            "com5", "com6", "com7", "com8", "com9", "lpt1", "lpt2", "lpt3",
                                            "lpt4", "lpt5", "lpt6", "lpt7", "lpt8", "lpt9"};
    for (const auto* name : kReserved) {
        if (stem == name) {
            return true;
        }
    }
    return false;
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

    bool firstSegment = true;
    std::size_t start = 0;
    while (start <= path.size()) {
        const auto end = path.find('/', start);
        const auto part = path.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (part.empty() || part == "." || part == "..") {
            return Result<void>::fail({ErrorCode::PathTraversalRejected, "Managed path contains unsafe segment"});
        }
        // ':' would address an NTFS alternate data stream instead of the file.
        if (part.find(':') != std::string::npos) {
            return Result<void>::fail({ErrorCode::PathTraversalRejected, "Managed path segment contains ':'"});
        }
        // Windows silently strips trailing dots and spaces, which would
        // desynchronize the hashed name from the on-disk name.
        if (part.back() == '.' || part.back() == ' ') {
            return Result<void>::fail(
                {ErrorCode::PathTraversalRejected, "Managed path segment ends with '.' or space"});
        }
        if (isWindowsReservedName(part)) {
            return Result<void>::fail({ErrorCode::PathTraversalRejected, "Managed path uses a reserved device name"});
        }
        // The updater's internal state directory must never be a target.
        if (firstSegment && part == ".autoupdater") {
            return Result<void>::fail({ErrorCode::PathTraversalRejected, "Managed path targets updater state"});
        }
        firstSegment = false;
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

std::filesystem::path defaultStagingRoot(const std::filesystem::path& installDir) {
    return installDir / ".autoupdater";
}

// True when `value` is a non-empty identifier made only of [A-Za-z0-9._+-]
// and is not "." or "..". Suitable for values interpolated into file names
// such as release ids or version strings.
bool isSafePathComponent(const std::string& value) noexcept {
    if (value.empty() || value == "." || value == "..") {
        return false;
    }
    for (const char c : value) {
        const auto uc = static_cast<unsigned char>(c);
        const bool ok = std::isalnum(uc) || c == '.' || c == '_' || c == '-' || c == '+';
        if (!ok) {
            return false;
        }
    }
    return true;
}

bool isValidSha256Hex(const std::string& value) noexcept {
    if (value.size() != 64) {
        return false;
    }
    for (const char c : value) {
        const bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        if (!ok) {
            return false;
        }
    }
    return true;
}

std::string sanitizePathComponent(std::string value) {
    for (auto& ch : value) {
        if (ch == '+' || ch == '/' || ch == '\\' || ch == ':') {
            ch = '_';
        }
    }
    return value;
}

} // namespace autoupdater::util
