#include "TestCommon.h"

#include "default/JsonStateStoreInternal.h"
#include "libAutoUpdater/interfaces/IFileSystem.h"
#include "libAutoUpdater/interfaces/IStateStore.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#endif

namespace {

constexpr int kStateStoreCrashExitCode = 86;
constexpr auto kChildTimeout = std::chrono::seconds(30);
constexpr std::size_t kThreadWrites = 12;
constexpr std::size_t kProcessWrites = 8;

std::uint64_t processId() {
#ifdef _WIN32
    return static_cast<std::uint64_t>(GetCurrentProcessId());
#else
    return static_cast<std::uint64_t>(getpid());
#endif
}

class TemporaryDirectory final {
  public:
    explicit TemporaryDirectory(const std::string& name) {
        static std::atomic<std::uint64_t> sequence{0};
        path_ = std::filesystem::temp_directory_path() / "libAutoUpdater-state-tests" /
                (std::to_string(processId()) + "-" + std::to_string(sequence.fetch_add(1)) + "-" + name);
        std::error_code error;
        std::filesystem::remove_all(path_, error);
        error.clear();
        std::filesystem::create_directories(path_, error);
        if (error) {
            throw std::runtime_error("Failed to create state-store test directory: " + error.message());
        }
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
    if (error) {
        throw std::runtime_error("Failed to create state-store test parent: " + error.message());
    }
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("Failed to open state-store test file");
    }
    output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    if (!output) {
        throw std::runtime_error("Failed to write state-store test file");
    }
}

std::string readFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("Failed to read state-store test file");
    }
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

autoupdater::Version version(const char* text) {
    auto parsed = autoupdater::Version::parse(text);
    if (!parsed) {
        throw std::runtime_error("Invalid test version");
    }
    return parsed.value();
}

autoupdater::PendingUpdate pendingUpdate(const std::filesystem::path& root, const char* versionText,
                                         char digestCharacter) {
    autoupdater::PendingUpdate pending;
    pending.version = version(versionText);
    pending.releaseId = std::string("release-") + versionText;
    pending.backupDir = root / "backup";
    pending.applyPlanPath = root / "apply-plan.json";
    pending.applyPlanDigest = std::string(64, digestCharacter);
    return pending;
}

autoupdater::DownloadResumeState resumeState(const std::string& key, std::uint64_t offset, char digestCharacter) {
    autoupdater::DownloadResumeState state;
    state.key = key;
    state.offset = offset;
    state.etag = "\"etag-" + std::to_string(offset) + "\"";
    state.lastModified = "Mon, 01 Jun 2026 10:00:00 GMT";
    state.sha256 = std::string(64, digestCharacter);
    return state;
}

enum class PersistenceFault {
    ShortWrite,
    DiskFull,
    PermissionDenied,
    Flush,
    Commit,
    CommitAfterPublish,
};

struct FaultState {
    PersistenceFault fault = PersistenceFault::DiskFull;
    std::string target;
    std::atomic_bool consumed{false};

    bool consume(PersistenceFault candidate, const std::string& candidateTarget) noexcept {
        if (candidate != fault || candidateTarget != target) {
            return false;
        }
        bool expected = false;
        return consumed.compare_exchange_strong(expected, true);
    }
};

autoupdater::Error injectedIoError(const char* message) {
    return {autoupdater::ErrorCode::FileSystemError, message};
}

class FaultingRootedFile final : public autoupdater::IRootedFile {
  public:
    FaultingRootedFile(autoupdater::IRootedFile& inner, std::shared_ptr<FaultState> fault, std::string target)
        : inner_(inner), fault_(std::move(fault)), target_(std::move(target)) {}

    autoupdater::Result<std::size_t> read(void* buffer, std::size_t size) noexcept override {
        return inner_.read(buffer, size);
    }

    autoupdater::Result<void> write(const void* data, std::size_t size) noexcept override {
        if (fault_->consume(PersistenceFault::DiskFull, target_)) {
            return autoupdater::Result<void>::fail(injectedIoError("Injected disk-full write failure"));
        }
        if (fault_->consume(PersistenceFault::ShortWrite, target_)) {
            const auto partialSize = size / 2;
            if (partialSize != 0) {
                auto partial = inner_.write(data, partialSize);
                if (!partial) {
                    return partial;
                }
            }
            // Simulate a backend that incorrectly reports success after a
            // short write. JsonStateStore must detect the size mismatch before
            // publishing the temporary file.
            return autoupdater::Result<void>::ok();
        }
        return inner_.write(data, size);
    }

    autoupdater::Result<void> seek(std::uint64_t offset) noexcept override {
        return inner_.seek(offset);
    }

    autoupdater::Result<void> truncate(std::uint64_t size) noexcept override {
        return inner_.truncate(size);
    }

    autoupdater::Result<void> flush() noexcept override {
        if (fault_->consume(PersistenceFault::Flush, target_)) {
            return autoupdater::Result<void>::fail(injectedIoError("Injected state flush failure"));
        }
        return inner_.flush();
    }

    autoupdater::Result<autoupdater::RootedFileMetadata> metadata() noexcept override {
        return inner_.metadata();
    }

    autoupdater::Result<void> setPermissions(std::filesystem::perms permissions) noexcept override {
        return inner_.setPermissions(permissions);
    }

    autoupdater::Result<void> close() noexcept override {
        return inner_.close();
    }

  private:
    autoupdater::IRootedFile& inner_;
    std::shared_ptr<FaultState> fault_;
    std::string target_;
};

class FaultingTemporaryFile final : public autoupdater::IRootedTemporaryFile {
  public:
    FaultingTemporaryFile(std::unique_ptr<autoupdater::IRootedTemporaryFile> inner, std::shared_ptr<FaultState> fault,
                          std::string target)
        : inner_(std::move(inner)), file_(inner_->file(), fault, target), fault_(std::move(fault)),
          target_(std::move(target)) {}

    autoupdater::IRootedFile& file() noexcept override {
        return file_;
    }

