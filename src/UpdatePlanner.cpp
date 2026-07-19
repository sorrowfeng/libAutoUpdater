#include "UpdatePlanner.h"

#include "UrlPolicy.h"
#include "util/PathUtil.h"
#include "util/UrlUtil.h"

#include <ctime>
#include <iomanip>
#include <sstream>
#include <utility>

namespace autoupdater {

namespace {

std::string currentUtcIsoLike() {
    const auto now = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &now);
#else
    gmtime_r(&now, &tm);
#endif
    std::ostringstream stream;
    stream << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return stream.str();
}

Result<void> validateManifestAgainstConfig(const Config& config, const ManifestEnvelope& envelope,
                                           const UrlPolicy& policy) {
    const auto& manifest = envelope.manifest;
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
    if (config.security.rejectExpiredManifest && !manifest.expiresAt.empty() &&
        currentUtcIsoLike() > manifest.expiresAt) {
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
                                  const std::optional<Version>& lastAcceptedVersion) {
    const auto& manifest = envelope.manifest;
    auto policy = UrlPolicy::fromConfig(config);
    if (!policy) {
        return Result<UpdateDecision>::fail(policy.error());
    }
    auto validation = validateManifestAgainstConfig(config, envelope, policy.value());
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

    if (manifest.minVersion && config.currentVersion < *manifest.minVersion) {
        decision.checkResult.reinstallRequired = true;
        return Result<UpdateDecision>::ok(std::move(decision));
    }

    if (config.security.rejectDowngrade && !manifest.allowDowngrade) {
        const auto baseline = lastAcceptedVersion ? *lastAcceptedVersion : config.currentVersion;
        if (manifest.version < baseline) {
            decision.checkResult.downgradeRejected = true;
            return Result<UpdateDecision>::fail(
                {ErrorCode::SecurityPolicyViolation, "Manifest version is lower than accepted version"});
        }
    }

    if (!(config.currentVersion < manifest.version) &&
        !(manifest.allowDowngrade && manifest.version != config.currentVersion)) {
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
