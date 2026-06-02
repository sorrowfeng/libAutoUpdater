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
    std::vector<std::string> arguments;
    std::filesystem::path workingDirectory;
    bool detached = true;
};

/// Process launcher abstraction for platform-specific process creation.
class IProcessLauncher {
  public:
    virtual ~IProcessLauncher() = default;

    virtual Result<void> launch(const ProcessLaunchRequest& request) noexcept = 0;
};

std::shared_ptr<IProcessLauncher> createDefaultProcessLauncher();

} // namespace autoupdater