    autoupdater::Result<void> commit(const autoupdater::RootedEntryExpectation& expectation) noexcept override {
        if (fault_->consume(PersistenceFault::Commit, target_)) {
            return autoupdater::Result<void>::fail(injectedIoError("Injected state commit failure"));
        }
        auto committed = inner_->commit(expectation);
        if (committed && fault_->consume(PersistenceFault::CommitAfterPublish, target_)) {
            injectedAcknowledgementFailure_ = true;
            return autoupdater::Result<void>::fail(
                injectedIoError("Injected post-publish state commit acknowledgement failure"));
        }
        return committed;
    }

    autoupdater::RootedPublishStatus publishStatus() const noexcept override {
        auto status = inner_->publishStatus();
        if (injectedAcknowledgementFailure_) {
            status.failureCanBeReconciled = true;
        }
        return status;
    }

    autoupdater::Result<void> discard() noexcept override {
        return inner_->discard();
    }

  private:
    std::unique_ptr<autoupdater::IRootedTemporaryFile> inner_;
    FaultingRootedFile file_;
    std::shared_ptr<FaultState> fault_;
    std::string target_;
    bool injectedAcknowledgementFailure_ = false;
};

class FaultingRootedDirectory final : public autoupdater::IRootedDirectory {
  public:
    FaultingRootedDirectory(std::unique_ptr<autoupdater::IRootedDirectory> inner, std::shared_ptr<FaultState> fault)
        : inner_(std::move(inner)), fault_(std::move(fault)) {}

    autoupdater::Result<autoupdater::RootedOpenResult>
    openRegularFile(const std::string& relativePath, autoupdater::RootedFileOpenMode mode,
                    autoupdater::RootedDirectoryCreationMode directoryMode) noexcept override {
        return inner_->openRegularFile(relativePath, mode, directoryMode);
    }

    autoupdater::Result<std::unique_ptr<autoupdater::IRootedTemporaryFile>>
    createAtomicReplacement(const std::string& relativePath,
                            autoupdater::RootedDirectoryCreationMode directoryMode) noexcept override {
        if (fault_->consume(PersistenceFault::PermissionDenied, relativePath)) {
            return autoupdater::Result<std::unique_ptr<autoupdater::IRootedTemporaryFile>>::fail(
                injectedIoError("Injected state permission failure"));
        }
        auto temporary = inner_->createAtomicReplacement(relativePath, directoryMode);
        if (!temporary) {
            return temporary;
        }
        std::unique_ptr<autoupdater::IRootedTemporaryFile> wrapped =
            std::make_unique<FaultingTemporaryFile>(std::move(temporary.value()), fault_, relativePath);
        return autoupdater::Result<std::unique_ptr<autoupdater::IRootedTemporaryFile>>::ok(std::move(wrapped));
    }

    autoupdater::Result<void>
    replaceWithOpenedFile(autoupdater::IRootedFile& source, const std::string& relativePath,
                          const autoupdater::RootedEntryExpectation& expectation) noexcept override {
        return inner_->replaceWithOpenedFile(source, relativePath, expectation);
    }

    autoupdater::Result<void>
    removeRegularFile(const std::string& relativePath,
                      const autoupdater::RootedEntryExpectation& expectation) noexcept override {
        return inner_->removeRegularFile(relativePath, expectation);
    }

    autoupdater::Result<std::unique_ptr<autoupdater::IRootedLock>>
    acquireExclusiveLock(const std::string& relativePath) noexcept override {
        return inner_->acquireExclusiveLock(relativePath);
    }

  private:
    std::unique_ptr<autoupdater::IRootedDirectory> inner_;
    std::shared_ptr<FaultState> fault_;
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
            std::make_unique<FaultingRootedDirectory>(std::move(opened.value()), fault_);
        return autoupdater::Result<std::unique_ptr<autoupdater::IRootedDirectory>>::ok(std::move(wrapped));
    }

  private:
    std::shared_ptr<autoupdater::IFileSystem> inner_;
    std::shared_ptr<FaultState> fault_;
};

std::filesystem::path currentExecutablePath() {
#ifdef _WIN32
    std::vector<wchar_t> buffer(512);
    for (;;) {
        const auto length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            throw std::runtime_error("GetModuleFileNameW failed for state-store helper");
        }
        if (length + 1 < buffer.size()) {
            return std::filesystem::path(std::wstring(buffer.data(), length));
        }
        buffer.resize(buffer.size() * 2);
    }
#elif defined(__APPLE__)
    std::uint32_t size = 0;
    (void)_NSGetExecutablePath(nullptr, &size);
    std::vector<char> buffer(size + 1, '\0');
    if (_NSGetExecutablePath(buffer.data(), &size) != 0) {
        throw std::runtime_error("_NSGetExecutablePath failed for state-store helper");
    }
    return std::filesystem::u8path(buffer.data());
#else
    std::vector<char> buffer(512);
    for (;;) {
        const auto length = readlink("/proc/self/exe", buffer.data(), buffer.size());
        if (length < 0) {
            throw std::runtime_error("readlink(/proc/self/exe) failed for state-store helper");
        }
        if (static_cast<std::size_t>(length) < buffer.size()) {
            return std::filesystem::u8path(std::string(buffer.data(), static_cast<std::size_t>(length)));
        }
        buffer.resize(buffer.size() * 2);
    }
#endif
}

std::string pathArgument(const std::filesystem::path& path) {
    return path.u8string();
}

#ifdef _WIN32
std::wstring widenUtf8(const std::string& value) {
    if (value.empty()) {
        return {};
    }
    const auto count =
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (count <= 0) {
        throw std::runtime_error("MultiByteToWideChar failed for state-store helper");
    }
    std::wstring result(static_cast<std::size_t>(count), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(),
                            count) != count) {
        throw std::runtime_error("MultiByteToWideChar failed for state-store helper");
    }
    return result;
}

std::wstring quoteWindowsArgument(const std::wstring& value) {
    std::wstring result = L"\"";
    std::size_t backslashes = 0;
    for (const auto character : value) {
        if (character == L'\\') {
            ++backslashes;
            continue;
        }
        if (character == L'\"') {
            result.append(backslashes * 2 + 1, L'\\');
            result.push_back(character);
            backslashes = 0;
            continue;
        }
        result.append(backslashes, L'\\');
        backslashes = 0;
        result.push_back(character);
    }
    result.append(backslashes * 2, L'\\');
    result.push_back(L'\"');
    return result;
}

