#pragma once

#include "libAutoUpdater/ApplyPlan.h"
#include "libAutoUpdater/ResourceLimits.h"
#include "libAutoUpdater/Result.h"
#include "libAutoUpdater/interfaces/IFileSystem.h"
#include "util/Rfc3339.h"

#include <filesystem>
#include <optional>
#include <string>

namespace autoupdater {

/// Content identity published after an apply transaction reaches a terminal state.
struct ApplyTransactionReceipt {
    std::string transactionId;
    std::string planDigest;
    std::optional<util::UtcInstant> completedAt{};
};

Result<std::string> serializeApplyTransactionReceipt(const ApplyTransactionReceipt& receipt) noexcept;
Result<ApplyTransactionReceipt> parseApplyTransactionReceipt(const std::string& text) noexcept;
Result<std::optional<ApplyTransactionReceipt>>
loadTerminalApplyTransaction(IFileSystem& fileSystem, const std::filesystem::path& installDir) noexcept;

namespace detail {

Result<std::string> formatApplyCompletionTimestamp(const util::UtcInstant& instant) noexcept;
Result<ApplyPlan> loadTerminalApplyPlan(IFileSystem& fileSystem, const std::filesystem::path& installDir,
                                        const ApplyTransactionReceipt& receipt, const ResourceLimits& limits) noexcept;

} // namespace detail

} // namespace autoupdater
