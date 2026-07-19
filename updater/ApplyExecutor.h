#pragma once

#include "libAutoUpdater/ApplyPlan.h"
#include "libAutoUpdater/Result.h"

#include <chrono>
#include <cstdint>
#include <string>

namespace autoupdater::updater {

Result<void> waitForProcessExit(std::uint64_t pid, std::chrono::seconds timeout) noexcept;
Result<std::string> transactionJournalFileName(const ApplyPlan& plan) noexcept;
Result<void> executeApplyPlan(const ApplyPlan& plan) noexcept;

} // namespace autoupdater::updater
