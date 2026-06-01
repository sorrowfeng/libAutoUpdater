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

Result<void> executeDownloads(const Config& config,
                              const UpdateDecision& decision,
                              INetworkClient& network,
                              IFileSystem& fileSystem,
                              IHashProvider& hashProvider,
                              ProgressCallback progress,
                              CancellationToken& cancel) {
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
            fileSystem.remove(tempPath);

            auto result = network.downloadToFile(
                download.url,
                tempPath,
                config.network,
                std::nullopt,
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
            } else {
                auto hash = hashProvider.sha256File(tempPath);
                if (!hash) {
                    lastError = hash.error();
                } else if (hash.value() != download.file.sha256) {
                    lastError = {ErrorCode::HashMismatch, "Downloaded file SHA-256 mismatch: " + download.file.path};
                } else {
                    auto replaced = fileSystem.renameOrReplace(tempPath, download.stagingPath);
                    if (!replaced) {
                        lastError = replaced.error();
                    } else {
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

