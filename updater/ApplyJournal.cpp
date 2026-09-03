#include "ApplyJournal.h"

#include "util/Json.h"
#include "util/Rfc3339.h"
#include "util/Sha256.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <iomanip>
#include <initializer_list>
#include <limits>
#include <random>
#include <sstream>
#include <string_view>

namespace autoupdater::updater {

namespace {

constexpr std::uint64_t kLegacySummarySchemaVersion = 2;
constexpr std::uint64_t kSummarySchemaVersion = 3;
constexpr std::uint64_t kOperationSchemaVersion = 2;
constexpr std::size_t kReadChunkBytes = 64 * 1024;

Error journalError(const std::string& message) {
    return {ErrorCode::ApplyFailed, message};
}

bool isLowerHexDigest(const std::string& value) {
    return util::isLowerHexSha256(value);
}

Result<void> validateTransaction(const std::string& transactionId, const std::string& planDigest) {
    if (!isLowerHexDigest(transactionId) || !isLowerHexDigest(planDigest)) {
        return Result<void>::fail(journalError("Invalid apply journal transaction or plan digest"));
    }
    return Result<void>::ok();
}

JsonResourceLimits journalJsonLimits() {
    JsonResourceLimits limits;
    limits.maxDepth = 16;
    limits.maxNodes = 256;
    limits.maxStringBytes = static_cast<std::size_t>(kMaxJournalRecordBytes);
    limits.maxNumberBytes = 32;
    limits.maxContainerEntries = 64;
    return limits;
}

Result<util::Json> parseObject(const std::string& text, const std::string& description,
                               std::initializer_list<std::uint64_t> supportedSchemas) {
    if (text.size() > kMaxJournalRecordBytes) {
        return Result<util::Json>::fail({ErrorCode::ResourceLimitExceeded, description + " exceeds its byte limit"});
    }
    auto parsed = util::Json::parse(text, journalJsonLimits());
    if (!parsed) {
        return parsed;
    }
    if (!parsed.value().isObject()) {
        return Result<util::Json>::fail(journalError(description + " must be a JSON object"));
    }
    const auto* schema = parsed.value().get("schemaVersion");
    if (!schema || !schema->isUnsignedInteger() ||
        std::find(supportedSchemas.begin(), supportedSchemas.end(), schema->asUInt64()) == supportedSchemas.end()) {
        return Result<util::Json>::fail(journalError("Unsupported " + description + " schema version"));
    }
    return parsed;
}

Result<void> requireOnlyKeys(const util::Json::Object& object, std::initializer_list<const char*> allowedKeys,
                             const std::string& description) {
    for (const auto& entry : object) {
        const auto allowed =
            std::any_of(allowedKeys.begin(), allowedKeys.end(), [&](const char* key) { return entry.first == key; });
        if (!allowed) {
            return Result<void>::fail(journalError(description + " contains unknown field '" + entry.first + "'"));
        }
    }
    return Result<void>::ok();
}

Result<std::string> requiredString(const util::Json& object, const std::string& key) {
    const auto* value = object.get(key);
    if (!value || !value->isString()) {
        return Result<std::string>::fail(journalError("Apply journal string field is missing: " + key));
    }
    return Result<std::string>::ok(value->asString());
}

Result<bool> requiredBool(const util::Json& object, const std::string& key) {
    const auto* value = object.get(key);
    if (!value || !value->isBool()) {
        return Result<bool>::fail(journalError("Apply journal boolean field is missing: " + key));
    }
    return Result<bool>::ok(value->asBool());
}

Result<std::uint64_t> parseDecimal(const util::Json& object, const std::string& key) {
    auto text = requiredString(object, key);
    if (!text) {
        return Result<std::uint64_t>::fail(text.error());
    }
    if (text.value().empty()) {
        return Result<std::uint64_t>::fail(journalError("Apply journal integer field is empty: " + key));
    }
    if (text.value().size() > 1 && text.value().front() == '0') {
        return Result<std::uint64_t>::fail(journalError("Apply journal integer field is not canonical: " + key));
    }
    std::uint64_t result = 0;
    for (const unsigned char character : text.value()) {
        if (character < '0' || character > '9') {
            return Result<std::uint64_t>::fail(journalError("Apply journal integer field is invalid: " + key));
        }
        const auto digit = static_cast<std::uint64_t>(character - '0');
        if (result > (std::numeric_limits<std::uint64_t>::max() - digit) / 10) {
            return Result<std::uint64_t>::fail(journalError("Apply journal integer field overflows: " + key));
        }
        result = result * 10 + digit;
    }
    return Result<std::uint64_t>::ok(result);
}

util::Json errorToJson(const JournalError& error) {
    util::Json::Object object;
    object.emplace("code", error.code);
    object.emplace("message", error.message);
    return util::Json(std::move(object));
}

Result<JournalError> errorFromJson(const util::Json& object, const std::string& key) {
    const auto* value = object.get(key);
    if (!value || !value->isObject()) {
        return Result<JournalError>::fail(journalError("Apply journal error field is missing: " + key));
    }
    auto keys = requireOnlyKeys(value->asObject(), {"code", "message"}, "Apply journal error field " + key);
    if (!keys) {
        return Result<JournalError>::fail(keys.error());
    }
    auto code = requiredString(*value, "code");
    auto message = requiredString(*value, "message");
    if (!code) {
        return Result<JournalError>::fail(code.error());
    }
    if (!message) {
        return Result<JournalError>::fail(message.error());
    }
    if (code.value().empty() != message.value().empty()) {
        return Result<JournalError>::fail(journalError("Apply journal error field is incomplete: " + key));
    }
    return Result<JournalError>::ok({code.value(), message.value()});
}

template <typename T> Result<T> enumFailure(const std::string& field) {
    return Result<T>::fail(journalError("Apply journal enum field is invalid: " + field));
}

Result<JournalFileState> fileStateFromName(const std::string& value) {
    if (value == "prepared")
        return Result<JournalFileState>::ok(JournalFileState::Prepared);
    if (value == "applying")
        return Result<JournalFileState>::ok(JournalFileState::Applying);
    if (value == "rolling_back")
        return Result<JournalFileState>::ok(JournalFileState::RollingBack);
    if (value == "rolled_back")
        return Result<JournalFileState>::ok(JournalFileState::RolledBack);
    if (value == "files_applied")
        return Result<JournalFileState>::ok(JournalFileState::FilesApplied);
    if (value == "complete")
        return Result<JournalFileState>::ok(JournalFileState::Complete);
    if (value == "recovery_failed")
        return Result<JournalFileState>::ok(JournalFileState::RecoveryFailed);
    return enumFailure<JournalFileState>("fileState");
}

Result<JournalRestartState> restartStateFromName(const std::string& value) {
    if (value == "not_requested")
        return Result<JournalRestartState>::ok(JournalRestartState::NotRequested);
    if (value == "not_attempted")
        return Result<JournalRestartState>::ok(JournalRestartState::NotAttempted);
    if (value == "intent")
        return Result<JournalRestartState>::ok(JournalRestartState::Intent);
    if (value == "launched")
        return Result<JournalRestartState>::ok(JournalRestartState::Launched);
    if (value == "failed")
        return Result<JournalRestartState>::ok(JournalRestartState::Failed);
    if (value == "outcome_unknown")
        return Result<JournalRestartState>::ok(JournalRestartState::OutcomeUnknown);
    return enumFailure<JournalRestartState>("restartState");
}

Result<JournalBackupState> backupStateFromName(const std::string& value) {
    if (value == "pending")
        return Result<JournalBackupState>::ok(JournalBackupState::Pending);
    if (value == "intent")
        return Result<JournalBackupState>::ok(JournalBackupState::Intent);
    if (value == "durable")
        return Result<JournalBackupState>::ok(JournalBackupState::Durable);
    if (value == "not_required")
        return Result<JournalBackupState>::ok(JournalBackupState::NotRequired);
    return enumFailure<JournalBackupState>("backupState");
}

Result<JournalApplyState> applyStateFromName(const std::string& value) {
    if (value == "pending")
        return Result<JournalApplyState>::ok(JournalApplyState::Pending);
    if (value == "intent")
        return Result<JournalApplyState>::ok(JournalApplyState::Intent);
    if (value == "complete")
        return Result<JournalApplyState>::ok(JournalApplyState::Complete);
    return enumFailure<JournalApplyState>("applyState");
}

Result<JournalRollbackState> rollbackStateFromName(const std::string& value) {
    if (value == "not_started")
        return Result<JournalRollbackState>::ok(JournalRollbackState::NotStarted);
    if (value == "intent")
        return Result<JournalRollbackState>::ok(JournalRollbackState::Intent);
    if (value == "complete")
        return Result<JournalRollbackState>::ok(JournalRollbackState::Complete);
    if (value == "failed")
        return Result<JournalRollbackState>::ok(JournalRollbackState::Failed);
    if (value == "not_required")
        return Result<JournalRollbackState>::ok(JournalRollbackState::NotRequired);
    return enumFailure<JournalRollbackState>("rollbackState");
}

Result<void> validateOperationRecord(const ApplyJournalOperation& operation) {
    if (!isLowerHexDigest(operation.transactionId))
        return Result<void>::fail(journalError("Invalid apply journal transaction identity"));
    if (!isLowerHexDigest(operation.operationId)) {
        return Result<void>::fail(journalError("Invalid apply journal operation digest"));
    }
    if (operation.intent != "replace" && operation.intent != "remove") {
        return Result<void>::fail(journalError("Invalid apply journal operation intent"));
    }
    if (journalBackupStateName(operation.backupState) == "invalid" ||
        journalApplyStateName(operation.applyState) == "invalid" ||
        journalRollbackStateName(operation.rollbackState) == "invalid") {
        return Result<void>::fail(journalError("Invalid apply journal operation state"));
    }
    if (operation.originalExists) {
        if (operation.originalIdentity.empty() || !isLowerHexDigest(operation.originalSha256)) {
            return Result<void>::fail(journalError("Original file evidence is incomplete in apply journal"));
        }
        if (!operation.originalPermissionsKnown && operation.originalPermissions != 0) {
            return Result<void>::fail(journalError("Unknown original permissions have a non-zero journal value"));
        }
        if ((operation.originalPermissions & ~static_cast<std::uint32_t>(0777)) != 0) {
            return Result<void>::fail(journalError("Original permissions contain unsupported journal bits"));
        }
    } else if (!operation.originalIdentity.empty() || operation.originalSize != 0 ||
               !operation.originalSha256.empty() || operation.originalPermissionsKnown ||
               operation.originalPermissions != 0) {
        return Result<void>::fail(journalError("Unexpected original file evidence in apply journal"));
    }
    if (operation.error.code.empty() != operation.error.message.empty()) {
        return Result<void>::fail(journalError("Apply journal operation error is incomplete"));
    }
    return Result<void>::ok();
}

Result<void> validateSummaryRecord(const ApplyJournalSummary& summary, bool allowLegacyCompleteWithoutTimestamp) {
    auto transaction = validateTransaction(summary.transactionId, summary.planDigest);
    if (!transaction) {
        return transaction;
    }
    if (journalFileStateName(summary.fileState) == "invalid" ||
        journalRestartStateName(summary.restartState) == "invalid") {
        return Result<void>::fail(journalError("Invalid apply journal summary state"));
    }
    const auto completeError = [](const JournalError& error) { return error.code.empty() == error.message.empty(); };
    if (!completeError(summary.applyError) || !completeError(summary.rollbackError) ||
        !completeError(summary.restartError)) {
        return Result<void>::fail(journalError("Apply journal summary contains an incomplete error"));
    }
    if (summary.completedAt) {
        if (summary.fileState != JournalFileState::Complete) {
            return Result<void>::fail(journalError("Apply journal completion timestamp is present before completion"));
        }
        auto formatted = autoupdater::detail::formatApplyCompletionTimestamp(*summary.completedAt);
        if (!formatted) {
            return Result<void>::fail(journalError("Invalid apply journal completion timestamp"));
        }
    } else if (summary.fileState == JournalFileState::Complete && !allowLegacyCompleteWithoutTimestamp) {
        return Result<void>::fail(journalError("Completed apply journal summary is missing completedAt"));
    }
    return Result<void>::ok();
}

} // namespace

std::string journalFileStateName(JournalFileState state) {
    switch (state) {
    case JournalFileState::Prepared:
        return "prepared";
    case JournalFileState::Applying:
        return "applying";
    case JournalFileState::RollingBack:
        return "rolling_back";
    case JournalFileState::RolledBack:
        return "rolled_back";
    case JournalFileState::FilesApplied:
        return "files_applied";
    case JournalFileState::Complete:
        return "complete";
    case JournalFileState::RecoveryFailed:
        return "recovery_failed";
    }
    return "invalid";
}

std::string journalRestartStateName(JournalRestartState state) {
    switch (state) {
    case JournalRestartState::NotRequested:
        return "not_requested";
    case JournalRestartState::NotAttempted:
        return "not_attempted";
    case JournalRestartState::Intent:
        return "intent";
    case JournalRestartState::Launched:
        return "launched";
    case JournalRestartState::Failed:
        return "failed";
    case JournalRestartState::OutcomeUnknown:
        return "outcome_unknown";
    }
    return "invalid";
}

std::string journalBackupStateName(JournalBackupState state) {
    switch (state) {
    case JournalBackupState::Pending:
        return "pending";
    case JournalBackupState::Intent:
        return "intent";
    case JournalBackupState::Durable:
        return "durable";
    case JournalBackupState::NotRequired:
        return "not_required";
    }
    return "invalid";
}

std::string journalApplyStateName(JournalApplyState state) {
    switch (state) {
    case JournalApplyState::Pending:
        return "pending";
    case JournalApplyState::Intent:
        return "intent";
    case JournalApplyState::Complete:
        return "complete";
    }
    return "invalid";
}

std::string journalRollbackStateName(JournalRollbackState state) {
    switch (state) {
    case JournalRollbackState::NotStarted:
        return "not_started";
    case JournalRollbackState::Intent:
        return "intent";
    case JournalRollbackState::Complete:
        return "complete";
    case JournalRollbackState::Failed:
        return "failed";
    case JournalRollbackState::NotRequired:
        return "not_required";
    }
    return "invalid";
}

Result<std::string> applyPlanDigest(const ApplyPlan& plan) noexcept {
    try {
        return Result<std::string>::ok(util::sha256Bytes(plan.toJson()));
    } catch (...) {
        return Result<std::string>::fail(journalError("Failed to calculate apply transaction identity"));
    }
}

Result<std::string> createApplyTransactionId(const std::string& planDigest) noexcept {
    try {
        if (!isLowerHexDigest(planDigest)) {
            return Result<std::string>::fail(journalError("Invalid plan digest for apply transaction"));
        }
        static std::atomic<std::uint64_t> sequence{0};
        std::random_device random;
        std::ostringstream material;
        material << planDigest << ':' << std::chrono::system_clock::now().time_since_epoch().count() << ':'
                 << std::chrono::steady_clock::now().time_since_epoch().count() << ':'
                 << sequence.fetch_add(1, std::memory_order_relaxed);
        for (int index = 0; index < 4; ++index) {
            material << ':' << random();
        }
        return Result<std::string>::ok(util::sha256Bytes(material.str()));
    } catch (...) {
        return Result<std::string>::fail(journalError("Failed to create a unique apply transaction identity"));
    }
}

Result<std::string> applyOperationId(const std::string& transactionId, std::size_t index,
                                     const ApplyOperation& operation) noexcept {
    try {
        if (!isLowerHexDigest(transactionId)) {
            return Result<std::string>::fail(journalError("Invalid transaction identity for apply operation"));
        }
        std::string material = transactionId;
        const auto append = [&material](std::string_view value) {
            material.push_back('\0');
            material.append(value.data(), value.size());
        };
        append(std::to_string(index));
        append(operation.type == ApplyOperationType::Replace ? "replace" : "remove");
        append(operation.source);
        append(operation.target);
        append(operation.sha256);
        append(std::to_string(operation.size));
        // Preserve the schema-v1 identity material exactly. Schema-v2-only
        // fields extend the identity only when they are present.
        if (operation.permissions) {
            append("permissions");
            append(std::to_string(*operation.permissions));
        }
        if (operation.precondition) {
            append("precondition");
            append(operation.precondition->exists ? "exists" : "missing");
            if (operation.precondition->exists) {
                append(std::to_string(operation.precondition->size));
                append(operation.precondition->sha256);
                if (operation.precondition->permissions) {
                    append(std::to_string(*operation.precondition->permissions));
                } else {
                    append("permissions-unknown");
                }
            }
        }
        return Result<std::string>::ok(util::sha256Bytes(material));
    } catch (...) {
        return Result<std::string>::fail(journalError("Failed to calculate apply operation identity"));
    }
}

Result<std::string> serializeActiveTransaction(const ActiveTransaction& active) noexcept {
    return serializeApplyTransactionReceipt(active);
}

Result<ActiveTransaction> parseActiveTransaction(const std::string& text) noexcept {
    return parseApplyTransactionReceipt(text);
}

Result<std::string> serializeApplyJournalSummary(const ApplyJournalSummary& summary) noexcept {
    try {
        auto valid = validateSummaryRecord(summary, false);
        if (!valid)
            return Result<std::string>::fail(valid.error());
        util::Json::Object object;
        object.emplace("schemaVersion", summary.completedAt ? kSummarySchemaVersion : kLegacySummarySchemaVersion);
        object.emplace("transactionId", summary.transactionId);
        object.emplace("planDigest", summary.planDigest);
        object.emplace("fileState", journalFileStateName(summary.fileState));
        if (summary.completedAt) {
            auto timestamp = autoupdater::detail::formatApplyCompletionTimestamp(*summary.completedAt);
            if (!timestamp) {
                return Result<std::string>::fail(timestamp.error());
            }
            object.emplace("completedAt", std::move(timestamp.value()));
        }
        object.emplace("operationCount", std::to_string(summary.operationCount));
        object.emplace("applyError", errorToJson(summary.applyError));
        object.emplace("rollbackError", errorToJson(summary.rollbackError));
        object.emplace("restartState", journalRestartStateName(summary.restartState));
        object.emplace("restartError", errorToJson(summary.restartError));
        return Result<std::string>::ok(util::Json(std::move(object)).stringify(2));
    } catch (...) {
        return Result<std::string>::fail(journalError("Failed to serialize apply journal summary"));
    }
}

Result<ApplyJournalSummary> parseApplyJournalSummary(const std::string& text) noexcept {
    try {
        auto object = parseObject(text, "apply journal summary", {kLegacySummarySchemaVersion, kSummarySchemaVersion});
        if (!object)
            return Result<ApplyJournalSummary>::fail(object.error());
        const auto schemaVersion = object.value().get("schemaVersion")->asUInt64();
        auto keys =
            schemaVersion == kLegacySummarySchemaVersion
                ? requireOnlyKeys(object.value().asObject(),
                                  {"schemaVersion", "transactionId", "planDigest", "fileState", "operationCount",
                                   "applyError", "rollbackError", "restartState", "restartError"},
                                  "Apply journal summary")
                : requireOnlyKeys(object.value().asObject(),
                                  {"schemaVersion", "transactionId", "planDigest", "fileState", "completedAt",
                                   "operationCount", "applyError", "rollbackError", "restartState", "restartError"},
                                  "Apply journal summary");
        if (!keys)
            return Result<ApplyJournalSummary>::fail(keys.error());
        auto transactionId = requiredString(object.value(), "transactionId");
        auto planDigest = requiredString(object.value(), "planDigest");
        auto fileStateText = requiredString(object.value(), "fileState");
        auto operationCount = parseDecimal(object.value(), "operationCount");
        auto applyError = errorFromJson(object.value(), "applyError");
        auto rollbackError = errorFromJson(object.value(), "rollbackError");
        auto restartStateText = requiredString(object.value(), "restartState");
        auto restartError = errorFromJson(object.value(), "restartError");
        if (!transactionId)
            return Result<ApplyJournalSummary>::fail(transactionId.error());
        if (!planDigest)
            return Result<ApplyJournalSummary>::fail(planDigest.error());
        if (!fileStateText)
            return Result<ApplyJournalSummary>::fail(fileStateText.error());
        if (!operationCount)
            return Result<ApplyJournalSummary>::fail(operationCount.error());
        if (!applyError)
            return Result<ApplyJournalSummary>::fail(applyError.error());
        if (!rollbackError)
            return Result<ApplyJournalSummary>::fail(rollbackError.error());
        if (!restartStateText)
            return Result<ApplyJournalSummary>::fail(restartStateText.error());
        if (!restartError)
            return Result<ApplyJournalSummary>::fail(restartError.error());
        auto valid = validateTransaction(transactionId.value(), planDigest.value());
        if (!valid)
            return Result<ApplyJournalSummary>::fail(valid.error());
        if (operationCount.value() > std::numeric_limits<std::size_t>::max()) {
            return Result<ApplyJournalSummary>::fail(journalError("Apply journal operation count is too large"));
        }
        auto fileState = fileStateFromName(fileStateText.value());
        auto restartState = restartStateFromName(restartStateText.value());
        if (!fileState)
            return Result<ApplyJournalSummary>::fail(fileState.error());
        if (!restartState)
            return Result<ApplyJournalSummary>::fail(restartState.error());
        std::optional<util::UtcInstant> completedAt;
        if (const auto* timestamp = object.value().get("completedAt")) {
            if (!timestamp->isString()) {
                return Result<ApplyJournalSummary>::fail(
                    journalError("Apply journal completion timestamp must be a string"));
            }
            auto instant = util::parseRfc3339(timestamp->asString());
            if (!instant || !autoupdater::detail::formatApplyCompletionTimestamp(instant.value())) {
                return Result<ApplyJournalSummary>::fail(journalError("Invalid apply journal completion timestamp"));
            }
            completedAt = instant.value();
        } else if (schemaVersion == kSummarySchemaVersion) {
            return Result<ApplyJournalSummary>::fail(
                journalError("Apply journal schemaVersion 3 summary is missing completedAt"));
        }
        ApplyJournalSummary summary;
        summary.schemaVersion = static_cast<int>(schemaVersion);
        summary.transactionId = transactionId.value();
        summary.planDigest = planDigest.value();
        summary.fileState = fileState.value();
        summary.completedAt = std::move(completedAt);
        summary.operationCount = static_cast<std::size_t>(operationCount.value());
        summary.applyError = applyError.value();
        summary.rollbackError = rollbackError.value();
        summary.restartState = restartState.value();
        summary.restartError = restartError.value();
        auto validSummary = validateSummaryRecord(summary, schemaVersion == kLegacySummarySchemaVersion);
        if (!validSummary) {
            return Result<ApplyJournalSummary>::fail(validSummary.error());
        }
        return Result<ApplyJournalSummary>::ok(std::move(summary));
    } catch (...) {
        return Result<ApplyJournalSummary>::fail(journalError("Failed to parse apply journal summary"));
    }
}

Result<std::string> serializeApplyJournalOperation(const ApplyJournalOperation& operation) noexcept {
    try {
        auto valid = validateOperationRecord(operation);
        if (!valid)
            return Result<std::string>::fail(valid.error());
        util::Json::Object object;
        object.emplace("schemaVersion", kOperationSchemaVersion);
        object.emplace("transactionId", operation.transactionId);
        object.emplace("index", std::to_string(operation.index));
        object.emplace("operationId", operation.operationId);
        object.emplace("intent", operation.intent);
        object.emplace("originalExists", operation.originalExists);
        object.emplace("originalIdentity", operation.originalIdentity);
        object.emplace("originalSize", std::to_string(operation.originalSize));
        object.emplace("originalSha256", operation.originalSha256);
        object.emplace("originalPermissionsKnown", operation.originalPermissionsKnown);
        object.emplace("originalPermissions", std::to_string(operation.originalPermissions));
        object.emplace("backupState", journalBackupStateName(operation.backupState));
        object.emplace("applyState", journalApplyStateName(operation.applyState));
        object.emplace("installedIdentity", operation.installedIdentity);
        object.emplace("rollbackState", journalRollbackStateName(operation.rollbackState));
        object.emplace("error", errorToJson(operation.error));
        return Result<std::string>::ok(util::Json(std::move(object)).stringify(2));
    } catch (...) {
        return Result<std::string>::fail(journalError("Failed to serialize apply journal operation"));
    }
}

Result<ApplyJournalOperation> parseApplyJournalOperation(const std::string& text) noexcept {
    try {
        auto object = parseObject(text, "apply journal operation", {kOperationSchemaVersion});
        if (!object)
            return Result<ApplyJournalOperation>::fail(object.error());
        auto keys = requireOnlyKeys(object.value().asObject(),
                                    {"schemaVersion", "transactionId", "index", "operationId", "intent",
                                     "originalExists", "originalIdentity", "originalSize", "originalSha256",
                                     "originalPermissionsKnown", "originalPermissions", "backupState", "applyState",
                                     "installedIdentity", "rollbackState", "error"},
                                    "Apply journal operation");
        if (!keys)
            return Result<ApplyJournalOperation>::fail(keys.error());
        auto transactionId = requiredString(object.value(), "transactionId");
        auto index = parseDecimal(object.value(), "index");
        auto operationId = requiredString(object.value(), "operationId");
        auto intent = requiredString(object.value(), "intent");
        auto originalExists = requiredBool(object.value(), "originalExists");
        auto originalIdentity = requiredString(object.value(), "originalIdentity");
        auto originalSize = parseDecimal(object.value(), "originalSize");
        auto originalSha256 = requiredString(object.value(), "originalSha256");
        auto originalPermissionsKnown = requiredBool(object.value(), "originalPermissionsKnown");
        auto originalPermissions = parseDecimal(object.value(), "originalPermissions");
        auto backupStateText = requiredString(object.value(), "backupState");
        auto applyStateText = requiredString(object.value(), "applyState");
        auto installedIdentity = requiredString(object.value(), "installedIdentity");
        auto rollbackStateText = requiredString(object.value(), "rollbackState");
        auto error = errorFromJson(object.value(), "error");
        if (!transactionId)
            return Result<ApplyJournalOperation>::fail(transactionId.error());
        if (!index)
            return Result<ApplyJournalOperation>::fail(index.error());
        if (!operationId)
            return Result<ApplyJournalOperation>::fail(operationId.error());
        if (!intent)
            return Result<ApplyJournalOperation>::fail(intent.error());
        if (!originalExists)
            return Result<ApplyJournalOperation>::fail(originalExists.error());
        if (!originalIdentity)
            return Result<ApplyJournalOperation>::fail(originalIdentity.error());
        if (!originalSize)
            return Result<ApplyJournalOperation>::fail(originalSize.error());
        if (!originalSha256)
            return Result<ApplyJournalOperation>::fail(originalSha256.error());
        if (!originalPermissionsKnown)
            return Result<ApplyJournalOperation>::fail(originalPermissionsKnown.error());
        if (!originalPermissions)
            return Result<ApplyJournalOperation>::fail(originalPermissions.error());
        if (!backupStateText)
            return Result<ApplyJournalOperation>::fail(backupStateText.error());
        if (!applyStateText)
            return Result<ApplyJournalOperation>::fail(applyStateText.error());
        if (!installedIdentity)
            return Result<ApplyJournalOperation>::fail(installedIdentity.error());
        if (!rollbackStateText)
            return Result<ApplyJournalOperation>::fail(rollbackStateText.error());
        if (!error)
            return Result<ApplyJournalOperation>::fail(error.error());
        if (index.value() > std::numeric_limits<std::size_t>::max()) {
            return Result<ApplyJournalOperation>::fail(journalError("Apply journal operation index is too large"));
        }
        if (originalPermissions.value() > std::numeric_limits<std::uint32_t>::max()) {
            return Result<ApplyJournalOperation>::fail(journalError("Apply journal permission value is too large"));
        }
        auto backupState = backupStateFromName(backupStateText.value());
        auto applyState = applyStateFromName(applyStateText.value());
        auto rollbackState = rollbackStateFromName(rollbackStateText.value());
        if (!backupState)
            return Result<ApplyJournalOperation>::fail(backupState.error());
        if (!applyState)
            return Result<ApplyJournalOperation>::fail(applyState.error());
        if (!rollbackState)
            return Result<ApplyJournalOperation>::fail(rollbackState.error());
        ApplyJournalOperation operation;
        operation.transactionId = transactionId.value();
        operation.index = static_cast<std::size_t>(index.value());
        operation.operationId = operationId.value();
        operation.intent = intent.value();
        operation.originalExists = originalExists.value();
        operation.originalIdentity = originalIdentity.value();
        operation.originalSize = originalSize.value();
        operation.originalSha256 = originalSha256.value();
        operation.originalPermissionsKnown = originalPermissionsKnown.value();
        operation.originalPermissions = static_cast<std::uint32_t>(originalPermissions.value());
        operation.backupState = backupState.value();
        operation.applyState = applyState.value();
        operation.installedIdentity = installedIdentity.value();
        operation.rollbackState = rollbackState.value();
        operation.error = error.value();
        auto valid = validateOperationRecord(operation);
        if (!valid)
            return Result<ApplyJournalOperation>::fail(valid.error());
        return Result<ApplyJournalOperation>::ok(std::move(operation));
    } catch (...) {
        return Result<ApplyJournalOperation>::fail(journalError("Failed to parse apply journal operation"));
    }
}

ApplyJournalStore::ApplyJournalStore(IRootedDirectory& installRoot, ResourceLimits limits)
    : installRoot_(installRoot), limits_(std::move(limits)) {}

std::string ApplyJournalStore::activePath() {
    return ".autoupdater/journal/active.json";
}

std::string ApplyJournalStore::terminalPath() {
    return ".autoupdater/journal/terminal.json";
}

std::string ApplyJournalStore::summaryPath(const std::string& transactionId) {
    return ".autoupdater/journal/" + transactionId + ".json";
}

std::string ApplyJournalStore::planPath(const std::string& transactionId) {
    return ".autoupdater/journal/" + transactionId + ".plan.json";
}

std::string ApplyJournalStore::operationPath(const std::string& transactionId, std::size_t index) {
    std::ostringstream stream;
    stream << ".autoupdater/journal/" << transactionId << ".ops/" << std::setfill('0') << std::setw(8) << index
           << ".json";
    return stream.str();
}

Result<std::optional<std::string>> ApplyJournalStore::readOptional(const std::string& path, std::uint64_t maxBytes,
                                                                   const std::string* expectedIdentity) noexcept {
    try {
        auto opened = installRoot_.openRegularFile(path, RootedFileOpenMode::ReadOnly);
        if (!opened)
            return Result<std::optional<std::string>>::fail(opened.error());
        if (!opened.value().exists())
            return Result<std::optional<std::string>>::ok(std::nullopt);
        const auto failOpened = [&](Error error) {
            auto closed = opened.value().file->close();
            if (!closed)
                error.message += "; failed to close apply journal file: " + closed.error().message;
            return Result<std::optional<std::string>>::fail(std::move(error));
        };
        auto metadata = opened.value().file->metadata();
        if (!metadata)
            return failOpened(metadata.error());
        if (expectedIdentity && metadata.value().identity != *expectedIdentity)
            return failOpened(
                {ErrorCode::SecurityPolicyViolation, "Apply journal publication identity changed: " + path});
        if (metadata.value().size > maxBytes || metadata.value().size > std::numeric_limits<std::size_t>::max()) {
            return failOpened({ErrorCode::ResourceLimitExceeded, "Apply journal file exceeds its byte limit: " + path});
        }
        auto seek = opened.value().file->seek(0);
        if (!seek)
            return failOpened(seek.error());
        std::string contents;
        contents.reserve(static_cast<std::size_t>(metadata.value().size));
        std::array<char, kReadChunkBytes> buffer{};
        while (true) {
            auto read = opened.value().file->read(buffer.data(), buffer.size());
            if (!read)
                return failOpened(read.error());
            if (read.value() == 0)
                break;
            if (contents.size() > maxBytes || read.value() > maxBytes - contents.size()) {
                return failOpened(
                    {ErrorCode::ResourceLimitExceeded, "Apply journal file grew beyond its byte limit: " + path});
            }
            contents.append(buffer.data(), read.value());
        }
        auto finalMetadata = opened.value().file->metadata();
        if (!finalMetadata)
            return failOpened(finalMetadata.error());
        if (finalMetadata.value().identity != metadata.value().identity ||
            finalMetadata.value().size != metadata.value().size || contents.size() != metadata.value().size)
            return failOpened(
                {ErrorCode::SecurityPolicyViolation, "Apply journal file changed while being read: " + path});
        auto closed = opened.value().file->close();
        if (!closed)
            return Result<std::optional<std::string>>::fail(closed.error());
        return Result<std::optional<std::string>>::ok(std::move(contents));
    } catch (...) {
        return Result<std::optional<std::string>>::fail(journalError("Failed to read apply journal file: " + path));
    }
}

Result<void> ApplyJournalStore::writeAtomic(const std::string& path, const std::string& contents,
                                            std::uint64_t maxBytes) noexcept {
    try {
        if (contents.size() > maxBytes) {
            return Result<void>::fail(
                {ErrorCode::ResourceLimitExceeded, "Apply journal record exceeds its byte limit"});
        }
        auto current = installRoot_.openRegularFile(path, RootedFileOpenMode::ReadOnly);
        if (!current)
            return Result<void>::fail(current.error());
        auto expectation = RootedEntryExpectation::missing();
        if (current.value().exists()) {
            auto metadata = current.value().file->metadata();
            if (!metadata) {
                auto error = metadata.error();
                auto closed = current.value().file->close();
                if (!closed)
                    error.message += "; failed to close apply journal target: " + closed.error().message;
                return Result<void>::fail(std::move(error));
            }
            expectation = RootedEntryExpectation::matching(metadata.value());
            auto closed = current.value().file->close();
            if (!closed)
                return closed;
        }
        auto temporary = installRoot_.createAtomicReplacement(path);
        if (!temporary)
            return Result<void>::fail(temporary.error());
        auto write = temporary.value()->file().write(contents.data(), contents.size());
        if (!write) {
            auto error = write.error();
            auto discarded = temporary.value()->discard();
            if (!discarded)
                error.message += "; failed to discard incomplete apply journal: " + discarded.error().message;
            return Result<void>::fail(std::move(error));
        }
        auto preparedMetadata = temporary.value()->file().metadata();
        if (!preparedMetadata) {
            auto error = preparedMetadata.error();
            auto discarded = temporary.value()->discard();
            if (!discarded)
                error.message += "; failed to discard invalid apply journal: " + discarded.error().message;
            return Result<void>::fail(std::move(error));
        }
        auto commit = temporary.value()->commit(expectation);
        auto discarded = temporary.value()->discard();
        const auto publication = temporary.value()->publishStatus();
        if (!commit) {
            auto error = commit.error();
            if (!discarded) {
                error.message += "; failed to finish apply journal cleanup: " + discarded.error().message;
                return Result<void>::fail(std::move(error));
            }
            if (publication.publication == RootedPublication::NotPublished || !publication.namespaceDurable ||
                !publication.failureCanBeReconciled) {
                return Result<void>::fail(std::move(error));
            }
            temporary.value().reset();
            auto reconciled = readOptional(path, maxBytes, &preparedMetadata.value().identity);
            if (!reconciled) {
                error.message += "; failed to reconcile apply journal publication: " + reconciled.error().message;
                return Result<void>::fail(std::move(error));
            }
            if (!reconciled.value() || *reconciled.value() != contents) {
                error.message += "; published apply journal contents do not match the prepared record";
                return Result<void>::fail(std::move(error));
            }
            return Result<void>::ok();
        }
        if (!discarded)
            return discarded;
        temporary.value().reset();
        auto verified = readOptional(path, maxBytes, &preparedMetadata.value().identity);
        if (!verified)
            return Result<void>::fail(verified.error());
        if (!verified.value() || *verified.value() != contents) {
            return Result<void>::fail(journalError("Apply journal commit verification failed: " + path));
        }
        return Result<void>::ok();
    } catch (...) {
        return Result<void>::fail(journalError("Failed to write apply journal file: " + path));
    }
}

Result<std::optional<ActiveTransaction>> ApplyJournalStore::loadActive() noexcept {
    try {
        auto contents = readOptional(activePath(), kMaxJournalRecordBytes);
        if (!contents)
            return Result<std::optional<ActiveTransaction>>::fail(contents.error());
        if (!contents.value())
            return Result<std::optional<ActiveTransaction>>::ok(std::nullopt);
        auto active = parseActiveTransaction(*contents.value());
        if (!active)
            return Result<std::optional<ActiveTransaction>>::fail(active.error());
        if (active.value().completedAt) {
            return Result<std::optional<ActiveTransaction>>::fail(
                journalError("Active apply transaction contains a completion timestamp"));
        }
        return Result<std::optional<ActiveTransaction>>::ok(std::move(active.value()));
    } catch (...) {
        return Result<std::optional<ActiveTransaction>>::fail(journalError("Failed to load active apply transaction"));
    }
}

Result<void> ApplyJournalStore::writeActive(const ActiveTransaction& active) noexcept {
    try {
        if (active.completedAt) {
            return Result<void>::fail(journalError("Active apply transaction cannot contain a completion timestamp"));
        }
        auto serialized = serializeActiveTransaction(active);
        if (!serialized)
            return Result<void>::fail(serialized.error());
        return writeAtomic(activePath(), serialized.value(), kMaxJournalRecordBytes);
    } catch (...) {
        return Result<void>::fail(journalError("Failed to write active apply transaction"));
    }
}

Result<std::optional<ActiveTransaction>> ApplyJournalStore::loadTerminal() noexcept {
    try {
        auto contents = readOptional(terminalPath(), kMaxJournalRecordBytes);
        if (!contents)
            return Result<std::optional<ActiveTransaction>>::fail(contents.error());
        if (!contents.value())
            return Result<std::optional<ActiveTransaction>>::ok(std::nullopt);
        auto terminal = parseActiveTransaction(*contents.value());
        if (!terminal)
            return Result<std::optional<ActiveTransaction>>::fail(terminal.error());
        return Result<std::optional<ActiveTransaction>>::ok(std::move(terminal.value()));
    } catch (...) {
        return Result<std::optional<ActiveTransaction>>::fail(
            journalError("Failed to load terminal apply transaction"));
    }
}

Result<void> ApplyJournalStore::writeTerminal(const ActiveTransaction& terminal) noexcept {
    try {
        if (!terminal.completedAt) {
            return Result<void>::fail(journalError("Terminal apply transaction is missing its completion timestamp"));
        }
        auto serialized = serializeActiveTransaction(terminal);
        if (!serialized)
            return Result<void>::fail(serialized.error());
        return writeAtomic(terminalPath(), serialized.value(), kMaxJournalRecordBytes);
    } catch (...) {
        return Result<void>::fail(journalError("Failed to write terminal apply transaction"));
    }
}

Result<void> ApplyJournalStore::clearActive(const std::string& expectedTransactionId) noexcept {
    try {
        auto active = loadActive();
        if (!active)
            return Result<void>::fail(active.error());
        if (!active.value())
            return Result<void>::ok();
        if (active.value()->transactionId != expectedTransactionId) {
            return Result<void>::fail(journalError("Active apply transaction changed before journal cleanup"));
        }
        auto opened = installRoot_.openRegularFile(activePath(), RootedFileOpenMode::ReadOnly);
        if (!opened)
            return Result<void>::fail(opened.error());
        if (!opened.value().exists())
            return Result<void>::ok();
        auto metadata = opened.value().file->metadata();
        if (!metadata) {
            auto error = metadata.error();
            auto closed = opened.value().file->close();
            if (!closed)
                error.message += "; failed to close active apply journal: " + closed.error().message;
            return Result<void>::fail(std::move(error));
        }
        auto closed = opened.value().file->close();
        if (!closed)
            return closed;
        auto removed = installRoot_.removeRegularFile(activePath(), RootedEntryExpectation::matching(metadata.value()));
        auto verified = loadActive();
        if (!verified) {
            if (!removed) {
                auto error = removed.error();
                error.message += "; failed to reconcile active journal removal: " + verified.error().message;
                return Result<void>::fail(std::move(error));
            }
            return Result<void>::fail(verified.error());
        }
        if (verified.value()) {
            if (!removed)
                return removed;
            return Result<void>::fail(journalError("Active apply transaction remained after journal cleanup"));
        }
        if (!removed) {
            auto durable = installRoot_.removeRegularFile(activePath(), RootedEntryExpectation::missing());
            if (!durable) {
                auto error = removed.error();
                error.message += "; failed to persist reconciled active journal removal: " + durable.error().message;
                return Result<void>::fail(std::move(error));
            }
            return removed;
        }
        return Result<void>::ok();
    } catch (...) {
        return Result<void>::fail(journalError("Failed to clear active apply transaction"));
    }
}

Result<void> ApplyJournalStore::writePlanSnapshot(const std::string& transactionId, const std::string& planDigest,
                                                  const std::string& planJson) noexcept {
    try {
        if (!isLowerHexDigest(transactionId) || !isLowerHexDigest(planDigest) ||
            util::sha256Bytes(planJson) != planDigest) {
            return Result<void>::fail(journalError("Apply plan snapshot does not match its recorded digest"));
        }
        auto existing = readOptional(planPath(transactionId), limits_.maxApplyPlanBytes);
        if (!existing)
            return Result<void>::fail(existing.error());
        if (existing.value()) {
            return Result<void>::fail(journalError("Refusing to reuse an existing apply transaction identity"));
        }
        return writeAtomic(planPath(transactionId), planJson, limits_.maxApplyPlanBytes);
    } catch (...) {
        return Result<void>::fail(journalError("Failed to write immutable apply plan snapshot"));
    }
}

Result<std::string> ApplyJournalStore::readPlanSnapshot(const std::string& transactionId,
                                                        const std::string& expectedPlanDigest) noexcept {
    try {
        if (!isLowerHexDigest(transactionId) || !isLowerHexDigest(expectedPlanDigest)) {
            return Result<std::string>::fail(journalError("Invalid apply transaction or plan identity"));
        }
        auto contents = readOptional(planPath(transactionId), limits_.maxApplyPlanBytes);
        if (!contents)
            return Result<std::string>::fail(contents.error());
        if (!contents.value())
            return Result<std::string>::fail(journalError("Apply plan snapshot is missing"));
        if (util::sha256Bytes(*contents.value()) != expectedPlanDigest) {
            return Result<std::string>::fail(journalError("Apply plan snapshot digest does not match the journal"));
        }
        return Result<std::string>::ok(std::move(*contents.value()));
    } catch (...) {
        return Result<std::string>::fail(journalError("Failed to read immutable apply plan snapshot"));
    }
}

Result<void> ApplyJournalStore::writeSummary(const ApplyJournalSummary& summary) noexcept {
    try {
        if (summary.operationCount > limits_.json.maxContainerEntries) {
            return Result<void>::fail(
                {ErrorCode::ResourceLimitExceeded, "Apply journal operation count exceeds its limit"});
        }
        auto serialized = serializeApplyJournalSummary(summary);
        if (!serialized)
            return Result<void>::fail(serialized.error());
        return writeAtomic(summaryPath(summary.transactionId), serialized.value(), kMaxJournalRecordBytes);
    } catch (...) {
        return Result<void>::fail(journalError("Failed to write apply journal summary"));
    }
}

Result<ApplyJournalSummary> ApplyJournalStore::readSummary(const std::string& transactionId) noexcept {
    try {
        if (!isLowerHexDigest(transactionId)) {
            return Result<ApplyJournalSummary>::fail(journalError("Invalid apply journal transaction identity"));
        }
        auto contents = readOptional(summaryPath(transactionId), kMaxJournalRecordBytes);
        if (!contents)
            return Result<ApplyJournalSummary>::fail(contents.error());
        if (!contents.value())
            return Result<ApplyJournalSummary>::fail(journalError("Apply journal summary is missing"));
        auto summary = parseApplyJournalSummary(*contents.value());
        if (!summary)
            return summary;
        if (summary.value().operationCount > limits_.json.maxContainerEntries) {
            return Result<ApplyJournalSummary>::fail(
                {ErrorCode::ResourceLimitExceeded, "Apply journal operation count exceeds its limit"});
        }
        if (summary.value().transactionId != transactionId) {
            return Result<ApplyJournalSummary>::fail(
                journalError("Apply journal summary transaction identity mismatch"));
        }
        return summary;
    } catch (...) {
        return Result<ApplyJournalSummary>::fail(journalError("Failed to read apply journal summary"));
    }
}

Result<void> ApplyJournalStore::writeOperation(const ApplyJournalOperation& operation) noexcept {
    try {
        auto serialized = serializeApplyJournalOperation(operation);
        if (!serialized)
            return Result<void>::fail(serialized.error());
        return writeAtomic(operationPath(operation.transactionId, operation.index), serialized.value(),
                           kMaxJournalRecordBytes);
    } catch (...) {
        return Result<void>::fail(journalError("Failed to write apply journal operation"));
    }
}

Result<std::optional<ApplyJournalOperation>> ApplyJournalStore::readOperation(const std::string& transactionId,
                                                                              std::size_t index) noexcept {
    try {
        if (!isLowerHexDigest(transactionId)) {
            return Result<std::optional<ApplyJournalOperation>>::fail(
                journalError("Invalid apply journal transaction identity"));
        }
        auto contents = readOptional(operationPath(transactionId, index), kMaxJournalRecordBytes);
        if (!contents)
            return Result<std::optional<ApplyJournalOperation>>::fail(contents.error());
        if (!contents.value())
            return Result<std::optional<ApplyJournalOperation>>::ok(std::nullopt);
        auto operation = parseApplyJournalOperation(*contents.value());
        if (!operation)
            return Result<std::optional<ApplyJournalOperation>>::fail(operation.error());
        if (operation.value().transactionId != transactionId || operation.value().index != index) {
            return Result<std::optional<ApplyJournalOperation>>::fail(
                journalError("Apply journal operation identity mismatch"));
        }
        return Result<std::optional<ApplyJournalOperation>>::ok(std::move(operation.value()));
    } catch (...) {
        return Result<std::optional<ApplyJournalOperation>>::fail(
            journalError("Failed to read apply journal operation"));
    }
}

} // namespace autoupdater::updater
