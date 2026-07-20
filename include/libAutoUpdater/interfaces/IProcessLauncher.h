#pragma once

#include "libAutoUpdater/Result.h"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace autoupdater {

/// Process launch request used to start the external updater.
struct ProcessLaunchRequest {
    std::filesystem::path executable;
    /// UTF-8 arguments passed directly to the child without shell interpretation.
    std::vector<std::string> arguments;
    std::filesystem::path workingDirectory;
    bool detached = true;
};

/// Process launcher abstraction for platform-specific process creation.
/// Implementations must not copy executable arguments, environment secrets, or
/// credential-bearing URLs into Error::message.
class IProcessLauncher {
  public:
    virtual ~IProcessLauncher() = default;

    /// Returns after the operating system has created the requested executable image, not after it exits.
    virtual Result<void> launch(const ProcessLaunchRequest& request) noexcept = 0;
};

std::shared_ptr<IProcessLauncher> createDefaultProcessLauncher();

} // namespace autoupdater
