#include "TestCommon.h"

#include "ApplyExecutor.h"
#include "ApplyJournal.h"
#include "libAutoUpdater/interfaces/IFileSystem.h"
#include "libAutoUpdater/interfaces/IHashProvider.h"
#include "libAutoUpdater/interfaces/IProcessLauncher.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace {

enum class TransactionFault {
    Read,
    ShortWrite,
    DiskFull,
    Permission,
    Flush,
    Rename,
    RemoveBefore,
    RemoveAfter,
};

std::uint64_t currentProcessId() {
#ifdef _WIN32
    return static_cast<std::uint64_t>(GetCurrentProcessId());
#else
    return static_cast<std::uint64_t>(getpid());
#endif
}

std::string safeName(std::string value) {
    std::replace_if(
        value.begin(), value.end(),
        [](char character) {
            return !((character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
                     (character >= '0' && character <= '9'));
        },
        '_');
    return value;
}

class TemporaryDirectory final {
  public:
    explicit TemporaryDirectory(const std::string& name) {
        static std::atomic_uint64_t sequence{0};
        path_ = std::filesystem::temp_directory_path() / "libAutoUpdater-transaction-faults" /
                (std::to_string(currentProcessId()) + "-" +
                 std::to_string(sequence.fetch_add(1, std::memory_order_relaxed)) + "-" + safeName(name));
        std::error_code error;
        std::filesystem::remove_all(path_, error);
        error.clear();
        std::filesystem::create_directories(path_, error);
        LAU_REQUIRE(!error);
    }

    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    const std::filesystem::path& path() const noexcept {
        return path_;
    }

  private:
    std::filesystem::path path_;
};

void writeFile(const std::filesystem::path& path, const std::string& contents) {
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    LAU_REQUIRE(!error);
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    LAU_REQUIRE(output.good());
    output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    LAU_REQUIRE(output.good());
}

std::string readFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    LAU_REQUIRE(input.good());
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

autoupdater::Error injectedError(TransactionFault fault) {
    const char* name = "unknown";
    switch (fault) {
    case TransactionFault::Read:
        name = "read";
        break;
    case TransactionFault::ShortWrite:
        name = "short-write";
        break;
    case TransactionFault::DiskFull:
        name = "disk-full";
        break;
    case TransactionFault::Permission:
        name = "permission";
        break;
    case TransactionFault::Flush:
        name = "flush";
        break;
    case TransactionFault::Rename:
        name = "rename";
        break;
    case TransactionFault::RemoveBefore:
        name = "remove-before";
        break;
    case TransactionFault::RemoveAfter:
        name = "remove-after";
        break;
    }
    return {autoupdater::ErrorCode::FileSystemError, std::string("Injected TEST-002 ") + name + " failure"};
}

struct FaultState {
    TransactionFault fault = TransactionFault::Read;
    std::filesystem::path root;
    std::string relativePath;
    std::size_t occurrence = 1;
    std::size_t matches = 0;
    bool consumed = false;

    bool consume(TransactionFault candidate, const std::filesystem::path& candidateRoot,
                 const std::string& candidatePath) noexcept {
        if (consumed || candidate != fault || candidateRoot.lexically_normal() != root.lexically_normal() ||
            candidatePath != relativePath) {
            return false;
        }
        ++matches;
        if (matches != occurrence) {
            return false;
        }
        consumed = true;
        return true;
    }
};

class FaultingRootedFile final : public autoupdater::IRootedFile {
  public:
    FaultingRootedFile(std::unique_ptr<autoupdater::IRootedFile> inner, std::shared_ptr<FaultState> fault,
                       std::filesystem::path root, std::string relativePath)
        : owned_(std::move(inner)), inner_(owned_.get()), fault_(std::move(fault)), root_(std::move(root)),
          relativePath_(std::move(relativePath)) {}

    FaultingRootedFile(autoupdater::IRootedFile& inner, std::shared_ptr<FaultState> fault, std::filesystem::path root,
                       std::string relativePath)
        : inner_(&inner), fault_(std::move(fault)), root_(std::move(root)), relativePath_(std::move(relativePath)) {}

    autoupdater::Result<std::size_t> read(void* buffer, std::size_t size) noexcept override {
        if (fault_->consume(TransactionFault::Read, root_, relativePath_)) {
            return autoupdater::Result<std::size_t>::fail(injectedError(TransactionFault::Read));
        }
        return inner_->read(buffer, size);
    }

    autoupdater::Result<void> write(const void* data, std::size_t size) noexcept override {
        if (fault_->consume(TransactionFault::ShortWrite, root_, relativePath_)) {
            const auto partialSize = size == 0 ? 0 : (std::max)(std::size_t{1}, size / 2);
            if (partialSize != 0) {
                auto partial = inner_->write(data, partialSize);
                if (!partial) {
                    return partial;
                }
            }
            return autoupdater::Result<void>::fail(injectedError(TransactionFault::ShortWrite));
        }
        if (fault_->consume(TransactionFault::DiskFull, root_, relativePath_)) {
            return autoupdater::Result<void>::fail(injectedError(TransactionFault::DiskFull));
        }
        return inner_->write(data, size);
    }

    autoupdater::Result<void> seek(std::uint64_t offset) noexcept override {
        return inner_->seek(offset);
    }

    autoupdater::Result<void> truncate(std::uint64_t size) noexcept override {
        return inner_->truncate(size);
    }

    autoupdater::Result<void> flush() noexcept override {
        if (fault_->consume(TransactionFault::Flush, root_, relativePath_)) {
            return autoupdater::Result<void>::fail(injectedError(TransactionFault::Flush));
        }
        return inner_->flush();
    }

    autoupdater::Result<autoupdater::RootedFileMetadata> metadata() noexcept override {
        return inner_->metadata();
    }

    autoupdater::Result<void> setPermissions(std::filesystem::perms permissions) noexcept override {
        if (fault_->consume(TransactionFault::Permission, root_, relativePath_)) {
            return autoupdater::Result<void>::fail(injectedError(TransactionFault::Permission));
        }
        return inner_->setPermissions(permissions);
    }

    autoupdater::Result<void> copyPermissionsFrom(autoupdater::IRootedFile& source) noexcept override {
        if (fault_->consume(TransactionFault::Permission, root_, relativePath_)) {
            return autoupdater::Result<void>::fail(injectedError(TransactionFault::Permission));
        }
        auto* wrappedSource = dynamic_cast<FaultingRootedFile*>(&source);
        return inner_->copyPermissionsFrom(wrappedSource ? wrappedSource->inner() : source);
    }

    autoupdater::Result<void> close() noexcept override {
        return inner_->close();
    }

    autoupdater::IRootedFile& inner() noexcept {
        return *inner_;
    }

  private:
    std::unique_ptr<autoupdater::IRootedFile> owned_;
    autoupdater::IRootedFile* inner_ = nullptr;
    std::shared_ptr<FaultState> fault_;
    std::filesystem::path root_;
    std::string relativePath_;
};

class FaultingTemporaryFile final : public autoupdater::IRootedTemporaryFile {
  public:
    FaultingTemporaryFile(std::unique_ptr<autoupdater::IRootedTemporaryFile> inner, std::shared_ptr<FaultState> fault,
                          std::filesystem::path root, std::string relativePath)
        : inner_(std::move(inner)), fault_(std::move(fault)), root_(std::move(root)),
          relativePath_(std::move(relativePath)), file_(inner_->file(), fault_, root_, relativePath_) {}

    autoupdater::IRootedFile& file() noexcept override {
        return file_;
    }

    autoupdater::Result<void> commit(const autoupdater::RootedEntryExpectation& expectation) noexcept override {
        // Production temporary-file implementations perform their durability
        // barrier inside commit(), directly on the owned file. Injecting here
        // therefore exercises a pre-publication flush failure without relying
        // on platform-specific ENOSPC or fsync behavior.
        if (fault_->consume(TransactionFault::Flush, root_, relativePath_)) {
            return autoupdater::Result<void>::fail(injectedError(TransactionFault::Flush));
        }
        if (fault_->consume(TransactionFault::Rename, root_, relativePath_)) {
            return autoupdater::Result<void>::fail(injectedError(TransactionFault::Rename));
        }
        return inner_->commit(expectation);
    }

    autoupdater::RootedPublishStatus publishStatus() const noexcept override {
        return inner_->publishStatus();
    }

    autoupdater::Result<void> discard() noexcept override {
        return inner_->discard();
    }

  private:
    std::unique_ptr<autoupdater::IRootedTemporaryFile> inner_;
    std::shared_ptr<FaultState> fault_;
    std::filesystem::path root_;
    std::string relativePath_;
    FaultingRootedFile file_;
};

class FaultingRootedDirectory final : public autoupdater::IRootedDirectory {
  public:
    FaultingRootedDirectory(std::unique_ptr<autoupdater::IRootedDirectory> inner, std::shared_ptr<FaultState> fault,
                            std::filesystem::path root)
        : inner_(std::move(inner)), fault_(std::move(fault)), root_(std::move(root)) {}

    autoupdater::Result<autoupdater::RootedOpenResult>
    openRegularFile(const std::string& relativePath, autoupdater::RootedFileOpenMode mode,
                    autoupdater::RootedDirectoryCreationMode directoryMode) noexcept override {
        auto opened = inner_->openRegularFile(relativePath, mode, directoryMode);
        if (!opened || !opened.value().exists()) {
            return opened;
        }
        opened.value().file =
            std::make_unique<FaultingRootedFile>(std::move(opened.value().file), fault_, root_, relativePath);
        return opened;
    }

    autoupdater::Result<std::unique_ptr<autoupdater::IRootedTemporaryFile>>
    createAtomicReplacement(const std::string& relativePath,
                            autoupdater::RootedDirectoryCreationMode directoryMode) noexcept override {
        auto temporary = inner_->createAtomicReplacement(relativePath, directoryMode);
        if (!temporary) {
            return temporary;
        }
        std::unique_ptr<autoupdater::IRootedTemporaryFile> wrapped =
            std::make_unique<FaultingTemporaryFile>(std::move(temporary.value()), fault_, root_, relativePath);
        return autoupdater::Result<std::unique_ptr<autoupdater::IRootedTemporaryFile>>::ok(std::move(wrapped));
    }

    autoupdater::Result<void>
    replaceWithOpenedFile(autoupdater::IRootedFile& source, const std::string& relativePath,
                          const autoupdater::RootedEntryExpectation& expectation) noexcept override {
        if (fault_->consume(TransactionFault::Rename, root_, relativePath)) {
            return autoupdater::Result<void>::fail(injectedError(TransactionFault::Rename));
        }
        return inner_->replaceWithOpenedFile(source, relativePath, expectation);
    }

    autoupdater::Result<void>
    removeRegularFile(const std::string& relativePath,
                      const autoupdater::RootedEntryExpectation& expectation) noexcept override {
        if (fault_->consume(TransactionFault::RemoveBefore, root_, relativePath)) {
            return autoupdater::Result<void>::fail(injectedError(TransactionFault::RemoveBefore));
        }
        auto removed = inner_->removeRegularFile(relativePath, expectation);
        if (removed && fault_->consume(TransactionFault::RemoveAfter, root_, relativePath)) {
            return autoupdater::Result<void>::fail(injectedError(TransactionFault::RemoveAfter));
        }
        return removed;
    }

    autoupdater::Result<std::unique_ptr<autoupdater::IRootedLock>>
    acquireExclusiveLock(const std::string& relativePath) noexcept override {
        return inner_->acquireExclusiveLock(relativePath);
    }

  private:
    std::unique_ptr<autoupdater::IRootedDirectory> inner_;
    std::shared_ptr<FaultState> fault_;
    std::filesystem::path root_;
};

class FaultingFileSystem final : public autoupdater::IFileSystem {
  public:
    FaultingFileSystem(std::shared_ptr<autoupdater::IFileSystem> inner, std::shared_ptr<FaultState> fault)
        : inner_(std::move(inner)), fault_(std::move(fault)) {}

    bool exists(const std::filesystem::path& path) noexcept override {
        return inner_->exists(path);
    }

    bool isRegularFile(const std::filesystem::path& path) noexcept override {
        return inner_->isRegularFile(path);
    }

    autoupdater::Result<std::uint64_t> fileSize(const std::filesystem::path& path) noexcept override {
        return inner_->fileSize(path);
    }

    autoupdater::Result<void> createDirectories(const std::filesystem::path& path) noexcept override {
        return inner_->createDirectories(path);
    }

    autoupdater::Result<void> copyFile(const std::filesystem::path& from, const std::filesystem::path& to,
                                       bool overwrite) noexcept override {
        return inner_->copyFile(from, to, overwrite);
    }

    autoupdater::Result<void> renameOrReplace(const std::filesystem::path& from,
                                              const std::filesystem::path& to) noexcept override {
        return inner_->renameOrReplace(from, to);
    }

    autoupdater::Result<void> remove(const std::filesystem::path& path) noexcept override {
        return inner_->remove(path);
    }

    autoupdater::Result<void> removeAll(const std::filesystem::path& path) noexcept override {
        return inner_->removeAll(path);
    }

    autoupdater::Result<std::string> readText(const std::filesystem::path& path,
                                              std::uint64_t maxBytes) noexcept override {
        return inner_->readText(path, maxBytes);
    }

    autoupdater::Result<void> writeText(const std::filesystem::path& path, const std::string& text) noexcept override {
        return inner_->writeText(path, text);
    }

    autoupdater::Result<std::unique_ptr<autoupdater::IRootedDirectory>>
    openRoot(const std::filesystem::path& path, autoupdater::RootAccess access, bool create,
             autoupdater::RootedDirectoryCreationMode directoryMode) noexcept override {
        auto opened = inner_->openRoot(path, access, create, directoryMode);
        if (!opened) {
            return opened;
        }
        std::unique_ptr<autoupdater::IRootedDirectory> wrapped =
            std::make_unique<FaultingRootedDirectory>(std::move(opened.value()), fault_, path);
        return autoupdater::Result<std::unique_ptr<autoupdater::IRootedDirectory>>::ok(std::move(wrapped));
    }

  private:
    std::shared_ptr<autoupdater::IFileSystem> inner_;
    std::shared_ptr<FaultState> fault_;
};

struct ApplyFixture {
    TemporaryDirectory temporary;
    std::filesystem::path installDir;
    std::filesystem::path stagingDir;
    std::filesystem::path backupDir;
    std::shared_ptr<autoupdater::IHashProvider> hashProvider;
    autoupdater::ApplyPlan plan;

    explicit ApplyFixture(const std::string& name, bool removeOperation = false)
        : temporary(name), installDir(temporary.path() / "install"), stagingDir(temporary.path() / "staging"),
          backupDir(temporary.path() / "backup"), hashProvider(autoupdater::createDefaultHashProvider()) {
        constexpr std::string_view kOldContents = "old-transaction-content";
        constexpr std::string_view kNewContents = "new-transaction-content";
        writeFile(installDir / "bin/app.txt", std::string(kOldContents));
        if (!removeOperation) {
            writeFile(stagingDir / "bin/app.txt", std::string(kNewContents));
        }

        plan.appId = "com.example.transaction-faults";
        plan.fromVersion = "1.0.0";
        plan.toVersion = "2.0.0";
        plan.releaseId = "test-002-" + safeName(name);
        plan.manifestSha256 = std::string(64, 'a');
        plan.installDir = installDir;
        plan.stagingDir = stagingDir;
        plan.backupDir = backupDir;
        if (removeOperation) {
            plan.operations.push_back({autoupdater::ApplyOperationType::Remove, "", "bin/app.txt", "", 0});
        } else {
            const auto hash = hashProvider->sha256Bytes(std::string(kNewContents));
            LAU_REQUIRE(hash);
            plan.operations.push_back({autoupdater::ApplyOperationType::Replace, "bin/app.txt", "bin/app.txt",
                                       hash.value(), kNewContents.size()});
        }
    }
};

autoupdater::updater::ApplyExecutorDependencies
dependencies(const std::shared_ptr<autoupdater::IHashProvider>& hashProvider,
             const std::shared_ptr<FaultState>& fault) {
    autoupdater::updater::ApplyExecutorDependencies result;
    result.fileSystem = std::make_shared<FaultingFileSystem>(autoupdater::createDefaultFileSystem(), fault);
    result.hashProvider = hashProvider;
    result.processLauncher = autoupdater::createDefaultProcessLauncher();
    return result;
}

std::filesystem::path activeJournal(const ApplyFixture& fixture) {
    return fixture.installDir / ".autoupdater" / "journal" / "active.json";
}

} // namespace

