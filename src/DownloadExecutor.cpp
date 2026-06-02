#include "DownloadExecutor.h"

#include <chrono>
#include <thread>

namespace autoupdater {

namespace {

std::filesystem::path temporaryDownloadPath(const std::filesystem::path& target) {
    auto path = target;
    path += ".download";
    return path;
}

} // namespace

Result<void> executeDownloads(const Config& config, const UpdateDecision& decision, INetworkClient& network,
                              IFileSystem& fileSystem, IHashProvider& hashProvider, IStateStore* stateStore,
                              ProgressCallback progress, CancellationToken& cancel) {
    std::uint64_t totalBytes = 0;
    for (const auto& download : decision.downloads) {
        totalBytes += download.file.size;
    }

    std::uint64_t completedBeforeFile = 0;
    for (const auto& download : decision.downloads) {
        if (cancel.isCancelled()) {
            return Result<void>::fail({ErrorCode::Cancelled, "Download cancelled"});
        }

        const auto parent = download.stagingPath.parent_path();
        auto mkdir = fileSystem.createDirectories(parent);
        if (!mkdir) {
            return mkdir;
        }

        if (fileSystem.exists(download.stagingPath)) {
            auto hash = hashProvider.sha256File(download.stagingPath);
            if (hash && hash.value() == download.file.sha256) {
                completedBeforeFile += download.file.size;
                if (progress) {
                    progress({completedBeforeFile, totalBytes, download.file.path});
                }
                continue;
            }
        }

        Error lastError{ErrorCode::DownloadFailed, "Download failed"};
        bool success = false;
        for (int attempt = 0; attempt <= config.retry.maxRetries; ++attempt) {
            if (cancel.isCancelled()) {
                return Result<void>::fail({ErrorCode::Cancelled, "Download cancelled"});
            }

            const auto tempPath = temporaryDownloadPath(download.stagingPath);
            std::optional<DownloadResumeInfo> resume;
            if (config.network.enableResume && fileSystem.exists(tempPath)) {
                auto tempSize = fileSystem.fileSize(tempPath);
                if (tempSize && tempSize.value() > 0) {
                    DownloadResumeState resumeState;
                    resumeState.key = download.url;
                    resumeState.offset = tempSize.value();
                    resumeState.sha256 = download.file.sha256;
                    if (stateStore) {
                        auto loaded = stateStore->loadDownloadResume(download.url);
                        if (loaded && loaded.value() && loaded.value()->sha256 == download.file.sha256) {
                            resumeState = loaded.value().value();
                            resumeState.offset = tempSize.value();
                        }
                    }
                    resume = DownloadResumeInfo{resumeState.offset, resumeState.etag, resumeState.lastModified};
                }
            }
            if (!resume) {
                fileSystem.remove(tempPath);
            }

            auto result = network.downloadToFile(
                download.url, tempPath, config.network, resume,
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
                if (config.network.enableResume && stateStore && fileSystem.exists(tempPath)) {
                    auto tempSize = fileSystem.fileSize(tempPath);
                    if (tempSize && tempSize.value() > 0) {
                        DownloadResumeState state;
                        state.key = download.url;
                        state.offset = tempSize.value();
                        state.sha256 = download.file.sha256;
                        if (resume) {
                            state.etag = resume->etag;
                            state.lastModified = resume->lastModified;
                        }
                        stateStore->saveDownloadResume(state);
                    }
                }
            } else {
                if (stateStore) {
                    DownloadResumeState state;
                    state.key = download.url;
                    state.offset = result.value().bytesWritten;
                    state.etag = result.value().etag;
                    state.lastModified = result.value().lastModified;
                    state.sha256 = download.file.sha256;
                    stateStore->saveDownloadResume(state);
                }
                auto hash = hashProvider.sha256File(tempPath);
                if (!hash) {
                    lastError = hash.error();
                } else if (hash.value() != download.file.sha256) {
                    lastError = {ErrorCode::HashMismatch, "Downloaded file SHA-256 mismatch: " + download.file.path};
                    fileSystem.remove(tempPath);
                    if (stateStore) {
                        stateStore->clearDownloadResume(download.url);
                    }
                } else {
                    auto replaced = fileSystem.renameOrReplace(tempPath, download.stagingPath);
                    if (!replaced) {
                        lastError = replaced.error();
                    } else {
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
