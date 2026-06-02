#include "UpdatePlanner.h"

#include "util/PathUtil.h"
#include "util/UrlUtil.h"

#include <ctime>
#include <iomanip>
#include <sstream>

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

Result<void> validateManifestAgainstConfig(const Config& config, const Manifest& manifest) {
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
    if (!util::urlStartsWithAny(manifest.baseUrl, config.security.allowedBaseUrls)) {
        return Result<void>::fail({ErrorCode::SecurityPolicyViolation, "Manifest baseUrl is not allowed"});
    }
    return Result<void>::ok();
}

} // namespace

Result<UpdateDecision> planUpdate(const Config& config, const ManifestEnvelope& envelope, const LocalSnapshot& snapshot,
                                  const std::optional<Version>& lastAcceptedVersion) {
    const auto& manifest = envelope.manifest;
    auto validation = validateManifestAgainstConfig(config, manifest);
    if (!validation) {
        return Result<UpdateDecision>::fail(validation.error());
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
            download.url = util::joinUrl(manifest.baseUrl, file.path);
            auto stagingPath = util::safeJoin(config.tempDir, file.path);
            if (!stagingPath) {
                return Result<UpdateDecision>::fail(stagingPath.error());
            }
            download.stagingPath = stagingPath.value();
            decision.downloads.push_back(std::move(download));
        }
    }

    for (const auto& path : manifest.remove) {
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