void testApplyExecutorHandlesInjectedFilesystemFaultMatrix() {
    struct FaultCase {
        const char* name;
        TransactionFault fault;
        enum class Root { Install, Staging } root;
        bool removeOperation;
    };

    const std::vector<FaultCase> cases = {
        {"read", TransactionFault::Read, FaultCase::Root::Staging, false},
        {"short-write", TransactionFault::ShortWrite, FaultCase::Root::Install, false},
        {"disk-full", TransactionFault::DiskFull, FaultCase::Root::Install, false},
        {"permission", TransactionFault::Permission, FaultCase::Root::Install, false},
        {"flush", TransactionFault::Flush, FaultCase::Root::Install, false},
        {"rename", TransactionFault::Rename, FaultCase::Root::Install, false},
        {"remove-before", TransactionFault::RemoveBefore, FaultCase::Root::Install, true},
        {"remove-after", TransactionFault::RemoveAfter, FaultCase::Root::Install, true},
    };

    for (const auto& testCase : cases) {
        ApplyFixture fixture(testCase.name, testCase.removeOperation);
        auto fault = std::make_shared<FaultState>();
        fault->fault = testCase.fault;
        fault->root = testCase.root == FaultCase::Root::Install ? fixture.installDir : fixture.stagingDir;
        fault->relativePath = "bin/app.txt";

        const auto result = autoupdater::updater::executeApplyPlanWithDependencies(
            fixture.plan, dependencies(fixture.hashProvider, fault));
        if (result || result.error().code != autoupdater::ErrorCode::FileSystemError ||
            result.error().phase != autoupdater::ErrorPhase::Apply) {
            throw std::runtime_error(std::string("Unexpected result for TEST-002 fault case ") + testCase.name +
                                     ": code=" + autoupdater::toString(result.error().code) +
                                     ", phase=" + autoupdater::toString(result.error().phase) +
                                     ", message=" + result.error().message);
        }
        LAU_REQUIRE(fault->consumed);
        LAU_REQUIRE(readFile(fixture.installDir / "bin/app.txt") == "old-transaction-content");
        LAU_REQUIRE(!std::filesystem::exists(activeJournal(fixture)));
    }
}

