#include "DownloadExecutor.h"

#include "DownloadResumeStore.h"
#include "ErrorUtil.h"
#include "NetworkLimits.h"
#include "NetworkRequest.h"
#include "UrlPolicy.h"
#include "util/Rfc3339.h"
#include "util/Sha256.h"
#include "util/UrlUtil.h"

#include <algorithm>
#include <chrono>
#include <map>
#include <set>
#include <string_view>
#include <thread>

namespace autoupdater {

namespace {

constexpr std::uint64_t kDownloadResumeMaxAgeSeconds = 7 * 24 * 60 * 60;

void appendIdentityInteger(std::string& material, std::uint64_t value) {
    for (int shift = 56; shift >= 0; shift -= 8) {
        material.push_back(static_cast<char>((value >> shift) & 0xffU));
    }
}

void appendIdentityField(std::string& material, std::string_view value) {
    appendIdentityInteger(material, static_cast<std::uint64_t>(value.size()));
    material.append(value.data(), value.size());
}

std::string urlSchemeIdentity(util::UrlScheme scheme) {
    switch (scheme) {
    case util::UrlScheme::Http:
        return "http";
    case util::UrlScheme::Https:
        return "https";
    case util::UrlScheme::File:
        return "file";
    }
    return {};
}

std::string temporaryDownloadPath(const std::string& target) {
    return target + ".download";
}

Error appendSecondaryError(Error primary, const std::string& context, const Error& secondary) {
    primary.message += "; " + context + " [" + toString(secondary.code) + "]: " + secondary.message;
    return primary;
}

Result<void> closeRootedFile(std::unique_ptr<IRootedFile>& file) {
    if (!file) {
        return Result<void>::ok();
    }
    auto closed = file->close();
    file.reset();
    return closed;
}

Result<std::optional<RootedFileMetadata>>
inspectVerifiedDownload(IRootedDirectory& root, const std::string& relativePath, std::uint64_t expectedSize,
                        const std::string& expectedHash, IHashProvider& hashProvider) {
    auto opened = root.openRegularFile(relativePath, RootedFileOpenMode::ReadOnly);
    if (!opened) {
        return Result<std::optional<RootedFileMetadata>>::fail(opened.error());
    }
    if (!opened.value().exists()) {
        return Result<std::optional<RootedFileMetadata>>::ok(std::nullopt);
    }
    auto metadata = opened.value().file->metadata();
    if (!metadata) {
        auto error = metadata.error();
        auto closed = closeRootedFile(opened.value().file);
        if (!closed) {
            error = appendSecondaryError(std::move(error), "failed to close the reconciled download", closed.error());
        }
        return Result<std::optional<RootedFileMetadata>>::fail(std::move(error));
    }
    if (metadata.value().size != expectedSize) {
        auto closed = closeRootedFile(opened.value().file);
        if (!closed) {
            return Result<std::optional<RootedFileMetadata>>::fail(closed.error());
        }
        return Result<std::optional<RootedFileMetadata>>::ok(std::nullopt);
    }
    auto hash = hashProvider.sha256Stream(*opened.value().file);
    if (!hash) {
        auto error = hash.error();
        auto closed = closeRootedFile(opened.value().file);
        if (!closed) {
            error = appendSecondaryError(std::move(error), "failed to close the reconciled download", closed.error());
        }
        return Result<std::optional<RootedFileMetadata>>::fail(std::move(error));
    }
    if (hash.value() != expectedHash) {
        auto closed = closeRootedFile(opened.value().file);
        if (!closed) {
            return Result<std::optional<RootedFileMetadata>>::fail(closed.error());
        }
        return Result<std::optional<RootedFileMetadata>>::ok(std::nullopt);
    }
    auto result = std::move(metadata.value());
    auto closed = closeRootedFile(opened.value().file);
    if (!closed) {
        return Result<std::optional<RootedFileMetadata>>::fail(closed.error());
    }
    return Result<std::optional<RootedFileMetadata>>::ok(std::move(result));
}

} // namespace

Result<std::string> detail::downloadResumeReleaseKey(const Config& config, const UpdateDecision& decision) noexcept {
    try {
        std::string material = "libAutoUpdater/resume-release/v1";
        appendIdentityField(material, config.appId);
        appendIdentityField(material, config.channel);
        appendIdentityField(material, config.platform);
        appendIdentityField(material, config.arch);
        appendIdentityField(material, decision.checkResult.remoteVersion
                                          ? decision.checkResult.remoteVersion->toString()
                                          : std::string{});
        appendIdentityField(material, decision.checkResult.releaseId);
        return Result<std::string>::ok(util::sha256Bytes(material));
    } catch (...) {
        return Result<std::string>::fail(
            {ErrorCode::InternalError, "Failed to derive the download resume release identity"});
    }
}

Result<std::string> detail::downloadResumeResourceKey(const std::string& releaseKey,
                                                      const PlannedDownload& download) noexcept {
    try {
        if (!util::isLowerHexSha256(releaseKey)) {
            return Result<std::string>::fail({ErrorCode::InternalError, "Download resume release identity is invalid"});
        }
        auto parsed = util::parseAbsoluteUrl(download.url);
        if (!parsed) {
            return Result<std::string>::fail(parsed.error());
        }

        std::string material = "libAutoUpdater/resume-resource/v1";
        appendIdentityField(material, releaseKey);
        appendIdentityField(material, urlSchemeIdentity(parsed.value().scheme));
        appendIdentityField(material, parsed.value().host);
        appendIdentityInteger(material, parsed.value().port);
        appendIdentityField(material, parsed.value().path);
        appendIdentityField(material, download.file.path);
        appendIdentityField(material, download.file.sha256);
        appendIdentityInteger(material, download.file.size);
        // The parsed query is deliberately excluded so rotating signed URL
        // credentials cannot enter state or invalidate otherwise identical
        // resume metadata.
        return Result<std::string>::ok(util::sha256Bytes(material));
    } catch (...) {
        return Result<std::string>::fail(
            {ErrorCode::InternalError, "Failed to derive the download resume resource identity"});
    }
}

namespace {

class DownloadResumeCoordinator final {
  public:
    DownloadResumeCoordinator(IStateStore* store, detail::DownloadResumeScope scope)
        : store_(store), batchStore_(dynamic_cast<detail::IDownloadResumeBatchStore*>(store)),
          scope_(std::move(scope)) {}

