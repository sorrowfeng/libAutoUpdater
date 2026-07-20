#pragma once

#include "libAutoUpdater/interfaces/IFileSystem.h"

#include <atomic>
#include <memory>
#include <string>
#include <utility>

namespace autoupdater::test {

struct CommitAcknowledgementFault {
    std::string target;
    bool reportUnknownPublication = false;
    bool failureCanBeReconciled = true;
    std::atomic_bool consumed{false};

    bool consume(const std::string& candidate) noexcept {
        if (candidate != target) {
            return false;
        }
        bool expected = false;
        return consumed.compare_exchange_strong(expected, true);
    }
};

class CommitFaultTemporaryFile final : public IRootedTemporaryFile {
  public:
    CommitFaultTemporaryFile(std::unique_ptr<IRootedTemporaryFile> inner,
                             std::shared_ptr<CommitAcknowledgementFault> fault, std::string target)
        : inner_(std::move(inner)), fault_(std::move(fault)), target_(std::move(target)) {}

    IRootedFile& file() noexcept override {
        return inner_->file();
    }

    Result<void> commit(const RootedEntryExpectation& expectation) noexcept override {
        auto committed = inner_->commit(expectation);
        if (committed && fault_->consume(target_)) {
            injectedAcknowledgementFailure_ = true;
            reportUnknownPublication_ = fault_->reportUnknownPublication;
            return Result<void>::fail(
                {ErrorCode::FileSystemError, "Injected post-publish commit acknowledgement failure"});
        }
        return committed;
    }

    RootedPublishStatus publishStatus() const noexcept override {
        auto status = inner_->publishStatus();
        if (reportUnknownPublication_) {
            status.publication = RootedPublication::Unknown;
        }
        if (injectedAcknowledgementFailure_) {
            status.failureCanBeReconciled = fault_->failureCanBeReconciled;
        }
        return status;
    }

    Result<void> discard() noexcept override {
        return inner_->discard();
    }

  private:
    std::unique_ptr<IRootedTemporaryFile> inner_;
    std::shared_ptr<CommitAcknowledgementFault> fault_;
    std::string target_;
    bool injectedAcknowledgementFailure_ = false;
    bool reportUnknownPublication_ = false;
};

class CommitFaultRootedDirectory final : public IRootedDirectory {
  public:
    CommitFaultRootedDirectory(std::unique_ptr<IRootedDirectory> inner,
                               std::shared_ptr<CommitAcknowledgementFault> fault)
        : inner_(std::move(inner)), fault_(std::move(fault)) {}

    Result<RootedOpenResult> openRegularFile(const std::string& relativePath, RootedFileOpenMode mode,
                                             RootedDirectoryCreationMode directoryMode) noexcept override {
        return inner_->openRegularFile(relativePath, mode, directoryMode);
    }

    Result<std::unique_ptr<IRootedTemporaryFile>>
    createAtomicReplacement(const std::string& relativePath,
                            RootedDirectoryCreationMode directoryMode) noexcept override {
        auto temporary = inner_->createAtomicReplacement(relativePath, directoryMode);
        if (!temporary) {
            return Result<std::unique_ptr<IRootedTemporaryFile>>::fail(temporary.error());
        }
        auto wrapped = std::make_unique<CommitFaultTemporaryFile>(std::move(temporary.value()), fault_, relativePath);
        return Result<std::unique_ptr<IRootedTemporaryFile>>::ok(std::move(wrapped));
    }

    Result<void> replaceWithOpenedFile(IRootedFile& source, const std::string& relativePath,
                                       const RootedEntryExpectation& expectation) noexcept override {
        return inner_->replaceWithOpenedFile(source, relativePath, expectation);
    }

    Result<void> removeRegularFile(const std::string& relativePath,
                                   const RootedEntryExpectation& expectation) noexcept override {
        return inner_->removeRegularFile(relativePath, expectation);
    }

    Result<std::unique_ptr<IRootedLock>> acquireExclusiveLock(const std::string& relativePath) noexcept override {
        return inner_->acquireExclusiveLock(relativePath);
    }

  private:
    std::unique_ptr<IRootedDirectory> inner_;
    std::shared_ptr<CommitAcknowledgementFault> fault_;
};

class CommitFaultFileSystem final : public IFileSystem {
  public:
    CommitFaultFileSystem(std::shared_ptr<IFileSystem> inner, std::shared_ptr<CommitAcknowledgementFault> fault)
        : inner_(std::move(inner)), fault_(std::move(fault)) {}

    bool exists(const std::filesystem::path& path) noexcept override {
        return inner_->exists(path);
    }
    bool isRegularFile(const std::filesystem::path& path) noexcept override {
        return inner_->isRegularFile(path);
    }
    Result<std::uint64_t> fileSize(const std::filesystem::path& path) noexcept override {
        return inner_->fileSize(path);
    }
    Result<void> createDirectories(const std::filesystem::path& path) noexcept override {
        return inner_->createDirectories(path);
    }
    Result<void> copyFile(const std::filesystem::path& from, const std::filesystem::path& to,
                          bool overwrite) noexcept override {
        return inner_->copyFile(from, to, overwrite);
    }
    Result<void> renameOrReplace(const std::filesystem::path& from, const std::filesystem::path& to) noexcept override {
        return inner_->renameOrReplace(from, to);
    }
    Result<void> remove(const std::filesystem::path& path) noexcept override {
        return inner_->remove(path);
    }
    Result<void> removeAll(const std::filesystem::path& path) noexcept override {
        return inner_->removeAll(path);
    }
    Result<std::string> readText(const std::filesystem::path& path, std::uint64_t maxBytes) noexcept override {
        return inner_->readText(path, maxBytes);
    }
    Result<void> writeText(const std::filesystem::path& path, const std::string& text) noexcept override {
        return inner_->writeText(path, text);
    }
    Result<std::unique_ptr<IRootedDirectory>> openRoot(const std::filesystem::path& path, RootAccess access,
                                                       bool create,
                                                       RootedDirectoryCreationMode directoryMode) noexcept override {
        auto root = inner_->openRoot(path, access, create, directoryMode);
        if (!root) {
            return Result<std::unique_ptr<IRootedDirectory>>::fail(root.error());
        }
        auto wrapped = std::make_unique<CommitFaultRootedDirectory>(std::move(root.value()), fault_);
        return Result<std::unique_ptr<IRootedDirectory>>::ok(std::move(wrapped));
    }

  private:
    std::shared_ptr<IFileSystem> inner_;
    std::shared_ptr<CommitAcknowledgementFault> fault_;
};

} // namespace autoupdater::test