struct ChildProcess {
    HANDLE process = nullptr;
};

ChildProcess startChild(const std::filesystem::path& executable, const std::vector<std::string>& arguments) {
    std::wstring command = quoteWindowsArgument(executable.wstring());
    for (const auto& argument : arguments) {
        command.push_back(L' ');
        command += quoteWindowsArgument(widenUtf8(argument));
    }
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup,
                        &process)) {
        throw std::runtime_error("CreateProcessW failed for state-store helper");
    }
    CloseHandle(process.hThread);
    return {process.hProcess};
}

int waitChild(ChildProcess child) {
    const auto wait = WaitForSingleObject(child.process, static_cast<DWORD>(kChildTimeout.count() * 1000));
    if (wait == WAIT_TIMEOUT) {
        (void)TerminateProcess(child.process, 124);
        (void)WaitForSingleObject(child.process, 5000);
        CloseHandle(child.process);
        throw std::runtime_error("State-store helper timed out");
    }
    if (wait != WAIT_OBJECT_0) {
        CloseHandle(child.process);
        throw std::runtime_error("WaitForSingleObject failed for state-store helper");
    }
    DWORD exitCode = 0;
    if (!GetExitCodeProcess(child.process, &exitCode)) {
        CloseHandle(child.process);
        throw std::runtime_error("GetExitCodeProcess failed for state-store helper");
    }
    CloseHandle(child.process);
    return static_cast<int>(exitCode);
}
#else
struct ChildProcess {
    pid_t process = -1;
};

ChildProcess startChild(const std::filesystem::path& executable, const std::vector<std::string>& arguments) {
    std::vector<std::string> storage;
    storage.reserve(arguments.size() + 1);
    storage.push_back(executable.string());
    storage.insert(storage.end(), arguments.begin(), arguments.end());
    std::vector<char*> argv;
    argv.reserve(storage.size() + 1);
    for (auto& item : storage) {
        argv.push_back(item.data());
    }
    argv.push_back(nullptr);

    const auto child = fork();
    if (child < 0) {
        throw std::runtime_error("fork failed for state-store helper");
    }
    if (child == 0) {
        execv(storage.front().c_str(), argv.data());
        std::_Exit(127);
    }
    return {child};
}