    Result<void> initialize(const std::vector<std::string>& keys) {
        if (!batchStore_) {
            return Result<void>::ok();
        }
        auto loaded = batchStore_->loadDownloadResumeBatch(scope_, keys);
        if (!loaded) {
            return Result<void>::fail(
                detail::withErrorPhase(loaded.error(), ErrorPhase::StatePersistence));
        }
        const std::set<std::string> requested(keys.begin(), keys.end());
        for (auto& state : loaded.value()) {
            if (requested.find(state.key) == requested.end() || cached_.find(state.key) != cached_.end()) {
                return Result<void>::fail(
                    {ErrorCode::StateStoreError, "Download resume batch returned an unexpected resource",
                     ErrorPhase::StatePersistence});
            }
            cached_.emplace(state.key, std::move(state));
        }
        return Result<void>::ok();
    }

    Result<std::optional<DownloadResumeState>> load(const std::string& key) {
        if (!store_) {
            return Result<std::optional<DownloadResumeState>>::ok(std::nullopt);
        }
        if (batchStore_) {
            const auto pending = upserts_.find(key);
            if (pending != upserts_.end()) {
                return Result<std::optional<DownloadResumeState>>::ok(pending->second);
            }
            if (clears_.find(key) != clears_.end()) {
                return Result<std::optional<DownloadResumeState>>::ok(std::nullopt);
            }
            const auto cached = cached_.find(key);
            return Result<std::optional<DownloadResumeState>>::ok(
                cached == cached_.end() ? std::optional<DownloadResumeState>{} : cached->second);
        }
        auto loaded = store_->loadDownloadResume(key);
        if (!loaded) {
            return Result<std::optional<DownloadResumeState>>::fail(
                detail::withErrorPhase(loaded.error(), ErrorPhase::StatePersistence));
        }
        return loaded;
    }

