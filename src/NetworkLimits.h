#pragma once

#include "libAutoUpdater/Result.h"
#include "libAutoUpdater/interfaces/INetworkClient.h"

#include <cstdint>
#include <optional>

namespace autoupdater::detail {

inline constexpr std::uint64_t kMaxNetworkResponseHeaderBytes = 64 * 1024;

bool checkedAdd(std::uint64_t left, std::uint64_t right, std::uint64_t& result) noexcept;

Result<void> validateResponseHeadersBudget(const NetworkResponseInfo& response) noexcept;

Result<std::optional<std::uint64_t>> declaredContentLength(const NetworkResponseInfo& response,
                                                           ErrorCode invalidHeaderCode) noexcept;

Result<void> validateResponseBodyBudget(const NetworkResponseInfo& response, std::uint64_t actualBytes,
                                        std::uint64_t maxBytes, ErrorCode invalidHeaderCode) noexcept;

Result<std::uint64_t> remainingTransferBudget(std::uint64_t initialBytes, std::uint64_t maxTotalBytes) noexcept;

} // namespace autoupdater::detail
