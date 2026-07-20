#include "DownloadExecutor.h"

#include "NetworkLimits.h"
#include "NetworkRequest.h"
#include "UrlPolicy.h"

#include <algorithm>
#include <chrono>
#include <thread>

namespace autoupdater {

namespace {

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

    auto stagingRoot = fileSystem.openRoot(config.tempDir, RootAccess::ReadWrite, true);
    if (!stagingRoot) {
        return Result<void>::fail(stagingRoot.error());
    }

    std::uint64_t completedBeforeFile = 0;
    for (const auto& download : decision.downloads) {
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
                    error =
                        appendSecondaryError(std::move(error), "failed to close the existing download", closed.error());
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
                        auto cleared = stateStore->clearDownloadResume(download.url);
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

            auto temporary = stagingRoot.value()->openRegularFile(tempRelativePath, RootedFileOpenMode::OpenOrCreate);
            if (!temporary) {
                return Result<void>::fail(temporary.error());
            }
            auto tempMetadata = temporary.value().file->metadata();
            if (!tempMetadata) {
                auto error = tempMetadata.error();
                auto closed = closeRootedFile(temporary.value().file);
                if (!closed) {
                    error =
                        appendSecondaryError(std::move(error), "failed to close the partial download", closed.error());
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
                    auto cleared = stateStore->clearDownloadResume(download.url);
                    if (!cleared) {
                        return Result<void>::fail(cleared.error());
                    }
                }
                temporary = stagingRoot.value()->openRegularFile(tempRelativePath, RootedFileOpenMode::OpenOrCreate);
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
                DownloadResumeState resumeState;
                resumeState.key = download.url;
                resumeState.offset = tempMetadata.value().size;
                resumeState.sha256 = download.file.sha256;
                if (stateStore) {
                    auto loaded = stateStore->loadDownloadResume(download.url);
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
                        resumeState = loaded.value().value();
                        resumeState.offset = tempMetadata.value().size;
                    }
                }
                resume = DownloadResumeInfo{resumeState.offset, resumeState.etag, resumeState.lastModified};
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
                        lastError = appendSecondaryError(lastError, "failed to close the oversized partial download",
                                                         closed.error());
                    }
                    auto removed = stagingRoot.value()->removeRegularFile(tempRelativePath, expectation);
                    if (!removed) {
                        lastError = appendSecondaryError(lastError, "failed to remove the oversized partial download",
                                                         removed.error());
                    }
                    if (stateStore) {
                        auto cleared = stateStore->clearDownloadResume(download.url);
                        if (!cleared) {
                            lastError = appendSecondaryError(lastError, "failed to clear download resume state",
                                                             cleared.error());
                        }
                    }
                    return Result<void>::fail(lastError);
                }
                if (config.network.enableResume && stateStore && failedMetadata.value().size > 0) {
                    DownloadResumeState state;
                    state.key = download.url;
                    state.offset = failedMetadata.value().size;
                    state.sha256 = download.file.sha256;
                    // The coordinator may have crossed a redirect or discarded
                    // stale resume metadata before this failure. Persisting the
                    // caller's old validator would bind it to the wrong resource.
                    auto saved = stateStore->saveDownloadResume(state);
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
                        primary = appendSecondaryError(
                            std::move(primary), "failed to close the unpublishable completed download", closed.error());
                    }
                    auto removed = stagingRoot.value()->removeRegularFile(
                        tempRelativePath, RootedEntryExpectation::matching(completedMetadata.value()));
                    if (!removed) {
                        primary = appendSecondaryError(std::move(primary),
                                                       "failed to remove the unpublishable completed download",
                                                       removed.error());
                    }
                    if (stateStore) {
                        auto cleared = stateStore->clearDownloadResume(download.url);
                        if (!cleared) {
                            primary = appendSecondaryError(std::move(primary), "failed to clear download resume state",
                                                           cleared.error());
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
                        auto cleared = stateStore->clearDownloadResume(download.url);
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
                        auto observed = inspectVerifiedDownload(*stagingRoot.value(), finalRelativePath,
                                                                download.file.size, download.file.sha256, hashProvider);
                        if (!observed) {
                            return failAfterDiscardingCompletedDownload(
                                appendSecondaryError(std::move(replacementError),
                                                     "failed to reconcile the replacement outcome", observed.error()));
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
                            auto cleared = stateStore->clearDownloadResume(download.url);
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
                            auto cleared = stateStore->clearDownloadResume(download.url);
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
}

} // namespace autoupdater