    Result<void> save(DownloadResumeState state) {
        if (!store_) {
            return Result<void>::ok();
        }
        if (!batchStore_) {
            auto saved = store_->saveDownloadResume(state);
            if (!saved) {
                return Result<void>::fail(
                    detail::withErrorPhase(saved.error(), ErrorPhase::StatePersistence));
            }
            return saved;
        }
        clears_.erase(state.key);
        cached_[state.key] = state;
        upserts_[state.key] = std::move(state);
        return Result<void>::ok();
    }

    Result<void> clear(const std::string& key) {
        if (!store_) {
            return Result<void>::ok();
        }
        if (!batchStore_) {
            auto cleared = store_->clearDownloadResume(key);
            if (!cleared) {
                return Result<void>::fail(
                    detail::withErrorPhase(cleared.error(), ErrorPhase::StatePersistence));
            }
            return cleared;
        }
        cached_.erase(key);
        upserts_.erase(key);
        clears_.insert(key);
        return Result<void>::ok();
    }

    Result<void> flush() {
        if (!batchStore_) {
            return Result<void>::ok();
        }
        std::vector<DownloadResumeState> upserts;
        upserts.reserve(upserts_.size());
        for (const auto& entry : upserts_) {
            upserts.push_back(entry.second);
        }
        std::vector<std::string> clears(clears_.begin(), clears_.end());
        auto applied = batchStore_->applyDownloadResumeBatch(scope_, upserts, clears);
        if (!applied) {
            return Result<void>::fail(
                detail::withErrorPhase(applied.error(), ErrorPhase::StatePersistence));
        }
        upserts_.clear();
        clears_.clear();
        return Result<void>::ok();
    }

