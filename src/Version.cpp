#include "libAutoUpdater/Version.h"

#include <algorithm>
#include <cctype>
#include <sstream>

#ifndef LIBAUTOUPDATER_VERSION
#define LIBAUTOUPDATER_VERSION "0.0.0"
#endif

namespace autoupdater {

namespace {

bool isIdentifierChar(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '-';
}

bool isNumericIdentifier(const std::string& text) {
    return !text.empty() &&
           std::all_of(text.begin(), text.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); });
}

std::vector<std::string> split(const std::string& text, char delimiter) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (start <= text.size()) {
        const auto end = text.find(delimiter, start);
        parts.push_back(text.substr(start, end == std::string::npos ? std::string::npos : end - start));
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    return parts;
}

bool validDotIdentifiers(const std::string& text, bool allowEmpty) {
    if (text.empty()) {
        return allowEmpty;
    }
    for (const auto& part : split(text, '.')) {
        if (part.empty()) {
            return false;
        }
        if (!std::all_of(part.begin(), part.end(), isIdentifierChar)) {
            return false;
        }
        if (isNumericIdentifier(part) && part.size() > 1 && part.front() == '0') {
            return false;
        }
    }
    return true;
}

int compareNumericIdentifier(const std::string& lhs, const std::string& rhs) noexcept {
    if (lhs.size() < rhs.size()) {
        return -1;
    }
    if (lhs.size() > rhs.size()) {
        return 1;
    }
    if (lhs < rhs) {
        return -1;
    }
    if (lhs > rhs) {
        return 1;
    }
    return 0;
}

Result<int> parseNumber(const std::string& text) {
    if (text.empty()) {
        return Result<int>::fail({ErrorCode::VersionParseFailed, "Version number segment is empty"});
    }
    if (text.size() > 1 && text.front() == '0') {
        return Result<int>::fail({ErrorCode::VersionParseFailed, "Version number segment has leading zero"});
    }
    if (!std::all_of(text.begin(), text.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); })) {
        return Result<int>::fail({ErrorCode::VersionParseFailed, "Version number segment is not numeric"});
    }
    try {
        return Result<int>::ok(std::stoi(text));
    } catch (...) {
        return Result<int>::fail({ErrorCode::VersionParseFailed, "Version number segment is out of range"});
    }
}

int comparePrerelease(const std::string& lhs, const std::string& rhs) noexcept {
    if (lhs.empty() && rhs.empty()) {
        return 0;
    }
    if (lhs.empty()) {
        return 1;
    }
    if (rhs.empty()) {
        return -1;
    }

    const auto left = split(lhs, '.');
    const auto right = split(rhs, '.');
    const auto count = std::min(left.size(), right.size());
    for (std::size_t i = 0; i < count; ++i) {
        const bool leftNumeric = isNumericIdentifier(left[i]);
        const bool rightNumeric = isNumericIdentifier(right[i]);
        if (leftNumeric && rightNumeric) {
            const auto compared = compareNumericIdentifier(left[i], right[i]);
            if (compared != 0) {
                return compared;
            }
        } else if (leftNumeric != rightNumeric) {
            return leftNumeric ? -1 : 1;
        } else {
            if (left[i] < right[i]) {
                return -1;
            }
            if (left[i] > right[i]) {
                return 1;
            }
        }
    }
    if (left.size() < right.size()) {
        return -1;
    }
    if (left.size() > right.size()) {
        return 1;
    }
    return 0;
}

} // namespace

Version::Version(int major, int minor, int patch, std::string prerelease, std::string buildMetadata)
    : major_(major), minor_(minor), patch_(patch), prerelease_(std::move(prerelease)),
      buildMetadata_(std::move(buildMetadata)) {}

Result<Version> Version::parse(const std::string& text) noexcept {
    try {
        if (text.empty()) {
            return Result<Version>::fail({ErrorCode::VersionParseFailed, "Version is empty"});
        }

        std::string coreAndPrerelease = text;
        std::string build;
        const auto plus = text.find('+');
        if (plus != std::string::npos) {
            coreAndPrerelease = text.substr(0, plus);
            build = text.substr(plus + 1);
            if (!validDotIdentifiers(build, false)) {
                return Result<Version>::fail({ErrorCode::VersionParseFailed, "Invalid build metadata"});
            }
        }

        std::string core = coreAndPrerelease;
        std::string prerelease;
        const auto dash = coreAndPrerelease.find('-');
        if (dash != std::string::npos) {
            core = coreAndPrerelease.substr(0, dash);
            prerelease = coreAndPrerelease.substr(dash + 1);
            if (!validDotIdentifiers(prerelease, false)) {
                return Result<Version>::fail({ErrorCode::VersionParseFailed, "Invalid prerelease identifiers"});
            }
        }

        const auto numbers = split(core, '.');
        if (numbers.size() != 3) {
            return Result<Version>::fail({ErrorCode::VersionParseFailed, "Version must have major.minor.patch"});
        }

        auto major = parseNumber(numbers[0]);
        auto minor = parseNumber(numbers[1]);
        auto patch = parseNumber(numbers[2]);
        if (!major) {
            return Result<Version>::fail(major.error());
        }
        if (!minor) {
            return Result<Version>::fail(minor.error());
        }
        if (!patch) {
            return Result<Version>::fail(patch.error());
        }

        return Result<Version>::ok(Version(major.value(), minor.value(), patch.value(), prerelease, build));
    } catch (...) {
        return Result<Version>::fail({ErrorCode::VersionParseFailed, "Unexpected version parser failure"});
    }
}

std::string Version::toString() const {
    std::ostringstream stream;
    stream << major_ << '.' << minor_ << '.' << patch_;
    if (!prerelease_.empty()) {
        stream << '-' << prerelease_;
    }
    if (!buildMetadata_.empty()) {
        stream << '+' << buildMetadata_;
    }
    return stream.str();
}

bool operator==(const Version& lhs, const Version& rhs) noexcept {
    return lhs.major_ == rhs.major_ && lhs.minor_ == rhs.minor_ && lhs.patch_ == rhs.patch_ &&
           lhs.prerelease_ == rhs.prerelease_;
}

bool operator!=(const Version& lhs, const Version& rhs) noexcept {
    return !(lhs == rhs);
}

bool operator<(const Version& lhs, const Version& rhs) noexcept {
    if (lhs.major_ != rhs.major_) {
        return lhs.major_ < rhs.major_;
    }
    if (lhs.minor_ != rhs.minor_) {
        return lhs.minor_ < rhs.minor_;
    }
    if (lhs.patch_ != rhs.patch_) {
        return lhs.patch_ < rhs.patch_;
    }
    return comparePrerelease(lhs.prerelease_, rhs.prerelease_) < 0;
}

bool operator>(const Version& lhs, const Version& rhs) noexcept {
    return rhs < lhs;
}

bool operator<=(const Version& lhs, const Version& rhs) noexcept {
    return !(rhs < lhs);
}

bool operator>=(const Version& lhs, const Version& rhs) noexcept {
    return !(lhs < rhs);
}

Version libraryVersion() noexcept {
    auto parsed = Version::parse(LIBAUTOUPDATER_VERSION);
    if (parsed) {
        return parsed.value();
    }
    return Version{};
}

} // namespace autoupdater
