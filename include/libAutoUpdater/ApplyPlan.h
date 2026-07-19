#pragma once

#include "libAutoUpdater/Result.h"
#include "libAutoUpdater/ResourceLimits.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace autoupdater {

/// Operation type executed by the external updater.
enum class ApplyOperationType { Replace, Remove };

/// High-level intent represented by an apply plan.
enum class ApplyPlanIntent { Install, Rollback };

/// Exact completed transaction referenced as the source of a rollback.
struct ApplyTransactionReference {
    std::string transactionId;
    std::string planDigest;
};

/// File state that must still be present immediately before an operation is
/// prepared. External rollback requests cannot provide this evidence; the
/// updater derives it from the completed forward transaction.
struct ApplyOperationPrecondition {
    bool exists = false;
    std::uint64_t size = 0;
    std::string sha256;
    std::optional<std::uint32_t> permissions;
};

/// One file replacement or removal operation in an apply plan.
struct ApplyOperation {
    ApplyOperation() = default;
    ApplyOperation(ApplyOperationType valueType, std::string valueSource, std::string valueTarget,
                   std::string valueSha256, std::uint64_t valueSize,
                   std::optional<std::uint32_t> valuePermissions = std::nullopt)
        : type(valueType), source(std::move(valueSource)), target(std::move(valueTarget)),
          sha256(std::move(valueSha256)), size(valueSize), permissions(valuePermissions) {}

    ApplyOperationType type = ApplyOperationType::Replace;
    std::string source;
    std::string target;
    std::string sha256;
    std::uint64_t size = 0;
    /// Desired portable rwx permission bits when the transaction must restore
    /// exact prior permissions. Forward install plans normally leave this unset.
    std::optional<std::uint32_t> permissions;
    /// Optional current-state binding used by internally derived rollback
    /// operations to close the validation-to-prepare race.
    std::optional<ApplyOperationPrecondition> precondition;
};

/// Contract file written by the library and consumed by autoupdater_apply.
struct ApplyPlan {
    int schemaVersion = 2;
    ApplyPlanIntent intent = ApplyPlanIntent::Install;
    std::optional<ApplyTransactionReference> rollbackOf;
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
    static Result<ApplyPlan> parse(const std::string& jsonText, const ResourceLimits& limits) noexcept;
    std::string toJson() const;
};

} // namespace autoupdater
