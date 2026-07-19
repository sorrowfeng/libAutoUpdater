#pragma once

#include "libAutoUpdater/Result.h"
#include "libAutoUpdater/interfaces/IFileSystem.h"

#include <filesystem>
#include <optional>
#include <string>

namespace autoupdater {

/// Content identity published after an apply transaction reaches a terminal state.
struct ApplyTransactionReceipt {
    std::string transactionId;
    std::string planDigest;
};

Result<std::string> serializeApplyTransactionReceipt(const ApplyTransactionReceipt& receipt) noexcept;
Result<ApplyTransactionReceipt> parseApplyTransactionReceipt(const std::string& text) noexcept;
Result<std::optional<ApplyTransactionReceipt>>
loadTerminalApplyTransaction(IFileSystem& fileSystem, const std::filesystem::path& installDir) noexcept;

} // namespace autoupdater