  private:
    IStateStore* store_ = nullptr;
    detail::IDownloadResumeBatchStore* batchStore_ = nullptr;
    detail::DownloadResumeScope scope_;
    std::map<std::string, DownloadResumeState> cached_;
    std::map<std::string, DownloadResumeState> upserts_;
    std::set<std::string> clears_;
};

} // namespace

Result<void> executeDownloads(const Config& config, const UpdateDecision& decision, INetworkClient& network,
                              IFileSystem& fileSystem, IHashProvider& hashProvider, IStateStore* stateStore,
                              ProgressCallback progress, CancellationToken& cancel) {
    std::uint64_t totalBytes = 0;
    for (const auto& download : decision.downloads) {
        if (download.file.size > config.resources.maxArtifactBytes) {
            return Result<void>::fail({ErrorCode::ResourceLimitExceeded, "Artifact exceeds the per-file byte limit"});
        }
        std::uint64_t updatedTotal = 0;
        if (!detail::checkedAdd(totalBytes, download.file.size, updatedTotal) ||
            updatedTotal > config.resources.maxTotalArtifactBytes) {
            return Result<void>::fail(
                {ErrorCode::ResourceLimitExceeded, "Downloads exceed the total artifact byte limit"});
        }
        totalBytes = updatedTotal;
    }

    auto policy = UrlPolicy::fromConfig(config);
    if (!policy) {
        return Result<void>::fail(policy.error());
    }

    auto releaseKey = detail::downloadResumeReleaseKey(config, decision);
    if (!releaseKey) {
        return Result<void>::fail(releaseKey.error());
    }
    std::vector<std::string> resumeKeys;
    resumeKeys.reserve(decision.downloads.size());
    for (const auto& download : decision.downloads) {
        auto key = detail::downloadResumeResourceKey(releaseKey.value(), download);
        if (!key) {
            return Result<void>::fail(key.error());
        }
        resumeKeys.push_back(std::move(key.value()));
    }

    detail::DownloadResumeScope resumeScope;
    resumeScope.releaseKey = releaseKey.value();
    resumeScope.maxAgeSeconds = kDownloadResumeMaxAgeSeconds;
    if (dynamic_cast<detail::IDownloadResumeBatchStore*>(stateStore)) {
        auto now = util::currentUtcInstant();
        if (!now) {
            return Result<void>::fail(
                detail::withErrorPhase(now.error(), ErrorPhase::StatePersistence));
        }
        if (now.value().unixSeconds < 0) {
            return Result<void>::fail(
                {ErrorCode::StateStoreError, "The system clock cannot timestamp download resume state",
                 ErrorPhase::StatePersistence});
        }
        resumeScope.nowUnixSeconds = static_cast<std::uint64_t>(now.value().unixSeconds);
    }
    DownloadResumeCoordinator resumeState(stateStore, std::move(resumeScope));
    auto initializedResume =
        resumeState.initialize(config.network.enableResume ? resumeKeys : std::vector<std::string>{});
    if (!initializedResume) {
        return Result<void>::fail(initializedResume.error());
    }

    auto stagingRoot = fileSystem.openRoot(config.tempDir, RootAccess::ReadWrite, true);
    if (!stagingRoot) {
        return Result<void>::fail(stagingRoot.error());
    }

    auto execution = [&]() -> Result<void> {
        std::uint64_t completedBeforeFile = 0;
        std::size_t downloadIndex = 0;
        for (const auto& download : decision.downloads) {
            const auto& resumeKey = resumeKeys[downloadIndex++];
            if (cancel.isCancelled()) {
                return Result<void>::fail({ErrorCode::Cancelled, "Download cancelled"});
            }

            const auto finalRelativePath = download.file.path;
            const auto tempRelativePath = temporaryDownloadPath(finalRelativePath);

            auto existing = stagingRoot.value()->openRegularFile(finalRelativePath, RootedFileOpenMode::ReadOnly);
            if (!existing) {
                return Result<void>::fail(existing.error());
            }
            RootedEntryExpectation finalExpectation = RootedEntryExpectation::missing();
            if (existing.value().exists()) {
                auto existingMetadata = existing.value().file->metadata();
                if (!existingMetadata) {
                    auto error = existingMetadata.error();
                    auto closed = closeRootedFile(existing.value().file);
                    if (!closed) {
                        error = appendSecondaryError(std::move(error), "failed to close the existing download",
                                                     closed.error());
                    }
                    return Result<void>::fail(std::move(error));
                }
                finalExpectation = RootedEntryExpectation::matching(existingMetadata.value());
                if (existingMetadata.value().size == download.file.size) {
                    auto hash = hashProvider.sha256Stream(*existing.value().file);
                    if (!hash) {
                        auto error = hash.error();
                        auto closed = closeRootedFile(existing.value().file);
                        if (!closed) {
                            error = appendSecondaryError(std::move(error), "failed to close the existing download",
                                                         closed.error());
                        }
                        return Result<void>::fail(std::move(error));
                    }
                    if (hash.value() != download.file.sha256) {
                        auto closed = closeRootedFile(existing.value().file);
                        if (!closed) {
                            return closed;
                        }
                    } else {
                        auto closed = closeRootedFile(existing.value().file);
                        if (!closed) {
                            return closed;
                        }
                        if (stateStore) {
                            auto cleared = resumeState.clear(resumeKey);
                            if (!cleared) {
                                return Result<void>::fail(cleared.error());
                            }
                        }
                        completedBeforeFile += download.file.size;
                        if (progress) {
                            progress({completedBeforeFile, totalBytes, download.file.path});
                        }
                        continue;
                    }
                }
            }
            auto existingClosed = closeRootedFile(existing.value().file);
            if (!existingClosed) {
                return existingClosed;
            }

            auto canonicalDownloadUrl = policy.value().authorize(download.url);
            if (!canonicalDownloadUrl) {
                return Result<void>::fail(canonicalDownloadUrl.error());
            }

            Error lastError{ErrorCode::DownloadFailed, "Download failed"};
            bool success = false;
            for (int attempt = 0; attempt <= config.retry.maxRetries; ++attempt) {
                if (cancel.isCancelled()) {
                    return Result<void>::fail({ErrorCode::Cancelled, "Download cancelled"});
                }

                auto temporary =
                    stagingRoot.value()->openRegularFile(tempRelativePath, RootedFileOpenMode::OpenOrCreate);
                if (!temporary) {
                    return Result<void>::fail(temporary.error());
                }
                auto tempMetadata = temporary.value().file->metadata();
                if (!tempMetadata) {
                    auto error = tempMetadata.error();
                    auto closed = closeRootedFile(temporary.value().file);
                    if (!closed) {
                        error = appendSecondaryError(std::move(error), "failed to close the partial download",
                                                     closed.error());
                    }
                    return Result<void>::fail(std::move(error));
                }
                if (tempMetadata.value().size > download.file.size) {
                    const auto expectation = RootedEntryExpectation::matching(tempMetadata.value());
                    auto closed = closeRootedFile(temporary.value().file);
                    if (!closed) {
                        return closed;
                    }
                    auto removed = stagingRoot.value()->removeRegularFile(tempRelativePath, expectation);
                    if (!removed) {
                        return removed;
                    }
                    if (stateStore) {
                        auto cleared = resumeState.clear(resumeKey);
                        if (!cleared) {
                            return Result<void>::fail(cleared.error());
                        }
                    }
                    temporary =
                        stagingRoot.value()->openRegularFile(tempRelativePath, RootedFileOpenMode::OpenOrCreate);
                    if (!temporary) {
                        return Result<void>::fail(temporary.error());
                    }
                    tempMetadata = temporary.value().file->metadata();
                    if (!tempMetadata) {
                        auto error = tempMetadata.error();
                        auto resetClosed = closeRootedFile(temporary.value().file);
                        if (!resetClosed) {
                            error = appendSecondaryError(std::move(error), "failed to close the reset partial download",
                                                         resetClosed.error());
                        }
                        return Result<void>::fail(std::move(error));
                    }
                }
                std::optional<DownloadResumeInfo> resume;
                if (config.network.enableResume && tempMetadata.value().size > 0) {
                    DownloadResumeState resumeRecord;
                    resumeRecord.key = resumeKey;
                    resumeRecord.offset = tempMetadata.value().size;
                    resumeRecord.sha256 = download.file.sha256;
                    if (stateStore) {
                        auto loaded = resumeState.load(resumeKey);
                        if (!loaded) {
                            auto error = loaded.error();
                            auto closed = closeRootedFile(temporary.value().file);
                            if (!closed) {
                                error = appendSecondaryError(std::move(error), "failed to close the partial download",
                                                             closed.error());
                            }
                            return Result<void>::fail(std::move(error));
                        }
                        if (loaded.value() && loaded.value()->sha256 == download.file.sha256) {
                            resumeRecord = loaded.value().value();
                            resumeRecord.offset = tempMetadata.value().size;
                        }
                    }
                    resume = DownloadResumeInfo{resumeRecord.offset, resumeRecord.etag, resumeRecord.lastModified};
                }
                if (!resume) {
                    auto truncated = temporary.value().file->truncate(0);
                    if (!truncated) {
                        auto error = truncated.error();
                        auto closed = closeRootedFile(temporary.value().file);
                        if (!closed) {
                            error = appendSecondaryError(std::move(error), "failed to close the partial download",
                                                         closed.error());
                        }
                        return Result<void>::fail(std::move(error));
                    }
                    auto rewound = temporary.value().file->seek(0);
                    if (!rewound) {
                        auto error = rewound.error();
                        auto closed = closeRootedFile(temporary.value().file);
                        if (!closed) {
                            error = appendSecondaryError(std::move(error), "failed to close the partial download",
                                                         closed.error());
                        }
                        return Result<void>::fail(std::move(error));
                    }
                } else {
                    auto positioned = temporary.value().file->seek(resume->offset);
                    if (!positioned) {
                        auto error = positioned.error();
                        auto closed = closeRootedFile(temporary.value().file);
                        if (!closed) {
                            error = appendSecondaryError(std::move(error), "failed to close the partial download",
                                                         closed.error());
                        }
                        return Result<void>::fail(std::move(error));
                    }
                }

                auto result = downloadWithRedirects(
                    download.url, *temporary.value().file, config.network, download.file.size, policy.value(), network,
                    resume,
                    [&](const Progress& current) {
                        if (progress) {
                            Progress aggregate;
                            const auto currentFileBytes = std::min(current.downloadedBytes, download.file.size);
                            if (!detail::checkedAdd(completedBeforeFile, currentFileBytes, aggregate.downloadedBytes)) {
                                aggregate.downloadedBytes = totalBytes;
                            }
                            aggregate.downloadedBytes = std::min(aggregate.downloadedBytes, totalBytes);
                            aggregate.totalBytes = totalBytes;
                            aggregate.currentFile = download.file.path;
                            progress(aggregate);
                        }
                    },
                    cancel);
                if (!result) {
                    lastError = result.error();
                    auto failedMetadata = temporary.value().file->metadata();
                    if (!failedMetadata) {
                        auto error = appendSecondaryError(lastError, "failed to inspect the partial download",
                                                          failedMetadata.error());
                        auto closed = closeRootedFile(temporary.value().file);
                        if (!closed) {
                            error = appendSecondaryError(std::move(error), "failed to close the partial download",
                                                         closed.error());
                        }
                        return Result<void>::fail(std::move(error));
                    }
                    if (lastError.code == ErrorCode::ResourceLimitExceeded) {
                        const auto expectation = RootedEntryExpectation::matching(failedMetadata.value());
                        auto closed = closeRootedFile(temporary.value().file);
                        if (!closed) {
                            lastError = appendSecondaryError(
                                lastError, "failed to close the oversized partial download", closed.error());
                        }
                        auto removed = stagingRoot.value()->removeRegularFile(tempRelativePath, expectation);
                        if (!removed) {
                            lastError = appendSecondaryError(
                                lastError, "failed to remove the oversized partial download", removed.error());
                        }
                        if (stateStore) {
                            auto cleared = resumeState.clear(resumeKey);
                            if (!cleared) {
                                lastError = appendSecondaryError(lastError, "failed to clear download resume state",
                                                                 cleared.error());
                            }
                        }
                        return Result<void>::fail(lastError);
                    }
                    if (config.network.enableResume && stateStore && failedMetadata.value().size > 0) {
                        DownloadResumeState state;
                        state.key = resumeKey;
                        state.offset = failedMetadata.value().size;
                        state.sha256 = download.file.sha256;
                        // The coordinator may have crossed a redirect or discarded
                        // stale resume metadata before this failure. Persisting the
                        // caller's old validator would bind it to the wrong resource.
                        auto saved = resumeState.save(std::move(state));
                        if (!saved) {
                            auto error =
                                appendSecondaryError(lastError, "failed to save download resume state", saved.error());
                            auto closed = closeRootedFile(temporary.value().file);
                            if (!closed) {
                                error = appendSecondaryError(std::move(error), "failed to close the partial download",
                                                             closed.error());
                            }
                            return Result<void>::fail(std::move(error));
                        }
                    }
                    auto closed = closeRootedFile(temporary.value().file);
                    if (!closed) {
                        return Result<void>::fail(
                            appendSecondaryError(lastError, "failed to close the partial download", closed.error()));
                    }
                } else {
                    auto flushed = temporary.value().file->flush();
                    if (!flushed) {
                        auto error = flushed.error();
                        auto closed = closeRootedFile(temporary.value().file);
                        if (!closed) {
                            error = appendSecondaryError(std::move(error), "failed to close the unflushed download",
                                                         closed.error());
                        }
                        return Result<void>::fail(std::move(error));
                    }
                    auto completedMetadata = temporary.value().file->metadata();
                    if (!completedMetadata) {
                        auto error = completedMetadata.error();
                        auto closed = closeRootedFile(temporary.value().file);
                        if (!closed) {
                            error = appendSecondaryError(std::move(error), "failed to close the completed download",
                                                         closed.error());
                        }
                        return Result<void>::fail(std::move(error));
                    }
                    const auto failAfterDiscardingCompletedDownload = [&](Error primary) {
                        auto closed = closeRootedFile(temporary.value().file);
                        if (!closed) {
                            primary = appendSecondaryError(std::move(primary),
                                                           "failed to close the unpublishable completed download",
                                                           closed.error());
                        }
                        auto removed = stagingRoot.value()->removeRegularFile(
                            tempRelativePath, RootedEntryExpectation::matching(completedMetadata.value()));
                        if (!removed) {
                            primary = appendSecondaryError(std::move(primary),
                                                           "failed to remove the unpublishable completed download",
                                                           removed.error());
                        }
                        if (stateStore) {
                            auto cleared = resumeState.clear(resumeKey);
                            if (!cleared) {
                                primary = appendSecondaryError(
                                    std::move(primary), "failed to clear download resume state", cleared.error());
                            }
                        }
                        return Result<void>::fail(std::move(primary));
                    };
                    bool contentMismatch = completedMetadata.value().size != download.file.size;
                    if (contentMismatch) {
                        lastError = {ErrorCode::HashMismatch, "Downloaded file size mismatch: " + download.file.path};
                    } else {
                        auto hash = hashProvider.sha256Stream(*temporary.value().file);
                        if (!hash) {
                            return failAfterDiscardingCompletedDownload(hash.error());
                        }
                        contentMismatch = hash.value() != download.file.sha256;
                        if (contentMismatch) {
                            lastError = {ErrorCode::HashMismatch,
                                         "Downloaded file SHA-256 mismatch: " + download.file.path};
                        }
                    }
                    if (contentMismatch) {
                        auto closed = closeRootedFile(temporary.value().file);
                        auto removed = stagingRoot.value()->removeRegularFile(
                            tempRelativePath, RootedEntryExpectation::matching(completedMetadata.value()));
                        bool cleanupFailed = false;
                        if (!closed) {
                            lastError = appendSecondaryError(lastError, "failed to close the hash-mismatched download",
                                                             closed.error());
                            cleanupFailed = true;
                        }
                        if (!removed) {
                            lastError = appendSecondaryError(lastError, "failed to remove the hash-mismatched download",
                                                             removed.error());
                            cleanupFailed = true;
                        }
                        if (stateStore) {
                            auto cleared = resumeState.clear(resumeKey);
                            if (!cleared) {
                                lastError = appendSecondaryError(lastError, "failed to clear download resume state",
                                                                 cleared.error());
                                cleanupFailed = true;
                            }
                        }
                        if (cleanupFailed) {
                            return Result<void>::fail(lastError);
                        }
                    } else {
                        auto replaced = stagingRoot.value()->replaceWithOpenedFile(*temporary.value().file,
                                                                                   finalRelativePath, finalExpectation);
                        if (!replaced) {
                            auto replacementError = replaced.error();
                            auto observed =
                                inspectVerifiedDownload(*stagingRoot.value(), finalRelativePath, download.file.size,
                                                        download.file.sha256, hashProvider);
                            if (!observed) {
                                return failAfterDiscardingCompletedDownload(appendSecondaryError(
                                    std::move(replacementError), "failed to reconcile the replacement outcome",
                                    observed.error()));
                            }
                            if (!observed.value()) {
                                return failAfterDiscardingCompletedDownload(std::move(replacementError));
                            }

                            // A rooted commit can report a directory-durability or
                            // cleanup failure after the verified bytes became
                            // visible. Re-publish once against the observed identity
                            // so success has a fresh durability acknowledgement.
                            auto reconciled = stagingRoot.value()->replaceWithOpenedFile(
                                *temporary.value().file, finalRelativePath,
                                RootedEntryExpectation::matching(*observed.value()));
                            if (!reconciled) {
                                return failAfterDiscardingCompletedDownload(appendSecondaryError(
                                    std::move(replacementError), "failed to durably reconcile the published download",
                                    reconciled.error()));
                            }
                            auto closed = closeRootedFile(temporary.value().file);
                            if (!closed) {
                                return Result<void>::fail(closed.error());
                            }
                            if (stateStore) {
                                auto cleared = resumeState.clear(resumeKey);
                                if (!cleared) {
                                    return Result<void>::fail(cleared.error());
                                }
                            }
                            success = true;
                            break;
                        } else {
                            auto closed = closeRootedFile(temporary.value().file);
                            if (!closed) {
                                return Result<void>::fail(closed.error());
                            }
                            if (stateStore) {
                                auto cleared = resumeState.clear(resumeKey);
                                if (!cleared) {
                                    return Result<void>::fail(cleared.error());
                                }
                            }
                            success = true;
                            break;
                        }
                    }
                }

                if (attempt < config.retry.maxRetries) {
                    std::this_thread::sleep_for(config.retry.retryDelay);
                }
            }

            if (!success) {
                return Result<void>::fail(lastError);
            }

            completedBeforeFile += download.file.size;
            if (progress) {
                progress({completedBeforeFile, totalBytes, download.file.path});
            }
        }

        return Result<void>::ok();
    }();

    auto flushedResume = resumeState.flush();
    if (!execution) {
        auto error = execution.error();
        if (!flushedResume) {
            error =
                appendSecondaryError(std::move(error), "failed to flush download resume state", flushedResume.error());
        }
        return Result<void>::fail(std::move(error));
    }
    if (!flushedResume) {
        return Result<void>::fail(flushedResume.error());
    }
    return Result<void>::ok();
}

} // namespace autoupdater
