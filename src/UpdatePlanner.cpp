#include "UpdatePlanner.h"

#include "UrlPolicy.h"
#include "util/PathUtil.h"
#include "util/Rfc3339.h"
#include "util/UrlUtil.h"

#include <optional>
#include <string>
#include <utility>

namespace autoupdater {

namespace {

Result<std::optional<util::UtcInstant>> parseOptionalTimestamp(const std::string& value, const char* field) {
    if (value.empty()) {
        return Result<std::optional<util::UtcInstant>>::ok(std::nullopt);
    }
    auto parsed = util::parseRfc3339(value);
    if (!parsed) {
        return Result<std::optional<util::UtcInstant>>::fail(
            {ErrorCode::ManifestParseFailed,
             std::string(field) + " must use the documented RFC 3339 timestamp profile"});
    }
    return Result<std::optional<util::UtcInstant>>::ok(parsed.value());
}

Result<void> validateManifestAgainstConfig(const Config& config, const ManifestEnvelope& envelope,
                                           const UrlPolicy& policy, const util::UtcInstant& currentTime) {
    const auto& manifest = envelope.manifest;
    std::vector<std::string> managedTargets;
    managedTargets.reserve(manifest.files.size() + manifest.remove.size());
    for (const auto& file : manifest.files) {
        managedTargets.push_back(file.localPath.empty() ? file.path : file.localPath);
    }
    managedTargets.insert(managedTargets.end(), manifest.remove.begin(), manifest.remove.end());
    auto validTargets = util::validateManagedTargetPaths(managedTargets);
    if (!validTargets) {
        return validTargets;
    }
    if (config.installLayout == InstallLayout::PackageManagerOwned) {
        return Result<void>::fail(
            {ErrorCode::UnsupportedInstallLayout, "Package-manager-owned installs are not self-updated"});
    }
    if (!config.appId.empty() && !manifest.appId.empty() && config.appId != manifest.appId) {
        return Result<void>::fail({ErrorCode::SecurityPolicyViolation, "Manifest appId does not match config"});
    }
    if (!config.channel.empty() && !manifest.channel.empty() && config.channel != manifest.channel) {
        return Result<void>::fail({ErrorCode::SecurityPolicyViolation, "Manifest channel does not match config"});
    }
    if (!config.platform.empty() && !manifest.platform.empty() && config.platform != manifest.platform) {
        return Result<void>::fail({ErrorCode::SecurityPolicyViolation, "Manifest platform does not match config"});
    }
    if (!config.arch.empty() && !manifest.arch.empty() && config.arch != manifest.arch) {
        return Result<void>::fail({ErrorCode::SecurityPolicyViolation, "Manifest arch does not match config"});
    }
    if (manifest.minClientVersion && config.clientVersion < *manifest.minClientVersion) {
        return Result<void>::fail({ErrorCode::UnsupportedManifestSchema, "Updater client version is too old"});
    }
    auto releaseDate = parseOptionalTimestamp(manifest.releaseDate, "releaseDate");
    auto publishedAt = parseOptionalTimestamp(manifest.publishedAt, "publishedAt");
    auto expiresAt = parseOptionalTimestamp(manifest.expiresAt, "expiresAt");
    if (!releaseDate) {
        return Result<void>::fail(releaseDate.error());
    }
    if (!publishedAt) {
        return Result<void>::fail(publishedAt.error());
    }
    if (!expiresAt) {
        return Result<void>::fail(expiresAt.error());
    }
    if (config.security.rejectExpiredManifest && expiresAt.value() && currentTime >= *expiresAt.value()) {
        return Result<void>::fail({ErrorCode::SecurityPolicyViolation, "Manifest has expired"});
    }
    auto source = policy.authorize(envelope.sourceUrl);
    if (!source) {
        return Result<void>::fail(source.error());
    }
    auto artifactBase = policy.authorize(envelope.artifactBaseUrl);
    if (!artifactBase) {
        return Result<void>::fail(artifactBase.error());
    }
    if (artifactBase.value().path.empty() || artifactBase.value().path.back() != '/' || artifactBase.value().hasQuery) {
        return Result<void>::fail(
            {ErrorCode::SecurityPolicyViolation, "Resolved artifact base URL is not a query-free directory"});
    }
    auto transition = policy.authorizeTransition(source.value(), artifactBase.value());
    if (!transition) {
        return transition;
    }
    return Result<void>::ok();
}

} // namespace

Result<UpdateDecision> planUpdate(const Config& config, const ManifestEnvelope& envelope, const LocalSnapshot& snapshot,
                                  const std::optional<Version>& lastAcceptedVersion,
                                  const util::UtcInstant& currentTime) {
    const auto& manifest = envelope.manifest;
    auto policy = UrlPolicy::fromConfig(config);
    if (!policy) {
        return Result<UpdateDecision>::fail(policy.error());
    }
    auto validation = validateManifestAgainstConfig(config, envelope, policy.value(), currentTime);
    if (!validation) {
        return Result<UpdateDecision>::fail(validation.error());
    }
    auto artifactBase = policy.value().authorize(envelope.artifactBaseUrl);
    if (!artifactBase) {
        return Result<UpdateDecision>::fail(artifactBase.error());
    }

    UpdateDecision decision;
    decision.checkResult.currentVersion = config.currentVersion;
    decision.checkResult.remoteVersion = manifest.version;
    decision.checkResult.mandatory = manifest.mandatory;
    decision.checkResult.releaseId = manifest.releaseId;
    decision.checkResult.notes = manifest.notes;

    auto downgradeBaseline = config.currentVersion;
    if (lastAcceptedVersion && downgradeBaseline < *lastAcceptedVersion) {
        downgradeBaseline = *lastAcceptedVersion;
    }
    const bool isDowngrade = manifest.version < downgradeBaseline;
    const bool downgradeAuthorized = isDowngrade && !config.security.rejectDowngrade &&
                                     config.security.requireManifestSignature &&
                                     envelope.releaseManifestSignatureVerified && manifest.allowDowngrade;
    if (isDowngrade && !downgradeAuthorized) {
        decision.checkResult.downgradeRejected = true;
        return Result<UpdateDecision>::fail(
            {ErrorCode::SecurityPolicyViolation,
             "Manifest version is lower than the local baseline without verified downgrade authorization"});
    }

    if (manifest.minVersion && config.currentVersion < *manifest.minVersion) {
        decision.checkResult.reinstallRequired = true;
        return Result<UpdateDecision>::ok(std::move(decision));
    }

    if (!(config.currentVersion < manifest.version) &&
        !(downgradeAuthorized && manifest.version != config.currentVersion)) {
        return Result<UpdateDecision>::ok(std::move(decision));
    }

    decision.checkResult.updateAvailable = true;

    for (const auto& file : manifest.files) {
        const auto localPath = file.localPath.empty() ? file.path : file.localPath;
        auto validTarget = util::validateManagedTargetPath(localPath);
        if (!validTarget) {
            return Result<UpdateDecision>::fail(validTarget.error());
        }
        if (!util::pathAllowedByWhitelist(localPath, config.managedPathWhitelist)) {
            return Result<UpdateDecision>::fail(
                {ErrorCode::SecurityPolicyViolation, "Manifest file is outside managed whitelist"});
        }

        const auto* local = snapshot.find(localPath);
        const bool needsDownload = !local || !local->exists || local->sha256 != file.sha256;
        if (needsDownload) {
            ApplyOperation operation;
            operation.type = ApplyOperationType::Replace;
            operation.source = file.path;
            operation.target = localPath;
            operation.sha256 = file.sha256;
            operation.size = file.size;
            decision.operations.push_back(operation);

            PlannedDownload download;
            download.file = file;
            auto artifactUrl = util::appendEncodedPath(artifactBase.value(), file.path);
            if (!artifactUrl) {
                return Result<UpdateDecision>::fail(artifactUrl.error());
            }
            auto allowed = policy.value().authorizeTransition(artifactBase.value(), artifactUrl.value());
            if (!allowed) {
                return Result<UpdateDecision>::fail(allowed.error());
            }
            download.url = std::move(artifactUrl.value().canonical);
            auto stagingPath = util::safeJoin(config.tempDir, file.path);
            if (!stagingPath) {
                return Result<UpdateDecision>::fail(stagingPath.error());
            }
            download.stagingPath = stagingPath.value();
            decision.downloads.push_back(std::move(download));
        }
    }

    for (const auto& path : manifest.remove) {
        auto validTarget = util::validateManagedTargetPath(path);
        if (!validTarget) {
            return Result<UpdateDecision>::fail(validTarget.error());
        }
        if (!util::pathAllowedByWhitelist(path, config.managedPathWhitelist)) {
            return Result<UpdateDecision>::fail(
                {ErrorCode::SecurityPolicyViolation, "Remove path is outside managed whitelist"});
        }
        ApplyOperation operation;
        operation.type = ApplyOperationType::Remove;
        operation.target = path;
        decision.operations.push_back(std::move(operation));
    }

    return Result<UpdateDecision>::ok(std::move(decision));
}

} // namespace autoupdater