int waitChild(ChildProcess child) {
    const auto deadline = std::chrono::steady_clock::now() + kChildTimeout;
    for (;;) {
        int status = 0;
        const auto waited = waitpid(child.process, &status, WNOHANG);
        if (waited == child.process) {
            if (WIFEXITED(status)) {
                return WEXITSTATUS(status);
            }
            if (WIFSIGNALED(status)) {
                return 128 + WTERMSIG(status);
            }
            throw std::runtime_error("State-store helper ended with an unknown wait status");
        }
        if (waited < 0 && errno != EINTR) {
            throw std::runtime_error("waitpid failed for state-store helper");
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            (void)kill(child.process, SIGKILL);
            while (waitpid(child.process, &status, 0) < 0 && errno == EINTR) {
            }
            throw std::runtime_error("State-store helper timed out");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}
#endif

void requireStateError(const autoupdater::Result<std::optional<autoupdater::Version>>& result) {
    LAU_REQUIRE(!result);
    LAU_REQUIRE(result.error().code == autoupdater::ErrorCode::StateStoreError);
}

} // namespace

void testStateStoreDownloadResume() {
    TemporaryDirectory temporary("round-trip");
    const auto statePath = temporary.path() / "state.json";
    auto store = autoupdater::createJsonStateStore(statePath);
    auto state = resumeState("https://example.com/file.bin", 42, 'a');

    LAU_REQUIRE(store->saveDownloadResume(state));
    auto loaded = store->loadDownloadResume(state.key);
    LAU_REQUIRE(loaded);
    LAU_REQUIRE(loaded.value().has_value());
    LAU_REQUIRE(loaded.value()->offset == 42);
    LAU_REQUIRE(loaded.value()->etag == "\"etag-42\"");
    LAU_REQUIRE(loaded.value()->sha256 == std::string(64, 'a'));

    LAU_REQUIRE(store->clearDownloadResume(state.key));
    auto afterClear = store->loadDownloadResume(state.key);
    LAU_REQUIRE(afterClear);
    LAU_REQUIRE(!afterClear.value().has_value());

    auto pending = pendingUpdate(temporary.path(), "2.0.0", 'b');
    LAU_REQUIRE(store->savePendingUpdate(pending));
    auto loadedPending = store->loadPendingUpdate();
    LAU_REQUIRE(loadedPending);
    LAU_REQUIRE(loadedPending.value().has_value());
    LAU_REQUIRE(loadedPending.value()->applyPlanDigest == pending.applyPlanDigest);
    LAU_REQUIRE(loadedPending.value()->backupDir == pending.backupDir);

    const auto limitedPath = temporary.path() / "limited-state.json";
    writeFile(limitedPath, "12345");
    autoupdater::ResourceLimits limits;
    limits.maxStateBytes = 4;
    auto limitedStore = autoupdater::createJsonStateStore(limitedPath, limits);
    auto oversizedLoad = limitedStore->loadLastAcceptedVersion();
    LAU_REQUIRE(!oversizedLoad);
    LAU_REQUIRE(oversizedLoad.error().code == autoupdater::ErrorCode::ResourceLimitExceeded);

    std::error_code error;
    std::filesystem::remove(limitedPath, error);
    state.offset = limits.maxArtifactBytes + 1;
    auto oversizedResume = limitedStore->saveDownloadResume(state);
    LAU_REQUIRE(!oversizedResume);
    LAU_REQUIRE(oversizedResume.error().code == autoupdater::ErrorCode::ResourceLimitExceeded);
}

void testStateStoreDistinguishesMissingAndCorruptState() {
    TemporaryDirectory temporary("strict-load");

    const auto missingPath = temporary.path() / "missing" / "state.json";
    auto missingStore = autoupdater::createJsonStateStore(missingPath);
    auto missing = missingStore->loadLastAcceptedVersion();
    LAU_REQUIRE(missing);
    LAU_REQUIRE(!missing.value().has_value());

    const std::vector<std::string> malformedStates = {
        "{",
        "[]",
        R"({"schemaVersion":2})",
        R"({"lastAcceptedVersion":"1.0.0"})",
        R"({"downloadResume":[]})",
        R"({"unknownField":true})",
    };
    for (std::size_t index = 0; index < malformedStates.size(); ++index) {
        const auto path = temporary.path() / ("malformed-" + std::to_string(index)) / "state.json";
        writeFile(path, malformedStates[index]);
        requireStateError(autoupdater::createJsonStateStore(path)->loadLastAcceptedVersion());
    }

    const auto corruptPath = temporary.path() / "corrupt-with-lkg" / "state.json";
    auto corruptStore = autoupdater::createJsonStateStore(corruptPath);
    LAU_REQUIRE(corruptStore->saveLastAcceptedVersion(version("0.9.0"), "release-0"));
    LAU_REQUIRE(corruptStore->saveLastAcceptedVersion(version("1.0.0"), "release-1"));
    LAU_REQUIRE(std::filesystem::is_regular_file(corruptPath.string() + ".lkg"));
    writeFile(corruptPath, "{not-json");
    requireStateError(autoupdater::createJsonStateStore(corruptPath)->loadLastAcceptedVersion());

    const auto absentPrimaryPath = temporary.path() / "absent-primary" / "state.json";
    auto absentPrimaryStore = autoupdater::createJsonStateStore(absentPrimaryPath);
    LAU_REQUIRE(absentPrimaryStore->saveLastAcceptedVersion(version("0.9.0"), "release-0"));
    LAU_REQUIRE(absentPrimaryStore->saveLastAcceptedVersion(version("1.0.0"), "release-1"));
    std::error_code error;
    LAU_REQUIRE(std::filesystem::remove(absentPrimaryPath, error));
    LAU_REQUIRE(!error);
    requireStateError(autoupdater::createJsonStateStore(absentPrimaryPath)->loadLastAcceptedVersion());

    const auto strictPendingPath = temporary.path() / "strict-pending" / "state.json";
    const auto strictBackupDir = (temporary.path() / "strict-backup").generic_u8string();
    const auto strictApplyPlan = (temporary.path() / "strict-apply-plan.json").generic_u8string();
    const std::string strictPendingWithoutDigest = "{\"schemaVersion\":1,\"pendingUpdate\":{"
                                                   "\"version\":\"2.0.0\",\"releaseId\":\"strict-release\","
                                                   "\"backupDir\":\"" +
                                                   strictBackupDir + "\",\"applyPlanPath\":\"" + strictApplyPlan +
                                                   "\"}}";
    writeFile(strictPendingPath, strictPendingWithoutDigest);
    auto rejectedStrictPending = autoupdater::createJsonStateStore(strictPendingPath)->loadPendingUpdate();
    LAU_REQUIRE(!rejectedStrictPending);
    LAU_REQUIRE(rejectedStrictPending.error().code == autoupdater::ErrorCode::StateStoreError);
    const std::string strictPendingWithEmptyDigest = "{\"schemaVersion\":1,\"pendingUpdate\":{"
                                                     "\"version\":\"2.0.0\",\"releaseId\":\"strict-release\","
                                                     "\"backupDir\":\"" +
                                                     strictBackupDir + "\",\"applyPlanPath\":\"" + strictApplyPlan +
                                                     "\",\"applyPlanDigest\":\"\"}}";
    writeFile(strictPendingPath, strictPendingWithEmptyDigest);
    rejectedStrictPending = autoupdater::createJsonStateStore(strictPendingPath)->loadPendingUpdate();
    LAU_REQUIRE(!rejectedStrictPending);
    LAU_REQUIRE(rejectedStrictPending.error().code == autoupdater::ErrorCode::StateStoreError);

    const auto legacyPath = temporary.path() / "legacy" / "state.json";
    const auto legacyBackupDir = (temporary.path() / "legacy-backup").generic_u8string();
    const auto legacyApplyPlan = (temporary.path() / "legacy-apply-plan.json").generic_u8string();
    const std::string legacySha256(64, 'd');
    const std::string legacy = "{\"lastAcceptedVersion\":\"1.2.3\",\"lastAcceptedReleaseId\":\"legacy-release\","
                               "\"pendingUpdate\":{\"version\":\"2.0.0\",\"releaseId\":\"legacy-pending\","
                               "\"backupDir\":\"" +
                               legacyBackupDir + "\",\"applyPlanPath\":\"" + legacyApplyPlan +
                               "\"},\"downloadResume\":{\"legacy-artifact\":{\"offset\":17,"
                               "\"etag\":\"legacy-etag\",\"lastModified\":\"Mon, 01 Jun 2026 10:00:00 GMT\","
                               "\"sha256\":\"" +
                               legacySha256 + "\"}}}";
    writeFile(legacyPath, legacy);
    auto legacyStore = autoupdater::createJsonStateStore(legacyPath);
    auto legacyVersion = legacyStore->loadLastAcceptedVersion();
    LAU_REQUIRE(legacyVersion);
    LAU_REQUIRE(legacyVersion.value().has_value());
    LAU_REQUIRE(legacyVersion.value()->toString() == "1.2.3");
    auto legacyPending = legacyStore->loadPendingUpdate();
    LAU_REQUIRE(legacyPending);
    LAU_REQUIRE(legacyPending.value().has_value());
    LAU_REQUIRE(legacyPending.value()->version.toString() == "2.0.0");
    LAU_REQUIRE(legacyPending.value()->applyPlanDigest.empty());
    auto legacyResume = legacyStore->loadDownloadResume("legacy-artifact");
    LAU_REQUIRE(legacyResume);
    LAU_REQUIRE(legacyResume.value().has_value());
    LAU_REQUIRE(legacyResume.value()->offset == 17);
    LAU_REQUIRE(legacyResume.value()->sha256 == legacySha256);

    LAU_REQUIRE(legacyStore->saveDownloadResume(resumeState("migrated-artifact", 23, 'e')));
    LAU_REQUIRE(readFile(legacyPath.string() + ".lkg") == legacy);
    const auto legacyWithResume = readFile(legacyPath);
    LAU_REQUIRE(legacyWithResume.find("\"schemaVersion\"") == std::string::npos);

    auto reopenedLegacyStore = autoupdater::createJsonStateStore(legacyPath);
    auto reopenedVersion = reopenedLegacyStore->loadLastAcceptedVersion();
    LAU_REQUIRE(reopenedVersion);
    LAU_REQUIRE(reopenedVersion.value().has_value());
    LAU_REQUIRE(reopenedVersion.value()->toString() == "1.2.3");
    auto reopenedPending = reopenedLegacyStore->loadPendingUpdate();
    LAU_REQUIRE(reopenedPending);
    LAU_REQUIRE(reopenedPending.value().has_value());
    LAU_REQUIRE(reopenedPending.value()->applyPlanDigest.empty());
    auto reopenedResume = reopenedLegacyStore->loadDownloadResume("migrated-artifact");
    LAU_REQUIRE(reopenedResume);
    LAU_REQUIRE(reopenedResume.value().has_value());
    LAU_REQUIRE(reopenedResume.value()->offset == 23);

    auto rejectedLegacyPendingWrite = reopenedLegacyStore->savePendingUpdate(*reopenedPending.value());
    LAU_REQUIRE(!rejectedLegacyPendingWrite);
    LAU_REQUIRE(rejectedLegacyPendingWrite.error().code == autoupdater::ErrorCode::StateStoreError);

    auto legacyCompareAndSet =
        std::dynamic_pointer_cast<autoupdater::IPendingUpdateCompareAndSet>(reopenedLegacyStore);
    LAU_REQUIRE(legacyCompareAndSet);
    const auto legacyBeforeRejectedClear = readFile(legacyPath);
    auto mismatchedLegacyPending = *reopenedPending.value();
    mismatchedLegacyPending.releaseId = "different-legacy-pending";
    auto rejectedLegacyClear = legacyCompareAndSet->clearPendingUpdateIfMatches(mismatchedLegacyPending);
    LAU_REQUIRE(!rejectedLegacyClear);
    LAU_REQUIRE(rejectedLegacyClear.error().code == autoupdater::ErrorCode::StateStoreError);
    LAU_REQUIRE(readFile(legacyPath) == legacyBeforeRejectedClear);

    LAU_REQUIRE(legacyCompareAndSet->clearPendingUpdateIfMatches(*reopenedPending.value()));
    const auto migrated = readFile(legacyPath);
    LAU_REQUIRE(migrated.find("\"schemaVersion\"") != std::string::npos);
    auto migratedStore = autoupdater::createJsonStateStore(legacyPath);
    auto migratedPending = migratedStore->loadPendingUpdate();
    LAU_REQUIRE(migratedPending);
    LAU_REQUIRE(!migratedPending.value().has_value());
    auto migratedVersion = migratedStore->loadLastAcceptedVersion();
    LAU_REQUIRE(migratedVersion);
    LAU_REQUIRE(migratedVersion.value().has_value());
    LAU_REQUIRE(migratedVersion.value()->toString() == "1.2.3");
    auto migratedResume = migratedStore->loadDownloadResume("migrated-artifact");
    LAU_REQUIRE(migratedResume);
    LAU_REQUIRE(migratedResume.value().has_value());
    LAU_REQUIRE(migratedResume.value()->offset == 23);
}

void testStateStorePreservesLastKnownGoodSnapshot() {
    TemporaryDirectory temporary("last-known-good");
    const auto statePath = temporary.path() / "state.json";
    auto store = autoupdater::createJsonStateStore(statePath);
    LAU_REQUIRE(store->saveLastAcceptedVersion(version("1.0.0"), "release-1"));
    const auto oldPrimary = readFile(statePath);

    LAU_REQUIRE(store->saveLastAcceptedVersion(version("2.0.0"), "release-2"));
    LAU_REQUIRE(readFile(statePath.string() + ".lkg") == oldPrimary);
    auto current = autoupdater::createJsonStateStore(statePath)->loadLastAcceptedVersion();
    LAU_REQUIRE(current);
    LAU_REQUIRE(current.value().has_value());
    LAU_REQUIRE(current.value()->toString() == "2.0.0");
}

void testStateStoreConcurrentInstancesDoNotLoseUpdates() {
    TemporaryDirectory temporary("thread-contention");
    const auto statePath = temporary.path() / "state.json";
    auto leftStore = autoupdater::createJsonStateStore(statePath);
    auto rightStore = autoupdater::createJsonStateStore(statePath);
    std::atomic_bool start{false};
    std::atomic_bool failed{false};

    const auto writer = [&](const std::shared_ptr<autoupdater::IStateStore>& store, const std::string& prefix,
                            char digestCharacter) {
        while (!start.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        for (std::size_t index = 0; index < kThreadWrites; ++index) {
            const auto key = "https://example.test/" + prefix + "/" + std::to_string(index);
            if (!store->saveDownloadResume(resumeState(key, index + 1, digestCharacter))) {
                failed.store(true, std::memory_order_release);
                return;
            }
        }
    };

    std::thread left(writer, leftStore, "left", 'a');
    std::thread right(writer, rightStore, "right", 'b');
    start.store(true, std::memory_order_release);
    left.join();
    right.join();
    LAU_REQUIRE(!failed.load(std::memory_order_acquire));

    auto verifier = autoupdater::createJsonStateStore(statePath);
    for (std::size_t index = 0; index < kThreadWrites; ++index) {
        for (const auto& prefix : {std::string("left"), std::string("right")}) {
            const auto key = "https://example.test/" + prefix + "/" + std::to_string(index);
            auto loaded = verifier->loadDownloadResume(key);
            LAU_REQUIRE(loaded);
            LAU_REQUIRE(loaded.value().has_value());
            LAU_REQUIRE(loaded.value()->offset == index + 1);
        }
    }
}

void testStateStoreHealthyCommitUsesCompareAndSet() {
    TemporaryDirectory temporary("healthy-cas");

    const auto pendingStatePath = temporary.path() / "pending-state.json";
    auto pendingStore = autoupdater::createJsonStateStore(pendingStatePath);
    auto pendingCompareAndSet =
        std::dynamic_pointer_cast<autoupdater::IPendingUpdateCompareAndSet>(pendingStore);
    LAU_REQUIRE(pendingCompareAndSet);
    LAU_REQUIRE(pendingStore->saveLastAcceptedVersion(version("1.0.0"), "release-1"));
    LAU_REQUIRE(pendingStore->saveLastAcceptedVersion(version("1.1.0"), "release-1.1"));

    const auto pending = pendingUpdate(temporary.path(), "2.0.0", 'a');
    LAU_REQUIRE(pendingStore->savePendingUpdate(pending));
    const auto afterFirstCreate = readFile(pendingStatePath);
    const auto lastKnownGoodAfterFirstCreate = readFile(pendingStatePath.string() + ".lkg");

    auto loadedPending = pendingStore->loadPendingUpdate();
    LAU_REQUIRE(loadedPending);
    LAU_REQUIRE(loadedPending.value().has_value());
    LAU_REQUIRE(loadedPending.value()->version.toString() == pending.version.toString());
    LAU_REQUIRE(loadedPending.value()->releaseId == pending.releaseId);
    LAU_REQUIRE(loadedPending.value()->backupDir == pending.backupDir);
    LAU_REQUIRE(loadedPending.value()->applyPlanPath == pending.applyPlanPath);
    LAU_REQUIRE(loadedPending.value()->applyPlanDigest == pending.applyPlanDigest);

    LAU_REQUIRE(pendingStore->savePendingUpdate(pending));
    LAU_REQUIRE(readFile(pendingStatePath) == afterFirstCreate);
    LAU_REQUIRE(readFile(pendingStatePath.string() + ".lkg") == lastKnownGoodAfterFirstCreate);

    const auto conflictingPending = pendingUpdate(temporary.path(), "3.0.0", 'b');
    auto rejectedConflict = pendingStore->savePendingUpdate(conflictingPending);
    LAU_REQUIRE(!rejectedConflict);
    LAU_REQUIRE(rejectedConflict.error().code == autoupdater::ErrorCode::StateStoreError);
    LAU_REQUIRE(readFile(pendingStatePath) == afterFirstCreate);
    LAU_REQUIRE(readFile(pendingStatePath.string() + ".lkg") == lastKnownGoodAfterFirstCreate);

    auto accepted = pendingStore->loadLastAcceptedVersion();
    LAU_REQUIRE(accepted);
    LAU_REQUIRE(accepted.value().has_value());
    LAU_REQUIRE(accepted.value()->toString() == "1.1.0");
    auto acceptedRelease = pendingStore->loadLastAcceptedReleaseId();
    LAU_REQUIRE(acceptedRelease);
    LAU_REQUIRE(acceptedRelease.value() == "release-1.1");
    loadedPending = pendingStore->loadPendingUpdate();
    LAU_REQUIRE(loadedPending);
    LAU_REQUIRE(loadedPending.value().has_value());
    LAU_REQUIRE(loadedPending.value()->version.toString() == pending.version.toString());
    LAU_REQUIRE(loadedPending.value()->applyPlanDigest == pending.applyPlanDigest);

    auto mismatchedPending = pending;
    mismatchedPending.applyPlanDigest = std::string(64, 'c');
    auto rejectedClear = pendingCompareAndSet->clearPendingUpdateIfMatches(mismatchedPending);
    LAU_REQUIRE(!rejectedClear);
    LAU_REQUIRE(rejectedClear.error().code == autoupdater::ErrorCode::StateStoreError);
    LAU_REQUIRE(readFile(pendingStatePath) == afterFirstCreate);
    LAU_REQUIRE(readFile(pendingStatePath.string() + ".lkg") == lastKnownGoodAfterFirstCreate);

    LAU_REQUIRE(pendingCompareAndSet->clearPendingUpdateIfMatches(pending));
    auto afterClear = pendingStore->loadPendingUpdate();
    LAU_REQUIRE(afterClear);
    LAU_REQUIRE(!afterClear.value().has_value());
    LAU_REQUIRE(readFile(pendingStatePath.string() + ".lkg") == afterFirstCreate);
    accepted = pendingStore->loadLastAcceptedVersion();
    LAU_REQUIRE(accepted);
    LAU_REQUIRE(accepted.value().has_value());
    LAU_REQUIRE(accepted.value()->toString() == "1.1.0");
    acceptedRelease = pendingStore->loadLastAcceptedReleaseId();
    LAU_REQUIRE(acceptedRelease);
    LAU_REQUIRE(acceptedRelease.value() == "release-1.1");

    const auto afterSuccessfulClear = readFile(pendingStatePath);
    const auto lastKnownGoodAfterSuccessfulClear = readFile(pendingStatePath.string() + ".lkg");
    auto rejectedMissingClear = pendingCompareAndSet->clearPendingUpdateIfMatches(pending);
    LAU_REQUIRE(!rejectedMissingClear);
    LAU_REQUIRE(rejectedMissingClear.error().code == autoupdater::ErrorCode::StateStoreError);
    LAU_REQUIRE(readFile(pendingStatePath) == afterSuccessfulClear);
    LAU_REQUIRE(readFile(pendingStatePath.string() + ".lkg") == lastKnownGoodAfterSuccessfulClear);

    const auto statePath = temporary.path() / "healthy-state.json";
    auto store = autoupdater::createJsonStateStore(statePath);
    LAU_REQUIRE(store->saveLastAcceptedVersion(version("1.0.0"), "release-1"));
    LAU_REQUIRE(store->savePendingUpdate(pending));
    const auto beforeFailedCas = readFile(statePath);

    auto mismatched = pending;
    mismatched.applyPlanDigest = std::string(64, 'b');
    auto rejected = store->commitHealthyVersion(version("2.0.0"), "release-2", mismatched);
    LAU_REQUIRE(!rejected);
    LAU_REQUIRE(rejected.error().code == autoupdater::ErrorCode::StateStoreError);
    LAU_REQUIRE(readFile(statePath) == beforeFailedCas);

    auto rejectedAbsent = store->commitHealthyVersion(version("2.0.0"), "release-2", std::nullopt);
    LAU_REQUIRE(!rejectedAbsent);
    LAU_REQUIRE(rejectedAbsent.error().code == autoupdater::ErrorCode::StateStoreError);
    LAU_REQUIRE(readFile(statePath) == beforeFailedCas);

    LAU_REQUIRE(store->commitHealthyVersion(version("2.0.0"), "release-2", pending));
    accepted = store->loadLastAcceptedVersion();
    LAU_REQUIRE(accepted);
    LAU_REQUIRE(accepted.value().has_value());
    LAU_REQUIRE(accepted.value()->toString() == "2.0.0");
    auto release = store->loadLastAcceptedReleaseId();
    LAU_REQUIRE(release);
    LAU_REQUIRE(release.value() == "release-2");
    auto afterCommit = store->loadPendingUpdate();
    LAU_REQUIRE(afterCommit);
    LAU_REQUIRE(!afterCommit.value().has_value());
    LAU_REQUIRE(readFile(statePath.string() + ".lkg") == beforeFailedCas);
}

void testStateStoreWriteFailuresPreservePrimary() {
    TemporaryDirectory temporary("write-failures");
    struct FaultCase {
        const char* name;
        PersistenceFault fault;
        bool targetBackup;
    };
    const std::vector<FaultCase> cases = {
        {"short-write", PersistenceFault::ShortWrite, false},
        {"disk-full", PersistenceFault::DiskFull, false},
        {"permission", PersistenceFault::PermissionDenied, false},
        {"flush", PersistenceFault::Flush, false},
        {"commit", PersistenceFault::Commit, false},
        {"backup-disk-full", PersistenceFault::DiskFull, true},
    };

    for (const auto& testCase : cases) {
        const auto caseRoot = temporary.path() / testCase.name;
        const auto statePath = caseRoot / "state.json";
        auto seed = autoupdater::createJsonStateStore(statePath);
        LAU_REQUIRE(seed->saveLastAcceptedVersion(version("0.9.0"), "release-0"));
        LAU_REQUIRE(seed->saveLastAcceptedVersion(version("1.0.0"), "release-1"));
        const auto oldPrimary = readFile(statePath);
        const auto oldBackup = readFile(statePath.string() + ".lkg");

        auto fault = std::make_shared<FaultState>();
        fault->fault = testCase.fault;
        fault->target = testCase.targetBackup ? "state.json.lkg" : "state.json";
        auto fileSystem = std::make_shared<FaultingFileSystem>(autoupdater::createDefaultFileSystem(), fault);
        auto failingStore =
            autoupdater::detail::createJsonStateStoreForTesting(statePath, autoupdater::ResourceLimits{}, fileSystem);
        auto failed = failingStore->saveLastAcceptedVersion(version("2.0.0"), "release-2");
        LAU_REQUIRE(!failed);
        LAU_REQUIRE(failed.error().code == autoupdater::ErrorCode::StateStoreError);
        LAU_REQUIRE(fault->consumed.load(std::memory_order_acquire));
        LAU_REQUIRE(readFile(statePath) == oldPrimary);
        LAU_REQUIRE(readFile(statePath.string() + ".lkg") == (testCase.targetBackup ? oldBackup : oldPrimary));

        auto reopened = autoupdater::createJsonStateStore(statePath)->loadLastAcceptedVersion();
        LAU_REQUIRE(reopened);
        LAU_REQUIRE(reopened.value().has_value());
        LAU_REQUIRE(reopened.value()->toString() == "1.0.0");
    }

    const auto prePublishRoot = temporary.path() / "pre-publish-same-content";
    const auto prePublishPath = prePublishRoot / "state.json";
    auto prePublishSeed = autoupdater::createJsonStateStore(prePublishPath);
    LAU_REQUIRE(prePublishSeed->saveLastAcceptedVersion(version("2.0.0"), "release-2"));
    const auto unchangedPrimary = readFile(prePublishPath);
    auto prePublishFault = std::make_shared<FaultState>();
    prePublishFault->fault = PersistenceFault::Commit;
    prePublishFault->target = "state.json";
    auto prePublishFileSystem =
        std::make_shared<FaultingFileSystem>(autoupdater::createDefaultFileSystem(), prePublishFault);
    auto prePublishStore = autoupdater::detail::createJsonStateStoreForTesting(
        prePublishPath, autoupdater::ResourceLimits{}, prePublishFileSystem);
    auto prePublishFailure = prePublishStore->saveLastAcceptedVersion(version("2.0.0"), "release-2");
    LAU_REQUIRE(!prePublishFailure);
    LAU_REQUIRE(prePublishFault->consumed.load(std::memory_order_acquire));
    LAU_REQUIRE(readFile(prePublishPath) == unchangedPrimary);

    const auto reconcileRoot = temporary.path() / "post-publish-reconciliation";
    const auto reconcilePath = reconcileRoot / "state.json";
    auto reconcileSeed = autoupdater::createJsonStateStore(reconcilePath);
    LAU_REQUIRE(reconcileSeed->saveLastAcceptedVersion(version("0.9.0"), "release-0"));
    LAU_REQUIRE(reconcileSeed->saveLastAcceptedVersion(version("1.0.0"), "release-1"));
    auto reconcileFault = std::make_shared<FaultState>();
    reconcileFault->fault = PersistenceFault::CommitAfterPublish;
    reconcileFault->target = "state.json";
    auto reconcileFileSystem =
        std::make_shared<FaultingFileSystem>(autoupdater::createDefaultFileSystem(), reconcileFault);
    auto reconcileStore = autoupdater::detail::createJsonStateStoreForTesting(
        reconcilePath, autoupdater::ResourceLimits{}, reconcileFileSystem);
    // The first commit publishes successfully but loses its acknowledgement.
    // JsonStateStore must reopen by identity and obtain a confirmed durability
    // barrier before reporting success.
    LAU_REQUIRE(reconcileStore->saveLastAcceptedVersion(version("2.0.0"), "release-2"));
    LAU_REQUIRE(reconcileFault->consumed.load(std::memory_order_acquire));
    auto reconciled = autoupdater::createJsonStateStore(reconcilePath)->loadLastAcceptedVersion();
    LAU_REQUIRE(reconciled);
    LAU_REQUIRE(reconciled.value().has_value());
    LAU_REQUIRE(reconciled.value()->toString() == "2.0.0");
}

void testStateStoreCrossProcessLockingAndCrashRecovery() {
    TemporaryDirectory temporary("process-contention");
    const auto executable = currentExecutablePath();

    const auto concurrentState = temporary.path() / "concurrent" / "state.json";
    const auto startMarker = temporary.path() / "concurrent" / "start.marker";
    auto left = startChild(executable, {"--state-store-helper-write", pathArgument(concurrentState), "left",
                                        std::to_string(kProcessWrites), pathArgument(startMarker)});
    auto right = startChild(executable, {"--state-store-helper-write", pathArgument(concurrentState), "right",
                                         std::to_string(kProcessWrites), pathArgument(startMarker)});
    writeFile(startMarker, "go");
    const auto leftExit = waitChild(left);
    const auto rightExit = waitChild(right);
    LAU_REQUIRE(leftExit == 0);
    LAU_REQUIRE(rightExit == 0);

    auto concurrentStore = autoupdater::createJsonStateStore(concurrentState);
    for (std::size_t index = 0; index < kProcessWrites; ++index) {
        for (const auto& prefix : {std::string("left"), std::string("right")}) {
            const auto key = "https://process.example/" + prefix + "/" + std::to_string(index);
            auto loaded = concurrentStore->loadDownloadResume(key);
            LAU_REQUIRE(loaded);
            LAU_REQUIRE(loaded.value().has_value());
        }
    }

    const auto crashState = temporary.path() / "crash" / "state.json";
    auto seed = autoupdater::createJsonStateStore(crashState);
    LAU_REQUIRE(seed->saveLastAcceptedVersion(version("0.9.0"), "release-0"));
    LAU_REQUIRE(seed->saveLastAcceptedVersion(version("1.0.0"), "release-1"));
    const auto oldPrimary = readFile(crashState);
    auto crash = startChild(executable, {"--state-store-helper-crash-backup", pathArgument(crashState)});
    LAU_REQUIRE(waitChild(crash) == kStateStoreCrashExitCode);
    LAU_REQUIRE(readFile(crashState) == oldPrimary);
    LAU_REQUIRE(readFile(crashState.string() + ".lkg") == oldPrimary);

    auto recoveredStore = autoupdater::createJsonStateStore(crashState);
    auto recovered = recoveredStore->loadLastAcceptedVersion();
    LAU_REQUIRE(recovered);
    LAU_REQUIRE(recovered.value().has_value());
    LAU_REQUIRE(recovered.value()->toString() == "1.0.0");
    // The crashing process must not leave a stale inter-process lock.
    LAU_REQUIRE(recoveredStore->saveLastAcceptedVersion(version("2.0.0"), "release-2"));

    const auto primaryCrashState = temporary.path() / "crash-after-primary" / "state.json";
    auto primarySeed = autoupdater::createJsonStateStore(primaryCrashState);
    LAU_REQUIRE(primarySeed->saveLastAcceptedVersion(version("0.9.0"), "release-0"));
    LAU_REQUIRE(primarySeed->saveLastAcceptedVersion(version("1.0.0"), "release-1"));
    const auto prePrimaryCrash = readFile(primaryCrashState);
    auto primaryCrash = startChild(executable, {"--state-store-helper-crash-primary", pathArgument(primaryCrashState)});
    LAU_REQUIRE(waitChild(primaryCrash) == kStateStoreCrashExitCode);
    LAU_REQUIRE(readFile(primaryCrashState.string() + ".lkg") == prePrimaryCrash);
    auto primaryRecoveredStore = autoupdater::createJsonStateStore(primaryCrashState);
    auto primaryRecovered = primaryRecoveredStore->loadLastAcceptedVersion();
    LAU_REQUIRE(primaryRecovered);
    LAU_REQUIRE(primaryRecovered.value().has_value());
    LAU_REQUIRE(primaryRecovered.value()->toString() == "2.0.0");
    LAU_REQUIRE(primaryRecoveredStore->saveLastAcceptedVersion(version("3.0.0"), "release-3"));
}

int runStateStoreHelper(int argc, char* argv[]) {
    try {
        if (argc >= 2 && std::string(argv[1]) == "--state-store-helper-write") {
            if (argc != 6) {
                return 2;
            }
            const auto statePath = std::filesystem::u8path(argv[2]);
            const std::string prefix = argv[3];
            const auto count = static_cast<std::size_t>(std::stoul(argv[4]));
            const auto marker = std::filesystem::u8path(argv[5]);
            const auto deadline = std::chrono::steady_clock::now() + kChildTimeout;
            for (;;) {
                std::error_code error;
                if (std::filesystem::exists(marker, error)) {
                    break;
                }
                if (error || std::chrono::steady_clock::now() >= deadline) {
                    return 3;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }

            auto store = autoupdater::createJsonStateStore(statePath);
            const char digestCharacter = prefix == "left" ? 'a' : 'b';
            for (std::size_t index = 0; index < count; ++index) {
                const auto key = "https://process.example/" + prefix + "/" + std::to_string(index);
                auto saved = store->saveDownloadResume(resumeState(key, index + 1, digestCharacter));
                if (!saved) {
                    writeFile(statePath.parent_path() / (prefix + ".error.txt"),
                              std::to_string(static_cast<int>(saved.error().code)) + ":" + saved.error().message);
                    return 4;
                }
            }
            return 0;
        }

        if (argc >= 2 && (std::string(argv[1]) == "--state-store-helper-crash-backup" ||
                          std::string(argv[1]) == "--state-store-helper-crash-primary")) {
            if (argc != 3) {
                return 2;
            }
            autoupdater::detail::JsonStateStoreHooks hooks;
            const auto target = std::string(argv[1]) == "--state-store-helper-crash-backup"
                                    ? autoupdater::detail::JsonStateStoreCheckpoint::BackupCommitted
                                    : autoupdater::detail::JsonStateStoreCheckpoint::PrimaryCommitted;
            hooks.checkpoint = [target](autoupdater::detail::JsonStateStoreCheckpoint checkpoint) {
                if (checkpoint == target) {
                    std::_Exit(kStateStoreCrashExitCode);
                }
            };
            auto store = autoupdater::detail::createJsonStateStoreForTesting(
                std::filesystem::u8path(argv[2]), autoupdater::ResourceLimits{}, autoupdater::createDefaultFileSystem(),
                std::move(hooks));
            return store->saveLastAcceptedVersion(version("2.0.0"), "release-2") ? 5 : 6;
        }
    } catch (...) {
        return 7;
    }
    return 2;
}
