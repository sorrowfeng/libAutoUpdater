#pragma once

#include "libAutoUpdater/ApplyPlan.h"
#include "libAutoUpdater/Config.h"
#include "libAutoUpdater/Manifest.h"
#include "libAutoUpdater/Result.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace autoupdater {

struct LocalFileInfo {
    std::string path;
    bool exists = false;
    std::string sha256;
    std::uint64_t size = 0;
};

struct LocalSnapshot {
    std::vector<LocalFileInfo> files;

    const LocalFileInfo* find(const std::string& path) const noexcept {
        for (const auto& file : files) {
            if (file.path == path) {
                return &file;
            }
        }
        return nullptr;
    }
};

struct PlannedDownload {
    ManifestFile file;
    std::string url;
    std::filesystem::path stagingPath;
};

struct UpdateDecision {
    CheckResult checkResult;
    std::vector<PlannedDownload> downloads;
    std::vector<ApplyOperation> operations;
};

struct ManifestEnvelope {
    Manifest manifest;
    std::string rawBytes;
    std::string sha256;
};

} // namespace autoupdater

