#pragma once

#include "libAutoUpdater/Error.h"

#include <utility>

namespace autoupdater::detail {

inline Error withErrorPhase(Error error, ErrorPhase phase) noexcept {
    error.phase = phase;
    return error;
}

inline Error withFallbackErrorPhase(Error error, ErrorPhase phase) noexcept {
    if (error.phase == ErrorPhase::General) {
        error.phase = phase;
    }
    return error;
}

} // namespace autoupdater::detail
