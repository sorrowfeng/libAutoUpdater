#pragma once

#include "libAutoUpdater/Result.h"

#include <string>
#include <vector>

namespace autoupdater {

class Version {
public:
    Version() = default;
    Version(int major, int minor, int patch, std::string prerelease = {}, std::string buildMetadata = {});

    static Result<Version> parse(const std::string& text) noexcept;

    std::string toString() const;

    int major() const noexcept { return major_; }
    int minor() const noexcept { return minor_; }
    int patch() const noexcept { return patch_; }
    const std::string& prerelease() const noexcept { return prerelease_; }
    const std::string& buildMetadata() const noexcept { return buildMetadata_; }

    friend bool operator==(const Version& lhs, const Version& rhs) noexcept;
    friend bool operator!=(const Version& lhs, const Version& rhs) noexcept;
    friend bool operator<(const Version& lhs, const Version& rhs) noexcept;
    friend bool operator>(const Version& lhs, const Version& rhs) noexcept;
    friend bool operator<=(const Version& lhs, const Version& rhs) noexcept;
    friend bool operator>=(const Version& lhs, const Version& rhs) noexcept;

private:
    int major_ = 0;
    int minor_ = 0;
    int patch_ = 0;
    std::string prerelease_;
    std::string buildMetadata_;
};

} // namespace autoupdater

