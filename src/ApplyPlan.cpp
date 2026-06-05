#include "libAutoUpdater/ApplyPlan.h"

#include "util/Json.h"
#include "util/PathUtil.h"

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

Result<ApplyOperationType> opFromString(const std::string& text) {
    if (text == "replace") {
        return Result<ApplyOperationType>::ok(ApplyOperationType::Replace);
    }
    if (text == "remove") {
        return Result<ApplyOperationType>::ok(ApplyOperationType::Remove);
    }
    return Result<ApplyOperationType>::fail({ErrorCode::ManifestParseFailed, "Unknown apply operation type"});
}

} // namespace

Result<ApplyPlan> ApplyPlan::parse(const std::string& jsonText) noexcept {
    try {
        auto json = util::Json::parse(jsonText);
        if (!json) {
            return Result<ApplyPlan>::fail(json.error());
        }
        if (!json.value().isObject()) {
            return Result<ApplyPlan>::fail({ErrorCode::ManifestParseFailed, "Apply plan root must be object"});
        }

        ApplyPlan plan;
        const auto* schema = json.value().get("schemaVersion");
        if (!schema || !schema->isNumber() || schema->asInt() != 1) {
            return Result<ApplyPlan>::fail(
                {ErrorCode::UnsupportedManifestSchema, "Unsupported apply plan schemaVersion"});
        }
        plan.schemaVersion = 1;
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
            const auto* size = item.get("size");
            if (size && size->isNumber()) {
                op.size = static_cast<std::uint64_t>(size->asNumber());
            }
            auto validTarget = util::validateManagedPath(op.target);
            if (!validTarget) {
                return Result<ApplyPlan>::fail(validTarget.error());
            }
            if (op.type == ApplyOperationType::Replace) {
                auto validSource = util::validateManagedPath(op.source);
                if (!validSource) {
                    return Result<ApplyPlan>::fail(validSource.error());
                }
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
        operationItems.emplace_back(std::move(item));
    }
    root.emplace("operations", std::move(operationItems));

    return util::Json(std::move(root)).stringify(2);
}

} // namespace autoupdater
