#pragma once

#include "libAutoUpdater/ApplyPlan.h"
#include "libAutoUpdater/ResourceLimits.h"
#include "libAutoUpdater/Result.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace autoupdater {
class IFileSystem;
class IHashProvider;
class IProcessLauncher;
} // namespace autoupdater

namespace autoupdater::updater {

enum class ApplyFaultAction { Continue, Fail };

struct ApplyExecutionHooks {
    std::function<ApplyFaultAction(std::string_view name, std::size_t operationIndex)> checkpoint;
};

struct ApplyExecutorDependencies {
    std::shared_ptr<IFileSystem> fileSystem;
    std::shared_ptr<IHashProvider> hashProvider;
    std::shared_ptr<IProcessLauncher> processLauncher;
    ResourceLimits limits;
};

Result<void> waitForProcessExit(std::uint64_t pid, std::chrono::seconds timeout) noexcept;
Result<void> executeApplyPlan(const ApplyPlan& plan) noexcept;
Result<void> executeApplyPlanWithDependencies(const ApplyPlan& plan, ApplyExecutorDependencies dependencies,
                                              const ApplyExecutionHooks& hooks = {}) noexcept;

} // namespace autoupdater::updater
