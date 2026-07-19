#pragma once

#include <chrono>
#include <cstdint>
#include <limits>

#ifndef _WIN32
#include <sys/types.h>
#endif

namespace autoupdater::detail {

/// Upper bound shared by the public coordinator and the external updater.
/// Keeping this finite prevents duration and platform timeout conversions from
/// overflowing while still allowing unusually slow desktop shutdowns.
constexpr std::chrono::seconds kMaximumProcessWaitTimeout{24 * 60 * 60};

constexpr bool validProcessWaitTimeout(std::chrono::seconds timeout) noexcept {
    return timeout.count() >= 0 && timeout <= kMaximumProcessWaitTimeout;
}

constexpr std::uint64_t maximumPlatformProcessId() noexcept {
#ifdef _WIN32
    return (std::numeric_limits<std::uint32_t>::max)();
#else
    return static_cast<std::uint64_t>((std::numeric_limits<pid_t>::max)());
#endif
}

constexpr bool validPlatformProcessId(std::uint64_t pid) noexcept {
    return pid <= maximumPlatformProcessId();
}

} // namespace autoupdater::detail
