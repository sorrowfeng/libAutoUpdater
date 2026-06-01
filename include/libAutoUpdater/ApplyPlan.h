#pragma once

#include "libAutoUpdater/Result.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace autoupdater {

enum class ApplyOperationType {
    Replace,
    Remove
};

struct ApplyOperation {
    ApplyOperationType type = ApplyOperationType::Replace;
    std::string source;
    std::string target;
    std::string sha256;
    std::uint64_t size = 0;
};

struct ApplyPlan {
    int schemaVersion = 1;
    std::string appId;
    std::string fromVersion;
    std::string toVersion;
    std::string releaseId;
    std::string manifestSha256;
    std::filesystem::path installDir;
    std::filesystem::path stagingDir;
    std::filesystem::path backupDir;
    std::vector<std::string> restartCommand;
    std::vector<ApplyOperation> operations;

    static Result<ApplyPlan> parse(const std::string& jsonText) noexcept;
    std::string toJson() const;
};

} // namespace autoupdater

