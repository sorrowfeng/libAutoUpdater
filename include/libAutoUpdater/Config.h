#pragma once

#include "libAutoUpdater/Types.h"
#include "libAutoUpdater/Version.h"

#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

namespace autoupdater {

struct RetryOptions {
    int maxRetries = 3;
    std::chrono::milliseconds retryDelay{500};
};

struct NetworkOptions {
    std::chrono::milliseconds connectTimeout{10000};
    std::chrono::milliseconds transferTimeout{60000};
    bool verifyTls = true;
    bool enableResume = true;
};

struct SecurityOptions {
    bool requireManifestSignature = false;
    std::string manifestSignatureUrl;
    std::string publicKeyPem;
    std::vector<std::string> allowedBaseUrls;
    bool rejectExpiredManifest = true;
    bool rejectDowngrade = true;
};

struct Config {
    std::string appId;
    std::string channel = "stable";
    std::string platform;
    std::string arch;
    std::string manifestUrl;
    Version currentVersion;
    Version clientVersion{0, 1, 0};

    std::filesystem::path installDir;
    std::filesystem::path tempDir;
    std::filesystem::path updaterExecutable;
    std::vector<std::string> restartCommand;

    InstallLayout installLayout = InstallLayout::PortableDirectory;

    NetworkOptions network;
    RetryOptions retry;
    SecurityOptions security;

    std::vector<std::string> managedPathWhitelist;
    std::chrono::seconds applyWaitTimeout{60};
    std::chrono::seconds healthConfirmationTimeout{120};
};

} // namespace autoupdater

