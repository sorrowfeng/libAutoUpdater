#include "libAutoUpdater/ApplyPlan.h"

#include "util/Json.h"
#include "util/PathUtil.h"
#include "util/Sha256.h"

#include <cmath>
#include <limits>

namespace autoupdater {

namespace {

Result<std::string> requiredString(const util::Json& object, const std::string& key) {
    const auto* value = object.get(key);
    if (!value || !value->isString()) {
        return Result<std::string>::fail({ErrorCode::ManifestParseFailed, "Required string field missing: " + key});
    }
    return Result<std::string>::ok(value->asString());
}

std::string optionalString(const util::Json& object, const std::string& key) {
    const auto* value = object.get(key);
    return value && value->isString() ? value->asString() : std::string();
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

Result<std::uint64_t> operationSize(const util::Json& object, const ResourceLimits& limits) {
    const auto* value = object.get("size");
    if (!value) {
        return Result<std::uint64_t>::ok(0);
    }
    if (!value->isNumber()) {
        return Result<std::uint64_t>::fail({ErrorCode::ManifestParseFailed, "operation size must be a number"});
    }
    const double number = value->asNumber();
    constexpr double kLargestExactlyRepresentableInteger = 9007199254740991.0;
    if (!std::isfinite(number) || number < 0 || number > kLargestExactlyRepresentableInteger ||
        std::floor(number) != number) {
        return Result<std::uint64_t>::fail(
            {ErrorCode::ManifestParseFailed, "operation size must be a non-negative exact integer"});
    }
    const auto parsed = static_cast<std::uint64_t>(number);
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
    if (!value->isNumber()) {
        return Result<std::optional<std::uint32_t>>::fail(
            {ErrorCode::ManifestParseFailed, "operation permissions must be a number"});
    }
    const double number = value->asNumber();
    constexpr double kPortablePermissionMask = 0777.0;
    if (!std::isfinite(number) || number < 0 || number > kPortablePermissionMask || std::floor(number) != number) {
        return Result<std::optional<std::uint32_t>>::fail(
            {ErrorCode::ManifestParseFailed, "operation permissions must be portable rwx bits"});
    }
    return Result<std::optional<std::uint32_t>>::ok(static_cast<std::uint32_t>(number));
}

Result<std::optional<ApplyOperationPrecondition>> operationPrecondition(const util::Json& object,
                                                                         int schemaVersion,
                                                                         const ResourceLimits& limits) {
    const auto* value = object.get("precondition");
    if (!value) {
        return Result<std::optional<ApplyOperationPrecondition>>::ok(std::nullopt);
    }
    if (schemaVersion < 2 || !value->isObject()) {
        return Result<std::optional<ApplyOperationPrecondition>>::fail(
            {ErrorCode::ManifestParseFailed, "operation precondition requires a schemaVersion 2 object"});
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
    auto parsedSize = operationSize(*value, limits);
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
        if (!schema || !schema->isNumber() ||
            (schema->asNumber() != 1.0 && schema->asNumber() != 2.0)) {
            return Result<ApplyPlan>::fail(
                {ErrorCode::UnsupportedManifestSchema, "Unsupported apply plan schemaVersion"});
        }
        plan.schemaVersion = static_cast<int>(schema->asNumber());
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
                return Result<ApplyPlan>::fail(
                    {ErrorCode::ManifestParseFailed, "Apply plan intent is required"});
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
                auto transactionId = requiredString(*rollbackOf, "transactionId");
                auto planDigest = requiredString(*rollbackOf, "planDigest");
                if (!transactionId) {
                    return Result<ApplyPlan>::fail(transactionId.error());
                }
                if (!planDigest) {
                    return Result<ApplyPlan>::fail(planDigest.error());
                }
                if (!util::isLowerHexSha256(transactionId.value()) ||
                    !util::isLowerHexSha256(planDigest.value())) {
                    return Result<ApplyPlan>::fail(
                        {ErrorCode::ManifestParseFailed, "Rollback transaction reference is invalid"});
                }
                plan.rollbackOf = ApplyTransactionReference{transactionId.value(), planDigest.value()};
            } else if (rollbackOf) {
                return Result<ApplyPlan>::fail(
                    {ErrorCode::ManifestParseFailed, "Install plans cannot contain rollbackOf"});
            }
        }
        plan.appId = optionalString(json.value(), "appId");
        plan.fromVersion = optionalString(json.value(), "fromVersion");
        plan.toVersion = optionalString(json.value(), "toVersion");
        plan.releaseId = optionalString(json.value(), "releaseId");
        plan.manifestSha256 = optionalString(json.value(), "manifestSha256");

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
        plan.installDir = util::pathFromUtf8(installDir.value());
        plan.stagingDir = util::pathFromUtf8(stagingDir.value());
        plan.backupDir = util::pathFromUtf8(backupDir.value());

        const auto* restart = json.value().get("restartCommand");
        if (restart && restart->isArray()) {
            for (const auto& item : restart->asArray()) {
                if (item.isString()) {
                    plan.restartCommand.push_back(item.asString());
                }
            }
        }

        const auto* operations = json.value().get("operations");
        if (!operations || !operations->isArray()) {
            return Result<ApplyPlan>::fail({ErrorCode::ManifestParseFailed, "operations array is required"});
        }
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

            ApplyOperation op;
            op.type = type.value();
            op.source = optionalString(item, "source");
            auto target = requiredString(item, "target");
            if (!target) {
                return Result<ApplyPlan>::fail(target.error());
            }
            op.target = target.value();
            op.sha256 = optionalString(item, "sha256");
            auto size = operationSize(item, limits);
            if (!size) {
                return Result<ApplyPlan>::fail(size.error());
            }
            op.size = size.value();
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
            plan.operations.push_back(std::move(op));
        }

        return Result<ApplyPlan>::ok(std::move(plan));
    } catch (...) {
        return Result<ApplyPlan>::fail({ErrorCode::ManifestParseFailed, "Unexpected apply plan parse failure"});
    }
}

std::string ApplyPlan::toJson() const {
    util::Json::Object root;
    root.emplace("schemaVersion", static_cast<double>(schemaVersion));
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
        item.emplace("size", static_cast<double>(operation.size));
        if (schemaVersion >= 2 && operation.permissions) {
            item.emplace("permissions", static_cast<double>(*operation.permissions));
        }
        if (schemaVersion >= 2 && operation.precondition) {
            util::Json::Object precondition;
            precondition.emplace("exists", operation.precondition->exists);
            if (operation.precondition->exists) {
                precondition.emplace("size", static_cast<double>(operation.precondition->size));
                precondition.emplace("sha256", operation.precondition->sha256);
                if (operation.precondition->permissions) {
                    precondition.emplace("permissions",
                                         static_cast<double>(*operation.precondition->permissions));
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
