#pragma once

#include "ApplyTransactionReceipt.h"
#include "libAutoUpdater/ApplyPlan.h"
#include "libAutoUpdater/ResourceLimits.h"
#include "libAutoUpdater/Result.h"
#include "libAutoUpdater/interfaces/IRootedFileSystem.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace autoupdater::updater {

inline constexpr std::uint64_t kMaxJournalRecordBytes = 256 * 1024;

struct JournalError {
    std::string code;
    std::string message;

    bool empty() const noexcept {
        return code.empty() && message.empty();
    }
};

enum class JournalFileState {
    Prepared,
    Applying,
    RollingBack,
    RolledBack,
    FilesApplied,
    Complete,
    RecoveryFailed,
};

enum class JournalRestartState {
    NotRequested,
    NotAttempted,
    Intent,
    Launched,
    Failed,
    OutcomeUnknown,
};

enum class JournalBackupState { Pending, Intent, Durable, NotRequired };
enum class JournalApplyState { Pending, Intent, Complete };
enum class JournalRollbackState { NotStarted, Intent, Complete, Failed, NotRequired };

using ActiveTransaction = ApplyTransactionReceipt;

struct ApplyJournalSummary {
    int schemaVersion = 2;
    std::string transactionId;
    std::string planDigest;
    JournalFileState fileState = JournalFileState::Prepared;
    std::size_t operationCount = 0;
    JournalError applyError;
    JournalError rollbackError;
    JournalRestartState restartState = JournalRestartState::NotAttempted;
    JournalError restartError;
};

struct ApplyJournalOperation {
    int schemaVersion = 2;
    std::string transactionId;
    std::size_t index = 0;
    std::string operationId;
    std::string intent = "none";
    bool originalExists = false;
    std::string originalIdentity;
    std::uint64_t originalSize = 0;
    std::string originalSha256;
    bool originalPermissionsKnown = false;
    std::uint32_t originalPermissions = 0;
    JournalBackupState backupState = JournalBackupState::Pending;
    JournalApplyState applyState = JournalApplyState::Pending;
    std::string installedIdentity;
    JournalRollbackState rollbackState = JournalRollbackState::NotStarted;
    JournalError error;
};

std::string journalFileStateName(JournalFileState state);
std::string journalRestartStateName(JournalRestartState state);
std::string journalBackupStateName(JournalBackupState state);
std::string journalApplyStateName(JournalApplyState state);
std::string journalRollbackStateName(JournalRollbackState state);

Result<std::string> applyPlanDigest(const ApplyPlan& plan) noexcept;
Result<std::string> createApplyTransactionId(const std::string& planDigest) noexcept;
Result<std::string> applyOperationId(const std::string& transactionId, std::size_t index,
                                     const ApplyOperation& operation) noexcept;

Result<std::string> serializeActiveTransaction(const ActiveTransaction& active) noexcept;
Result<ActiveTransaction> parseActiveTransaction(const std::string& text) noexcept;
Result<std::string> serializeApplyJournalSummary(const ApplyJournalSummary& summary) noexcept;
Result<ApplyJournalSummary> parseApplyJournalSummary(const std::string& text) noexcept;
Result<std::string> serializeApplyJournalOperation(const ApplyJournalOperation& operation) noexcept;
Result<ApplyJournalOperation> parseApplyJournalOperation(const std::string& text) noexcept;

class ApplyJournalStore {
  public:
    explicit ApplyJournalStore(IRootedDirectory& installRoot, ResourceLimits limits = {});

    Result<std::optional<ActiveTransaction>> loadActive() noexcept;
    Result<void> writeActive(const ActiveTransaction& active) noexcept;
    Result<void> clearActive(const std::string& expectedTransactionId) noexcept;
    Result<std::optional<ActiveTransaction>> loadTerminal() noexcept;
    Result<void> writeTerminal(const ActiveTransaction& terminal) noexcept;

    Result<void> writePlanSnapshot(const std::string& transactionId, const std::string& planDigest,
                                   const std::string& planJson) noexcept;
    Result<std::string> readPlanSnapshot(const std::string& transactionId,
                                         const std::string& expectedPlanDigest) noexcept;

    Result<void> writeSummary(const ApplyJournalSummary& summary) noexcept;
    Result<ApplyJournalSummary> readSummary(const std::string& transactionId) noexcept;

    Result<void> writeOperation(const ApplyJournalOperation& operation) noexcept;
    Result<std::optional<ApplyJournalOperation>> readOperation(const std::string& transactionId,
                                                               std::size_t index) noexcept;

    static std::string activePath();
    static std::string terminalPath();
    static std::string summaryPath(const std::string& transactionId);
    static std::string planPath(const std::string& transactionId);
    static std::string operationPath(const std::string& transactionId, std::size_t index);

  private:
    Result<std::optional<std::string>> readOptional(const std::string& path, std::uint64_t maxBytes) noexcept;
    Result<void> writeAtomic(const std::string& path, const std::string& contents, std::uint64_t maxBytes) noexcept;

    IRootedDirectory& installRoot_;
    ResourceLimits limits_;
};

} // namespace autoupdater::updater
