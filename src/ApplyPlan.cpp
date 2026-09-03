#include "libAutoUpdater/ApplyPlan.h"

#include "libAutoUpdater/Version.h"
#include "util/Json.h"
#include "util/PathUtil.h"
#include "util/Sha256.h"

#include <algorithm>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <map>

namespace autoupdater {

namespace {

Result<std::string> requiredString(const util::Json& object, const std::string& key) {
    const auto* value = object.get(key);
    if (!value || !value->isString()) {
        return Result<std::string>::fail({ErrorCode::ManifestParseFailed, "Required string field missing: " + key});
    }
    return Result<std::string>::ok(value->asString());
}

Result<std::string> optionalString(const util::Json& object, const std::string& key) {
    const auto* value = object.get(key);
    if (!value) {
        return Result<std::string>::ok({});
    }
    if (!value->isString()) {
        return Result<std::string>::fail({ErrorCode::ManifestParseFailed, "Optional field must be a string: " + key});
    }
    return Result<std::string>::ok(value->asString());
}

Result<void> requireOnlyKeys(const util::Json& object, std::initializer_list<const char*> allowedKeys,
                             const std::string& context) {
    for (const auto& entry : object.asObject()) {
        const auto allowed =
            std::any_of(allowedKeys.begin(), allowedKeys.end(), [&](const char* key) { return entry.first == key; });
        if (!allowed) {
            return Result<void>::fail(
                {ErrorCode::ManifestParseFailed, context + " contains unknown field: " + entry.first});
        }
    }
    return Result<void>::ok();
}

bool isNonNegativeInteger(const util::Json& value) {
    return value.isUnsignedInteger() || (value.isSignedInteger() && value.asInt64(-1) >= 0);
}

bool hasEmbeddedNul(const std::string& value) {
    return value.find('\0') != std::string::npos;
}

std::string portableSourceKey(std::string path) {
    std::transform(path.begin(), path.end(), path.begin(), [](unsigned char character) {
        if (character == '/') {
            return '\0';
        }
        if (character >= 'A' && character <= 'Z') {
            return static_cast<char>(character - 'A' + 'a');
        }
        return static_cast<char>(character);
    });
    return path;
}

Result<void>
validateSourceTopology(const std::map<std::string, std::pair<std::string, std::uint64_t>>& sourceEvidence) {
    if (sourceEvidence.empty()) {
        return Result<void>::ok();
    }
    auto previous = sourceEvidence.begin();
    for (auto current = std::next(previous); current != sourceEvidence.end(); ++current, ++previous) {
        if (current->first.size() > previous->first.size() &&
            current->first.compare(0, previous->first.size(), previous->first) == 0 &&
            current->first[previous->first.size()] == '\0') {
            return Result<void>::fail(
                {ErrorCode::ManifestParseFailed, "Apply source paths have an ancestor/descendant conflict"});
        }
    }
    return Result<void>::ok();
}

void addString(util::Json::Object& object, const std::string& key, const std::string& value) {
    if (!value.empty()) {
        object.emplace(key, value);
    }
}

std::string opToString(ApplyOperationType type) {
    return type == ApplyOperationType::Replace ? "replace" : "remove";
}

std::string intentToString(ApplyPlanIntent intent) {
    return intent == ApplyPlanIntent::Rollback ? "rollback" : "install";
}

Result<ApplyPlanIntent> intentFromString(const std::string& text) {
    if (text == "install") {
        return Result<ApplyPlanIntent>::ok(ApplyPlanIntent::Install);
    }
    if (text == "rollback") {
        return Result<ApplyPlanIntent>::ok(ApplyPlanIntent::Rollback);
    }
    return Result<ApplyPlanIntent>::fail({ErrorCode::ManifestParseFailed, "Unknown apply plan intent"});
}

Result<ApplyOperationType> opFromString(const std::string& text) {
    if (text == "replace") {
        return Result<ApplyOperationType>::ok(ApplyOperationType::Replace);
    }
    if (text == "remove") {
        return Result<ApplyOperationType>::ok(ApplyOperationType::Remove);
    }
    return Result<ApplyOperationType>::fail({ErrorCode::ManifestParseFailed, "Unknown apply operation type"});
}

Result<std::uint64_t> operationSize(const util::Json& object, const ResourceLimits& limits, bool required) {
    const auto* value = object.get("size");
    if (!value) {
        if (!required) {
            return Result<std::uint64_t>::ok(0);
        }
        return Result<std::uint64_t>::fail({ErrorCode::ManifestParseFailed, "operation size is required"});
    }
    if (!isNonNegativeInteger(*value)) {
        return Result<std::uint64_t>::fail(
            {ErrorCode::ManifestParseFailed, "operation size must be a non-negative integer"});
    }
    const auto parsed = value->asUInt64();
    if (parsed > limits.maxArtifactBytes) {
        return Result<std::uint64_t>::fail(
            {ErrorCode::ResourceLimitExceeded, "Apply operation exceeds the artifact byte limit"});
    }
    return Result<std::uint64_t>::ok(parsed);
}

Result<std::optional<std::uint32_t>> operationPermissions(const util::Json& object, int schemaVersion) {
    const auto* value = object.get("permissions");
    if (!value) {
        return Result<std::optional<std::uint32_t>>::ok(std::nullopt);
    }
    if (schemaVersion < 2) {
        return Result<std::optional<std::uint32_t>>::fail(
            {ErrorCode::ManifestParseFailed, "operation permissions require apply plan schemaVersion 2"});
    }
    if (!isNonNegativeInteger(*value)) {
        return Result<std::optional<std::uint32_t>>::fail(
            {ErrorCode::ManifestParseFailed, "operation permissions must be a non-negative integer"});
    }
    constexpr std::uint64_t kPortablePermissionMask = 0777;
    const auto number = value->asUInt64();
    if (number > kPortablePermissionMask) {
        return Result<std::optional<std::uint32_t>>::fail(
            {ErrorCode::ManifestParseFailed, "operation permissions must be portable rwx bits"});
    }
    return Result<std::optional<std::uint32_t>>::ok(static_cast<std::uint32_t>(number));
}

Result<std::optional<ApplyOperationPrecondition>> operationPrecondition(const util::Json& object, int schemaVersion,
                                                                        const ResourceLimits& limits) {
    const auto* value = object.get("precondition");
    if (!value) {
        return Result<std::optional<ApplyOperationPrecondition>>::ok(std::nullopt);
    }
    if (schemaVersion < 2 || !value->isObject()) {
        return Result<std::optional<ApplyOperationPrecondition>>::fail(
            {ErrorCode::ManifestParseFailed, "operation precondition requires a schemaVersion 2 object"});
    }
    auto keys = requireOnlyKeys(*value, {"exists", "size", "sha256", "permissions"}, "Apply operation precondition");
    if (!keys) {
        return Result<std::optional<ApplyOperationPrecondition>>::fail(keys.error());
    }
    const auto* exists = value->get("exists");
    if (!exists || !exists->isBool()) {
        return Result<std::optional<ApplyOperationPrecondition>>::fail(
            {ErrorCode::ManifestParseFailed, "operation precondition exists flag is required"});
    }

    ApplyOperationPrecondition precondition;
    precondition.exists = exists->asBool();
    const auto* size = value->get("size");
    const auto* sha256 = value->get("sha256");
    const auto* permissions = value->get("permissions");
    if (!precondition.exists) {
        if (size || sha256 || permissions) {
            return Result<std::optional<ApplyOperationPrecondition>>::fail(
                {ErrorCode::ManifestParseFailed, "missing-file preconditions cannot contain file evidence"});
        }
        return Result<std::optional<ApplyOperationPrecondition>>::ok(precondition);
    }
    if (!size || !sha256 || !sha256->isString() || !util::isLowerHexSha256(sha256->asString())) {
        return Result<std::optional<ApplyOperationPrecondition>>::fail(
            {ErrorCode::ManifestParseFailed, "existing-file precondition evidence is invalid"});
    }
    auto parsedSize = operationSize(*value, limits, true);
    if (!parsedSize) {
        return Result<std::optional<ApplyOperationPrecondition>>::fail(parsedSize.error());
    }
    auto parsedPermissions = operationPermissions(*value, schemaVersion);
    if (!parsedPermissions) {
        return Result<std::optional<ApplyOperationPrecondition>>::fail(parsedPermissions.error());
    }
    precondition.size = parsedSize.value();
    precondition.sha256 = sha256->asString();
    precondition.permissions = parsedPermissions.value();
    return Result<std::optional<ApplyOperationPrecondition>>::ok(std::move(precondition));
}

bool checkedAdd(std::uint64_t left, std::uint64_t right, std::uint64_t& result) {
    if (right > std::numeric_limits<std::uint64_t>::max() - left) {
        return false;
    }
    result = left + right;
    return true;
}

} // namespace

Result<ApplyPlan> ApplyPlan::parse(const std::string& jsonText) noexcept {
    return parse(jsonText, ResourceLimits{});
}

Result<ApplyPlan> ApplyPlan::parse(const std::string& jsonText, const ResourceLimits& limits) noexcept {
    try {
        if (jsonText.size() > limits.maxApplyPlanBytes) {
            return Result<ApplyPlan>::fail({ErrorCode::ResourceLimitExceeded, "Apply plan exceeds its byte limit"});
        }
        auto json = util::Json::parse(jsonText, limits.json);
        if (!json) {
            return Result<ApplyPlan>::fail(json.error());
        }
        if (!json.value().isObject()) {
            return Result<ApplyPlan>::fail({ErrorCode::ManifestParseFailed, "Apply plan root must be object"});
        }

        ApplyPlan plan;
        std::uint64_t totalArtifactBytes = 0;
        const auto* schema = json.value().get("schemaVersion");
        if (!schema || !isNonNegativeInteger(*schema) || (schema->asUInt64() != 1 && schema->asUInt64() != 2)) {
            return Result<ApplyPlan>::fail(
                {ErrorCode::UnsupportedManifestSchema, "Unsupported apply plan schemaVersion"});
        }
        plan.schemaVersion = static_cast<int>(schema->asUInt64());
        Result<void> rootKeys =
            plan.schemaVersion == 1
                ? requireOnlyKeys(json.value(),
                                  {"schemaVersion", "appId", "fromVersion", "toVersion", "releaseId", "manifestSha256",
                                   "installDir", "stagingDir", "backupDir", "restartCommand", "operations"},
                                  "Apply plan")
                : requireOnlyKeys(json.value(),
                                  {"schemaVersion", "intent", "rollbackOf", "appId", "fromVersion", "toVersion",
                                   "releaseId", "manifestSha256", "installDir", "stagingDir", "backupDir",
                                   "restartCommand", "operations"},
                                  "Apply plan");
        if (!rootKeys) {
            return Result<ApplyPlan>::fail(rootKeys.error());
        }
        const auto* intent = json.value().get("intent");
        const auto* rollbackOf = json.value().get("rollbackOf");
        if (plan.schemaVersion == 1) {
            if (intent || rollbackOf) {
                return Result<ApplyPlan>::fail(
                    {ErrorCode::ManifestParseFailed, "Apply plan schemaVersion 1 cannot contain transaction intent"});
            }
            plan.intent = ApplyPlanIntent::Install;
        } else {
            if (!intent || !intent->isString()) {
                return Result<ApplyPlan>::fail({ErrorCode::ManifestParseFailed, "Apply plan intent is required"});
            }
            auto parsedIntent = intentFromString(intent->asString());
            if (!parsedIntent) {
                return Result<ApplyPlan>::fail(parsedIntent.error());
            }
            plan.intent = parsedIntent.value();
            if (plan.intent == ApplyPlanIntent::Rollback) {
                if (!rollbackOf || !rollbackOf->isObject()) {
                    return Result<ApplyPlan>::fail(
                        {ErrorCode::ManifestParseFailed, "Rollback plans require rollbackOf"});
                }
                auto referenceKeys =
                    requireOnlyKeys(*rollbackOf, {"transactionId", "planDigest"}, "Apply rollback reference");
                if (!referenceKeys) {
                    return Result<ApplyPlan>::fail(referenceKeys.error());
                }
                auto transactionId = requiredString(*rollbackOf, "transactionId");
                auto planDigest = requiredString(*rollbackOf, "planDigest");
                if (!transactionId) {
                    return Result<ApplyPlan>::fail(transactionId.error());
                }
                if (!planDigest) {
                    return Result<ApplyPlan>::fail(planDigest.error());
                }
                if (!util::isLowerHexSha256(transactionId.value()) || !util::isLowerHexSha256(planDigest.value())) {
                    return Result<ApplyPlan>::fail(
                        {ErrorCode::ManifestParseFailed, "Rollback transaction reference is invalid"});
                }
                plan.rollbackOf = ApplyTransactionReference{transactionId.value(), planDigest.value()};
            } else if (rollbackOf) {
                return Result<ApplyPlan>::fail(
                    {ErrorCode::ManifestParseFailed, "Install plans cannot contain rollbackOf"});
            }
        }
        auto appId = optionalString(json.value(), "appId");
        auto fromVersion = optionalString(json.value(), "fromVersion");
        auto toVersion = optionalString(json.value(), "toVersion");
        auto releaseId = optionalString(json.value(), "releaseId");
        auto manifestSha256 = optionalString(json.value(), "manifestSha256");
        if (!appId)
            return Result<ApplyPlan>::fail(appId.error());
        if (!fromVersion)
            return Result<ApplyPlan>::fail(fromVersion.error());
        if (!toVersion)
            return Result<ApplyPlan>::fail(toVersion.error());
        if (!releaseId)
            return Result<ApplyPlan>::fail(releaseId.error());
        if (!manifestSha256)
            return Result<ApplyPlan>::fail(manifestSha256.error());
        if (!fromVersion.value().empty()) {
            auto parsed = Version::parse(fromVersion.value());
            if (!parsed) {
                return Result<ApplyPlan>::fail({ErrorCode::ManifestParseFailed, "Apply plan fromVersion is invalid"});
            }
        }
        if (!toVersion.value().empty()) {
            auto parsed = Version::parse(toVersion.value());
            if (!parsed) {
                return Result<ApplyPlan>::fail({ErrorCode::ManifestParseFailed, "Apply plan toVersion is invalid"});
            }
        }
        if (!manifestSha256.value().empty() && !util::isLowerHexSha256(manifestSha256.value())) {
            return Result<ApplyPlan>::fail({ErrorCode::ManifestParseFailed, "Apply plan manifestSha256 is invalid"});
        }
        plan.appId = std::move(appId.value());
        plan.fromVersion = std::move(fromVersion.value());
        plan.toVersion = std::move(toVersion.value());
        plan.releaseId = std::move(releaseId.value());
        plan.manifestSha256 = std::move(manifestSha256.value());

        auto installDir = requiredString(json.value(), "installDir");
        auto stagingDir = requiredString(json.value(), "stagingDir");
        auto backupDir = requiredString(json.value(), "backupDir");
        if (!installDir) {
            return Result<ApplyPlan>::fail(installDir.error());
        }
        if (!stagingDir) {
            return Result<ApplyPlan>::fail(stagingDir.error());
        }
        if (!backupDir) {
            return Result<ApplyPlan>::fail(backupDir.error());
        }
        if (installDir.value().empty() || stagingDir.value().empty() || backupDir.value().empty() ||
            hasEmbeddedNul(installDir.value()) || hasEmbeddedNul(stagingDir.value()) ||
            hasEmbeddedNul(backupDir.value())) {
            return Result<ApplyPlan>::fail(
                {ErrorCode::ManifestParseFailed, "Apply plan paths must be non-empty and contain no NUL"});
        }
        plan.installDir = util::pathFromUtf8(installDir.value());
        plan.stagingDir = util::pathFromUtf8(stagingDir.value());
        plan.backupDir = util::pathFromUtf8(backupDir.value());
        if (plan.installDir.empty() || plan.stagingDir.empty() || plan.backupDir.empty()) {
            return Result<ApplyPlan>::fail({ErrorCode::ManifestParseFailed, "Apply plan paths are invalid"});
        }

        const auto* restart = json.value().get("restartCommand");
        if (restart) {
            if (!restart->isArray()) {
                return Result<ApplyPlan>::fail({ErrorCode::ManifestParseFailed, "restartCommand must be an array"});
            }
            for (const auto& item : restart->asArray()) {
                if (!item.isString() || hasEmbeddedNul(item.asString())) {
                    return Result<ApplyPlan>::fail(
                        {ErrorCode::ManifestParseFailed, "restartCommand items must be NUL-free strings"});
                }
                plan.restartCommand.push_back(item.asString());
            }
            if (!plan.restartCommand.empty() && plan.restartCommand.front().empty()) {
                return Result<ApplyPlan>::fail(
                    {ErrorCode::ManifestParseFailed, "restartCommand executable must not be empty"});
            }
        }

        const auto* operations = json.value().get("operations");
        if (!operations || !operations->isArray()) {
            return Result<ApplyPlan>::fail({ErrorCode::ManifestParseFailed, "operations array is required"});
        }
        std::vector<std::string> managedTargets;
        managedTargets.reserve(operations->asArray().size());
        std::map<std::string, std::pair<std::string, std::uint64_t>> sourceEvidence;
        for (const auto& item : operations->asArray()) {
            if (!item.isObject()) {
                return Result<ApplyPlan>::fail({ErrorCode::ManifestParseFailed, "operation must be object"});
            }
            auto typeText = requiredString(item, "type");
            if (!typeText) {
                return Result<ApplyPlan>::fail(typeText.error());
            }
            auto type = opFromString(typeText.value());
            if (!type) {
                return Result<ApplyPlan>::fail(type.error());
            }
            auto operationKeys =
                plan.schemaVersion == 1
                    ? requireOnlyKeys(item, {"type", "source", "target", "sha256", "size"}, "Apply operation")
                    : requireOnlyKeys(item,
                                      {"type", "source", "target", "sha256", "size", "permissions", "precondition"},
                                      "Apply operation");
            if (!operationKeys) {
                return Result<ApplyPlan>::fail(operationKeys.error());
            }

            ApplyOperation op;
            op.type = type.value();
            auto target = requiredString(item, "target");
            if (!target) {
                return Result<ApplyPlan>::fail(target.error());
            }
            op.target = target.value();
            if (op.type == ApplyOperationType::Replace) {
                auto source = requiredString(item, "source");
                auto sha256 = requiredString(item, "sha256");
                if (!source) {
                    return Result<ApplyPlan>::fail(source.error());
                }
                if (!sha256) {
                    return Result<ApplyPlan>::fail(sha256.error());
                }
                if (!util::isLowerHexSha256(sha256.value())) {
                    return Result<ApplyPlan>::fail({ErrorCode::ManifestParseFailed,
                                                    "Replace operation sha256 must be a lowercase SHA-256 digest"});
                }
                op.source = std::move(source.value());
                op.sha256 = std::move(sha256.value());
            } else if (item.get("source") || item.get("sha256")) {
                return Result<ApplyPlan>::fail(
                    {ErrorCode::ManifestParseFailed, "Remove operations cannot specify source or sha256"});
            }
            auto size = operationSize(item, limits, op.type == ApplyOperationType::Replace);
            if (!size) {
                return Result<ApplyPlan>::fail(size.error());
            }
            op.size = size.value();
            if (op.type == ApplyOperationType::Remove && op.size != 0) {
                return Result<ApplyPlan>::fail({ErrorCode::ManifestParseFailed, "Remove operation size must be zero"});
            }
            auto permissions = operationPermissions(item, plan.schemaVersion);
            if (!permissions) {
                return Result<ApplyPlan>::fail(permissions.error());
            }
            op.permissions = permissions.value();
            auto precondition = operationPrecondition(item, plan.schemaVersion, limits);
            if (!precondition) {
                return Result<ApplyPlan>::fail(precondition.error());
            }
            op.precondition = std::move(precondition.value());
            auto validTarget = util::validateManagedTargetPath(op.target);
            if (!validTarget) {
                return Result<ApplyPlan>::fail(validTarget.error());
            }
            if (op.type == ApplyOperationType::Replace) {
                auto validSource = util::validateManagedPath(op.source);
                if (!validSource) {
                    return Result<ApplyPlan>::fail(validSource.error());
                }
                const auto sourceKey = portableSourceKey(op.source);
                const auto existingEvidence = sourceEvidence.find(sourceKey);
                if (existingEvidence == sourceEvidence.end()) {
                    sourceEvidence.emplace(sourceKey, std::make_pair(op.sha256, op.size));
                } else if (existingEvidence->second.first != op.sha256 || existingEvidence->second.second != op.size) {
                    return Result<ApplyPlan>::fail(
                        {ErrorCode::ManifestParseFailed,
                         "Shared apply sources must use identical size and SHA-256 evidence"});
                }
                std::uint64_t updatedTotal = 0;
                if (!checkedAdd(totalArtifactBytes, op.size, updatedTotal) ||
                    updatedTotal > limits.maxTotalArtifactBytes) {
                    return Result<ApplyPlan>::fail(
                        {ErrorCode::ResourceLimitExceeded, "Apply operations exceed the total artifact byte limit"});
                }
                totalArtifactBytes = updatedTotal;
            } else if (op.permissions) {
                return Result<ApplyPlan>::fail(
                    {ErrorCode::ManifestParseFailed, "Remove operations cannot specify permissions"});
            }
            managedTargets.push_back(op.target);
            plan.operations.push_back(std::move(op));
        }

        auto validSources = validateSourceTopology(sourceEvidence);
        if (!validSources) {
            return Result<ApplyPlan>::fail(validSources.error());
        }

        auto validTargets = util::validateManagedTargetPaths(managedTargets);
        if (!validTargets) {
            return Result<ApplyPlan>::fail(validTargets.error());
        }

        return Result<ApplyPlan>::ok(std::move(plan));
    } catch (...) {
        return Result<ApplyPlan>::fail({ErrorCode::ManifestParseFailed, "Unexpected apply plan parse failure"});
    }
}

std::string ApplyPlan::toJson() const {
    util::Json::Object root;
    root.emplace("schemaVersion", schemaVersion);
    if (schemaVersion >= 2) {
        root.emplace("intent", intentToString(intent));
        if (rollbackOf) {
            util::Json::Object reference;
            reference.emplace("transactionId", rollbackOf->transactionId);
            reference.emplace("planDigest", rollbackOf->planDigest);
            root.emplace("rollbackOf", std::move(reference));
        }
    }
    addString(root, "appId", appId);
    addString(root, "fromVersion", fromVersion);
    addString(root, "toVersion", toVersion);
    addString(root, "releaseId", releaseId);
    addString(root, "manifestSha256", manifestSha256);
    root.emplace("installDir", util::pathToUtf8(installDir));
    root.emplace("stagingDir", util::pathToUtf8(stagingDir));
    root.emplace("backupDir", util::pathToUtf8(backupDir));

    util::Json::Array restart;
    for (const auto& arg : restartCommand) {
        restart.emplace_back(arg);
    }
    root.emplace("restartCommand", std::move(restart));

    util::Json::Array operationItems;
    for (const auto& operation : this->operations) {
        util::Json::Object item;
        item.emplace("type", opToString(operation.type));
        if (!operation.source.empty()) {
            item.emplace("source", operation.source);
        }
        item.emplace("target", operation.target);
        if (!operation.sha256.empty()) {
            item.emplace("sha256", operation.sha256);
        }
        item.emplace("size", operation.size);
        if (schemaVersion >= 2 && operation.permissions) {
            item.emplace("permissions", *operation.permissions);
        }
        if (schemaVersion >= 2 && operation.precondition) {
            util::Json::Object precondition;
            precondition.emplace("exists", operation.precondition->exists);
            if (operation.precondition->exists) {
                precondition.emplace("size", operation.precondition->size);
                precondition.emplace("sha256", operation.precondition->sha256);
                if (operation.precondition->permissions) {
                    precondition.emplace("permissions", *operation.precondition->permissions);
                }
            }
            item.emplace("precondition", std::move(precondition));
        }
        operationItems.emplace_back(std::move(item));
    }
    root.emplace("operations", std::move(operationItems));

    return util::Json(std::move(root)).stringify(2);
}

} // namespace autoupdater