void testApplyExecutorRecoversAfterInjectedRollbackFilesystemFailure() {
    ApplyFixture fixture("rollback-filesystem-recovery");
    auto fault = std::make_shared<FaultState>();
    fault->fault = TransactionFault::Rename;
    fault->root = fixture.installDir;
    fault->relativePath = "bin/app.txt";
    // The first install-root commit publishes the update. The second commit is
    // the compensating replacement from the durable backup.
    fault->occurrence = 2;

    bool applyFailureInjected = false;
    autoupdater::updater::ApplyExecutionHooks hooks;
    hooks.checkpoint = [&](std::string_view boundary, std::size_t operationIndex) {
        if (!applyFailureInjected && boundary == "replace.after" && operationIndex == 0) {
            applyFailureInjected = true;
            return autoupdater::updater::ApplyFaultAction::Fail;
        }
        return autoupdater::updater::ApplyFaultAction::Continue;
    };

    auto faultingDependencies = dependencies(fixture.hashProvider, fault);
    const auto failed =
        autoupdater::updater::executeApplyPlanWithDependencies(fixture.plan, faultingDependencies, hooks);
    if (failed || !applyFailureInjected) {
        throw std::runtime_error(std::string("Rollback fault setup did not reach replace.after: code=") +
                                 autoupdater::toString(failed.error().code) + ", phase=" +
                                 autoupdater::toString(failed.error().phase) + ", message=" + failed.error().message);
    }
    LAU_REQUIRE(fault->consumed);
    LAU_REQUIRE(readFile(fixture.installDir / "bin/app.txt") == "new-transaction-content");
    LAU_REQUIRE(std::filesystem::is_regular_file(activeJournal(fixture)));

    const auto active = autoupdater::updater::parseActiveTransaction(readFile(activeJournal(fixture)));
    LAU_REQUIRE(active);
    const auto summaryPath = fixture.installDir / ".autoupdater" / "journal" / (active.value().transactionId + ".json");
    const auto failedSummary = autoupdater::updater::parseApplyJournalSummary(readFile(summaryPath));
    LAU_REQUIRE(failedSummary);
    LAU_REQUIRE(failedSummary.value().fileState == autoupdater::updater::JournalFileState::RecoveryFailed);
    LAU_REQUIRE(!failedSummary.value().applyError.empty());
    LAU_REQUIRE(!failedSummary.value().rollbackError.empty());

    // The one-shot filesystem fault is now consumed. A later updater process
    // must recover from the durable journal before attempting a new update.
    const auto recovered =
        autoupdater::updater::executeApplyPlanWithDependencies(fixture.plan, std::move(faultingDependencies));
    LAU_REQUIRE(!recovered);
    LAU_REQUIRE(recovered.error().phase == autoupdater::ErrorPhase::Recovery);
    LAU_REQUIRE(readFile(fixture.installDir / "bin/app.txt") == "old-transaction-content");
    LAU_REQUIRE(!std::filesystem::exists(activeJournal(fixture)));

    const auto recoveredSummary = autoupdater::updater::parseApplyJournalSummary(readFile(summaryPath));
    LAU_REQUIRE(recoveredSummary);
    LAU_REQUIRE(recoveredSummary.value().fileState == autoupdater::updater::JournalFileState::RolledBack);
    LAU_REQUIRE(recoveredSummary.value().rollbackError.empty());
}
