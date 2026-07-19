#include "DownloadExecutor.h"

#include "NetworkRequest.h"
#include "UrlPolicy.h"

#include <chrono>
#include <thread>

namespace autoupdater {

namespace {

std::string temporaryDownloadPath(const std::string& target) {
    return target + ".download";
}

} // namespace

Result<void> executeDownloads(const Config& config, const UpdateDecision& decision, INetworkClient& network,
                              IFileSystem& fileSystem, IHashProvider& hashProvider, IStateStore* stateStore,
                              ProgressCallback progress, CancellationToken& cancel) {
    std::uint64_t totalBytes = 0;
    for (const auto& download : decision.downloads) {
        totalBytes += download.file.size;
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
                return Result<void>::fail(existingMetadata.error());
            }
            finalExpectation = RootedEntryExpectation::matching(existingMetadata.value());
            auto hash = hashProvider.sha256Stream(*existing.value().file);
            if (!hash) {
                return Result<void>::fail(hash.error());
            }
            if (existingMetadata.value().size == download.file.size && hash.value() == download.file.sha256) {
                completedBeforeFile += download.file.size;
                if (progress) {
                    progress({completedBeforeFile, totalBytes, download.file.path});
                }
                continue;
            }
        }
        existing.value().file.reset();

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
                return Result<void>::fail(tempMetadata.error());
            }
            std::optional<DownloadResumeInfo> resume;
            if (config.network.enableResume && tempMetadata.value().size > 0) {
                DownloadResumeState resumeState;
                resumeState.key = download.url;
                resumeState.offset = tempMetadata.value().size;
                resumeState.sha256 = download.file.sha256;
                if (stateStore) {
                    auto loaded = stateStore->loadDownloadResume(download.url);
                    if (loaded && loaded.value() && loaded.value()->sha256 == download.file.sha256) {
                        resumeState = loaded.value().value();
                        resumeState.offset = tempMetadata.value().size;
                    }
                }
                resume = DownloadResumeInfo{resumeState.offset, resumeState.etag, resumeState.lastModified};
            }
            if (!resume) {
                auto truncated = temporary.value().file->truncate(0);
                if (!truncated) {
                    return truncated;
                }
                auto rewound = temporary.value().file->seek(0);
                if (!rewound) {
                    return rewound;
                }
            } else {
                auto positioned = temporary.value().file->seek(resume->offset);
                if (!positioned) {
                    return positioned;
                }
            }

            auto result = downloadWithRedirects(
                download.url, *temporary.value().file, config.network, policy.value(), network, resume,
                [&](const Progress& current) {
                    if (progress) {
                        Progress aggregate;
                        aggregate.downloadedBytes = completedBeforeFile + current.downloadedBytes;
                        aggregate.totalBytes = totalBytes;
                        aggregate.currentFile = download.file.path;
                        progress(aggregate);
                    }
                },
                cancel);
            if (!result) {
                lastError = result.error();
                auto failedMetadata = temporary.value().file->metadata();
                if (config.network.enableResume && stateStore && failedMetadata && failedMetadata.value().size > 0) {
                    DownloadResumeState state;
                    state.key = download.url;
                    state.offset = failedMetadata.value().size;
                    state.sha256 = download.file.sha256;
                    // The coordinator may have crossed a redirect or discarded
                    // stale resume metadata before this failure. Persisting the
                    // caller's old validator would bind it to the wrong resource.
                    stateStore->saveDownloadResume(state);
                }
            } else {
                auto flushed = temporary.value().file->flush();
                if (!flushed) {
                    lastError = flushed.error();
                    continue;
                }
                auto completedMetadata = temporary.value().file->metadata();
                if (!completedMetadata) {
                    lastError = completedMetadata.error();
                    continue;
                }
                if (stateStore) {
                    DownloadResumeState state;
                    state.key = download.url;
                    state.offset = completedMetadata.value().size;
                    if (result.value().effectiveUrl == canonicalDownloadUrl.value().canonical) {
                        state.etag = result.value().download.etag;
                        state.lastModified = result.value().download.lastModified;
                    }
                    state.sha256 = download.file.sha256;
                    stateStore->saveDownloadResume(state);
                }
                auto hash = hashProvider.sha256Stream(*temporary.value().file);
                if (!hash) {
                    lastError = hash.error();
                } else if (completedMetadata.value().size != download.file.size ||
                           hash.value() != download.file.sha256) {
                    lastError = {ErrorCode::HashMismatch, "Downloaded file SHA-256 mismatch: " + download.file.path};
                    temporary.value().file.reset();
                    auto removed = stagingRoot.value()->removeRegularFile(
                        tempRelativePath, RootedEntryExpectation::matching(completedMetadata.value()));
                    if (!removed) {
                        lastError = removed.error();
                    }
                    if (stateStore) {
                        stateStore->clearDownloadResume(download.url);
                    }
                } else {
                    auto replaced = stagingRoot.value()->replaceWithOpenedFile(*temporary.value().file,
                                                                               finalRelativePath, finalExpectation);
                    if (!replaced) {
                        lastError = replaced.error();
                    } else {
                        temporary.value().file.reset();
                        if (stateStore) {
                            stateStore->clearDownloadResume(download.url);
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
