#pragma once

#include "libAutoUpdater/Result.h"

#include <cstdint>
#include <string_view>

namespace autoupdater::util {

/// An RFC 3339 instant normalized to UTC without relying on platform time_t
/// range or precision.
struct UtcInstant {
    std::int64_t unixSeconds = 0;
    std::uint32_t nanoseconds = 0;
};

constexpr bool operator==(const UtcInstant& left, const UtcInstant& right) noexcept {
    return left.unixSeconds == right.unixSeconds && left.nanoseconds == right.nanoseconds;
}

constexpr bool operator!=(const UtcInstant& left, const UtcInstant& right) noexcept {
    return !(left == right);
}

constexpr bool operator<(const UtcInstant& left, const UtcInstant& right) noexcept {
    return left.unixSeconds < right.unixSeconds ||
           (left.unixSeconds == right.unixSeconds && left.nanoseconds < right.nanoseconds);
}

constexpr bool operator>=(const UtcInstant& left, const UtcInstant& right) noexcept {
    return !(left < right);
}

/// Parse the documented updater profile:
/// YYYY-MM-DDTHH:MM:SS[.1-9DIGIT](Z|+/-HH:MM).
Result<UtcInstant> parseRfc3339(std::string_view text) noexcept;

/// Read the system wall clock and normalize it to the same representation.
Result<UtcInstant> currentUtcInstant() noexcept;

} // namespace autoupdater::util
