#include "TestCommon.h"

#include "ApplyJournal.h"
#include "libAutoUpdater/ApplyPlan.h"
#include "libAutoUpdater/interfaces/IHashProvider.h"
#include "util/PathUtil.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
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

constexpr int kCrashExitCode = 86;
constexpr auto kChildTimeout = std::chrono::seconds(30);
constexpr const char* kOldContents = "old-content\n";
constexpr const char* kNewContents = "new-content\n";
constexpr const char* kOldAContents = "old-a\n";
constexpr const char* kNewAContents = "new-a\n";
constexpr const char* kOldBContents = "old-b\n";
constexpr const char* kNewBContents = "new-b\n";
constexpr const char* kThirdContents = "third-content\n";
constexpr const char* kPostRestartContents = "application-mutated-after-restart\n";

void writeFile(const std::filesystem::path& path, const std::string& contents) {
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error) {
        throw std::runtime_error("Failed to create test directory: " + error.message());
    }
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("Failed to open test file for writing");
    }
    output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    if (!output) {
        throw std::runtime_error("Failed to write test file");
    }
}

std::string readFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("Failed to open test file for reading");
    }
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

std::uint64_t processId() {
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
    explicit TemporaryDirectory(const std::string& name)
        : path_(std::filesystem::temp_directory_path() / "libAutoUpdater-forced-crash-recovery" /
                (std::to_string(processId()) + "-" + safeName(name))) {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
        if (error) {
            throw std::runtime_error("Failed to reset temporary directory: " + error.message());
        }
        error.clear();
        std::filesystem::create_directories(path_, error);
        if (error) {
            throw std::runtime_error("Failed to create temporary directory: " + error.message());
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

std::filesystem::path currentExecutablePath() {
#ifdef _WIN32
    std::vector<wchar_t> buffer(512);
    for (;;) {
        const auto length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            throw std::runtime_error("GetModuleFileNameW failed");
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
        throw std::runtime_error("_NSGetExecutablePath failed");
    }
    return std::filesystem::u8path(buffer.data());
#else
    std::vector<char> buffer(512);
    for (;;) {
        const auto length = readlink("/proc/self/exe", buffer.data(), buffer.size());
        if (length < 0) {
            throw std::runtime_error("readlink(/proc/self/exe) failed");
        }
        if (static_cast<std::size_t>(length) < buffer.size()) {
            return std::filesystem::u8path(std::string(buffer.data(), static_cast<std::size_t>(length)));
        }
        buffer.resize(buffer.size() * 2);
    }
#endif
}

std::filesystem::path crashHelperPath() {
#ifdef _WIN32
    const auto configuredSize = GetEnvironmentVariableW(L"LIBAUTOUPDATER_APPLY_CRASH_HELPER", nullptr, 0);
    if (configuredSize > 1) {
        std::vector<wchar_t> configured(configuredSize, L'\0');
        if (GetEnvironmentVariableW(L"LIBAUTOUPDATER_APPLY_CRASH_HELPER", configured.data(), configuredSize) == 0) {
            throw std::runtime_error("GetEnvironmentVariableW failed");
        }
        return std::filesystem::path(configured.data());
    }
#else
    if (const auto* configured = std::getenv("LIBAUTOUPDATER_APPLY_CRASH_HELPER")) {
        if (*configured != '\0') {
            return std::filesystem::u8path(configured);
        }
    }
#endif

#ifdef LIBAUTOUPDATER_APPLY_CRASH_HELPER_PATH
    return std::filesystem::u8path(LIBAUTOUPDATER_APPLY_CRASH_HELPER_PATH);
#else
    auto sibling = currentExecutablePath().parent_path() / "libAutoUpdater_apply_crash_helper";
#ifdef _WIN32
    sibling += ".exe";
#endif
    return sibling;
#endif
}

#ifdef _WIN32
std::wstring widenUtf8(const std::string& value) {
    if (value.empty()) {
        return {};
    }
    const auto count =
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (count <= 0) {
        throw std::runtime_error("MultiByteToWideChar failed");
    }
    std::wstring result(static_cast<std::size_t>(count), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(),
                            count) != count) {
        throw std::runtime_error("MultiByteToWideChar failed");
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

int runChild(const std::filesystem::path& executable, const std::vector<std::string>& arguments) {
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
        throw std::runtime_error("CreateProcessW failed for crash helper");
    }
    CloseHandle(process.hThread);
    const auto wait = WaitForSingleObject(process.hProcess, static_cast<DWORD>(kChildTimeout.count() * 1000));
    if (wait == WAIT_TIMEOUT) {
        (void)TerminateProcess(process.hProcess, 124);
        (void)WaitForSingleObject(process.hProcess, 5000);
        CloseHandle(process.hProcess);
        throw std::runtime_error("Crash helper timed out");
    }
    if (wait != WAIT_OBJECT_0) {
        CloseHandle(process.hProcess);
        throw std::runtime_error("WaitForSingleObject failed for crash helper");
    }
    DWORD exitCode = 0;
    if (!GetExitCodeProcess(process.hProcess, &exitCode)) {
        CloseHandle(process.hProcess);
        throw std::runtime_error("GetExitCodeProcess failed for crash helper");
    }
    CloseHandle(process.hProcess);
    return static_cast<int>(exitCode);
}
#else
int runChild(const std::filesystem::path& executable, const std::vector<std::string>& arguments) {
    std::vector<std::string> storage;
    storage.reserve(arguments.size() + 1);
    storage.push_back(executable.string());
    storage.insert(storage.end(), arguments.begin(), arguments.end());

    const auto child = fork();
    if (child < 0) {
        throw std::runtime_error("fork failed for crash helper");
    }
    if (child == 0) {
        std::vector<char*> argv;
        argv.reserve(storage.size() + 1);
        for (auto& item : storage) {
            argv.push_back(item.data());
        }
        argv.push_back(nullptr);
        execv(storage.front().c_str(), argv.data());
        std::_Exit(127);
    }

    const auto deadline = std::chrono::steady_clock::now() + kChildTimeout;
    for (;;) {
        int status = 0;
        const auto waited = waitpid(child, &status, WNOHANG);
        if (waited == child) {
            if (WIFEXITED(status)) {
                return WEXITSTATUS(status);
            }
            if (WIFSIGNALED(status)) {
                return 128 + WTERMSIG(status);
            }
            throw std::runtime_error("Crash helper ended with an unknown wait status");
        }
        if (waited < 0 && errno != EINTR) {
            throw std::runtime_error("waitpid failed for crash helper");
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            (void)kill(child, SIGKILL);
            while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
            }
            throw std::runtime_error("Crash helper timed out");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}
#endif

std::string pathArgument(const std::filesystem::path& path) {
#ifdef _WIN32
    const auto utf8Length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, path.c_str(),
                                                static_cast<int>(path.native().size()), nullptr, 0, nullptr, nullptr);
    if (utf8Length <= 0) {
        throw std::runtime_error("WideCharToMultiByte failed");
    }
    std::string result(static_cast<std::size_t>(utf8Length), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, path.c_str(), static_cast<int>(path.native().size()),
                            result.data(), utf8Length, nullptr, nullptr) != utf8Length) {
        throw std::runtime_error("WideCharToMultiByte failed");
    }
    return result;
#else
    return path.string();
#endif
}

int crashAt(const std::filesystem::path& helper, const std::filesystem::path& planPath, const std::string& checkpoint,
            bool operationScoped) {
    return runChild(helper, {"--crash", pathArgument(planPath), checkpoint, operationScoped ? "0" : "any"});
}

int crashAtOperation(const std::filesystem::path& helper, const std::filesystem::path& planPath,
                     const std::string& checkpoint, std::size_t operationIndex) {
    return runChild(helper, {"--crash", pathArgument(planPath), checkpoint, std::to_string(operationIndex)});
}

int failAt(const std::filesystem::path& helper, const std::filesystem::path& planPath, const std::string& checkpoint,
           bool operationScoped) {
    return runChild(helper, {"--fail", pathArgument(planPath), checkpoint, operationScoped ? "0" : "any"});
}

int failAtOperation(const std::filesystem::path& helper, const std::filesystem::path& planPath,
                    const std::string& checkpoint, std::size_t operationIndex) {
    return runChild(helper, {"--fail", pathArgument(planPath), checkpoint, std::to_string(operationIndex)});
}

int recover(const std::filesystem::path& helper, const std::filesystem::path& planPath) {
    return runChild(helper, {"--recover", pathArgument(planPath)});
}

int crashAtWithMarker(const std::filesystem::path& helper, const std::filesystem::path& planPath,
                      const std::string& checkpoint, bool operationScoped, const std::filesystem::path& marker) {
    return runChild(helper, {"--crash-marker", pathArgument(planPath), checkpoint, operationScoped ? "0" : "any",
                             pathArgument(marker)});
}

int recoverWithMarker(const std::filesystem::path& helper, const std::filesystem::path& planPath,
                      const std::filesystem::path& marker) {
    return runChild(helper, {"--recover-marker", pathArgument(planPath), pathArgument(marker)});
}

int crashAtWithFailingMarker(const std::filesystem::path& helper, const std::filesystem::path& planPath,
                             const std::string& checkpoint, const std::filesystem::path& marker) {
    return runChild(helper, {"--crash-marker-fail", pathArgument(planPath), checkpoint, "any", pathArgument(marker)});
}

int failAtWithMarker(const std::filesystem::path& helper, const std::filesystem::path& planPath,
                     const std::string& checkpoint, bool operationScoped, const std::filesystem::path& marker) {
    return runChild(helper, {"--fail-marker", pathArgument(planPath), checkpoint, operationScoped ? "0" : "any",
                             pathArgument(marker)});
}

int failAtWithFailingMarker(const std::filesystem::path& helper, const std::filesystem::path& planPath,
                            const std::string& checkpoint, const std::filesystem::path& marker) {
    return runChild(helper, {"--fail-marker-fail", pathArgument(planPath), checkpoint, "any", pathArgument(marker)});
}

int recoverWithFailingMarker(const std::filesystem::path& helper, const std::filesystem::path& planPath,
                             const std::filesystem::path& marker) {
    return runChild(helper, {"--recover-marker-fail", pathArgument(planPath), pathArgument(marker)});
}

int recoverForOperatorIntervention(const std::filesystem::path& helper, const std::filesystem::path& planPath,
                                   const std::filesystem::path& marker) {
    return runChild(helper, {"--recover-intervention", pathArgument(planPath), pathArgument(marker)});
}

int expectBlocked(const std::filesystem::path& helper, const std::filesystem::path& planPath) {
    return runChild(helper, {"--expect-blocked", pathArgument(planPath)});
}

int expectLockBlocked(const std::filesystem::path& helper, const std::filesystem::path& planPath) {
    return runChild(helper, {"--expect-lock-blocked", pathArgument(planPath)});
}

std::filesystem::path activeJournal(const std::filesystem::path& installDir) {
    return installDir / ".autoupdater" / "journal" / "active.json";
}

void requireActiveJournalCleared(const autoupdater::ApplyPlan& plan, const std::string& checkpoint) {
    if (std::filesystem::exists(activeJournal(plan.installDir))) {
        throw std::runtime_error("Active journal remained after recovery from checkpoint: " + checkpoint);
    }
}

void requireActiveJournalState(const autoupdater::ApplyPlan& plan, bool expected, const std::string& checkpoint) {
    if (std::filesystem::exists(activeJournal(plan.installDir)) != expected) {
        throw std::runtime_error(std::string(expected ? "Active journal was missing after checkpoint: "
                                                      : "Active journal remained after checkpoint: ") +
                                 checkpoint);
    }
}

autoupdater::updater::ActiveTransaction readActiveTransaction(const autoupdater::ApplyPlan& plan) {
    const auto parsed = autoupdater::updater::parseActiveTransaction(readFile(activeJournal(plan.installDir)));
    LAU_REQUIRE(parsed);
    return parsed.value();
}

autoupdater::updater::ActiveTransaction readTerminalTransaction(const autoupdater::ApplyPlan& plan) {
    const auto path = plan.installDir / ".autoupdater" / "journal" / "terminal.json";
    const auto parsed = autoupdater::updater::parseActiveTransaction(readFile(path));
    LAU_REQUIRE(parsed);
    return parsed.value();
}

autoupdater::updater::ApplyJournalSummary readSummary(const autoupdater::ApplyPlan& plan,
                                                      const std::string& transactionId) {
    const auto path = plan.installDir / ".autoupdater" / "journal" / (transactionId + ".json");
    const auto parsed = autoupdater::updater::parseApplyJournalSummary(readFile(path));
    LAU_REQUIRE(parsed);
    return parsed.value();
}

autoupdater::updater::ApplyJournalOperation readOperation(const autoupdater::ApplyPlan& plan,
                                                          const std::string& transactionId, std::size_t index) {
    const auto path =
        plan.installDir /
        std::filesystem::u8path(autoupdater::updater::ApplyJournalStore::operationPath(transactionId, index));
    const auto parsed = autoupdater::updater::parseApplyJournalOperation(readFile(path));
    LAU_REQUIRE(parsed);
    return parsed.value();
}

std::filesystem::path summaryJournal(const autoupdater::ApplyPlan& plan, const std::string& transactionId) {
    return plan.installDir /
           std::filesystem::u8path(autoupdater::updater::ApplyJournalStore::summaryPath(transactionId));
}

std::filesystem::path planJournal(const autoupdater::ApplyPlan& plan, const std::string& transactionId) {
    return plan.installDir / std::filesystem::u8path(autoupdater::updater::ApplyJournalStore::planPath(transactionId));
}

std::filesystem::path terminalJournal(const autoupdater::ApplyPlan& plan) {
    return plan.installDir / std::filesystem::u8path(autoupdater::updater::ApplyJournalStore::terminalPath());
}

std::size_t launchCount(const std::filesystem::path& marker) {
    if (!std::filesystem::exists(marker)) {
        return 0;
    }
    const auto contents = readFile(marker);
    constexpr std::string_view launchRecord = "launch\n";
    if (contents.size() % launchRecord.size() != 0) {
        throw std::runtime_error("Restart launch marker contains a partial record");
    }
    for (std::size_t offset = 0; offset < contents.size(); offset += launchRecord.size()) {
        if (std::string_view(contents).substr(offset, launchRecord.size()) != launchRecord) {
            throw std::runtime_error("Restart launch marker contains an invalid record");
        }
    }
    return contents.size() / launchRecord.size();
}

std::vector<std::string> planSnapshotNames(const autoupdater::ApplyPlan& plan) {
    const auto journal = plan.installDir / ".autoupdater" / "journal";
    std::vector<std::string> result;
    for (const auto& entry : std::filesystem::directory_iterator(journal)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        const auto name = entry.path().filename().string();
        constexpr std::string_view suffix = ".plan.json";
        if (name.size() >= suffix.size() &&
            std::string_view(name).substr(name.size() - suffix.size(), suffix.size()) == suffix) {
            result.push_back(name);
        }
    }
    std::sort(result.begin(), result.end());
    return result;
}

std::string sha256(const std::string& contents) {
    const auto provider = autoupdater::createDefaultHashProvider();
    const auto result = provider->sha256Bytes(contents);
    LAU_REQUIRE(result);
    return result.value();
}

autoupdater::ApplyPlan replacePlan(const TemporaryDirectory& temporary, bool originalExists) {
    const auto install = temporary.path() / "install";
    const auto staging = temporary.path() / "staging";
    const auto backup = temporary.path() / "backup";
    const auto target = std::filesystem::path("bin/app.txt");
    if (originalExists) {
        writeFile(install / target, kOldContents);
    }
    writeFile(staging / target, kNewContents);

    autoupdater::ApplyPlan plan;
    plan.appId = "crash-recovery-test";
    plan.fromVersion = "1.0.0";
    plan.toVersion = "2.0.0";
    plan.releaseId = "forced-termination";
    plan.installDir = install;
    plan.stagingDir = staging;
    plan.backupDir = backup;
    plan.operations.push_back({autoupdater::ApplyOperationType::Replace, "bin/app.txt", "bin/app.txt",
                               sha256(kNewContents), std::string(kNewContents).size()});
    return plan;
}

autoupdater::ApplyPlan multiReplacePlan(const TemporaryDirectory& temporary) {
    const auto install = temporary.path() / "install";
    const auto staging = temporary.path() / "staging";
    writeFile(install / "bin/a.txt", kOldAContents);
    writeFile(install / "bin/b.txt", kOldBContents);
    writeFile(staging / "bin/a.txt", kNewAContents);
    writeFile(staging / "bin/b.txt", kNewBContents);

    autoupdater::ApplyPlan plan;
    plan.appId = "crash-recovery-test";
    plan.fromVersion = "1.0.0";
    plan.toVersion = "2.0.0";
    plan.releaseId = "forced-multi-operation";
    plan.installDir = install;
    plan.stagingDir = staging;
    plan.backupDir = temporary.path() / "backup";
    plan.operations.push_back({autoupdater::ApplyOperationType::Replace, "bin/a.txt", "bin/a.txt",
                               sha256(kNewAContents), std::string(kNewAContents).size()});
    plan.operations.push_back({autoupdater::ApplyOperationType::Replace, "bin/b.txt", "bin/b.txt",
                               sha256(kNewBContents), std::string(kNewBContents).size()});
    return plan;
}

autoupdater::ApplyPlan multiReplaceWithMissingOriginalPlan(const TemporaryDirectory& temporary) {
    const auto install = temporary.path() / "install";
    const auto staging = temporary.path() / "staging";
    writeFile(install / "bin/b.txt", kOldBContents);
    writeFile(staging / "bin/a.txt", kNewAContents);
    writeFile(staging / "bin/b.txt", kNewBContents);

    autoupdater::ApplyPlan plan;
    plan.appId = "crash-recovery-test";
    plan.fromVersion = "1.0.0";
    plan.toVersion = "2.0.0";
    plan.releaseId = "forced-multi-operation-missing-original";
    plan.installDir = install;
    plan.stagingDir = staging;
    plan.backupDir = temporary.path() / "backup";
    plan.operations.push_back({autoupdater::ApplyOperationType::Replace, "bin/a.txt", "bin/a.txt",
                               sha256(kNewAContents), std::string(kNewAContents).size()});
    plan.operations.push_back({autoupdater::ApplyOperationType::Replace, "bin/b.txt", "bin/b.txt",
                               sha256(kNewBContents), std::string(kNewBContents).size()});
    return plan;
}

autoupdater::ApplyPlan removePlan(const TemporaryDirectory& temporary) {
    const auto install = temporary.path() / "install";
    const auto target = std::filesystem::path("bin/old.txt");
    writeFile(install / target, kOldContents);

    autoupdater::ApplyPlan plan;
    plan.appId = "crash-recovery-test";
    plan.fromVersion = "1.0.0";
    plan.toVersion = "2.0.0";
    plan.releaseId = "forced-remove";
    plan.installDir = install;
    plan.stagingDir = temporary.path() / "staging";
    plan.backupDir = temporary.path() / "backup";
    plan.operations.push_back({autoupdater::ApplyOperationType::Remove, "", "bin/old.txt", "", 0});
    return plan;
}

std::filesystem::path savePlan(const TemporaryDirectory& temporary, const autoupdater::ApplyPlan& plan) {
    const auto path = temporary.path() / "apply-plan.json";
    writeFile(path, plan.toJson());
    return path;
}

std::filesystem::path savePlanAs(const TemporaryDirectory& temporary, const autoupdater::ApplyPlan& plan,
                                 const std::string& filename) {
    const auto path = temporary.path() / filename;
    writeFile(path, plan.toJson());
    return path;
}

struct ApplyBoundary {
    const char* name;
    bool operationScoped;
    bool activeRecorded;
    bool mutationCompleted;
    bool expectedNewAfterNextStart;
    bool backupCompleted;
};

void testReplaceAndJournalBoundaries(const std::filesystem::path& helper) {
    const std::vector<ApplyBoundary> boundaries = {
        {"journal.plan.after", false, false, false, true, false},
        {"journal.prepared.after", false, false, false, true, false},
        {"journal.operation_initial.after", true, false, false, true, false},
        {"backup.before", true, false, false, true, false},
        {"backup.after", true, false, false, true, true},
        {"journal.backup_durable.after", true, false, false, true, true},
        {"journal.preparation_complete.after", false, false, false, true, true},
        {"journal.active.after", false, true, false, false, true},
        {"journal.applying.after", false, true, false, false, true},
        {"journal.apply_intent.after", true, true, false, false, true},
        {"replace.before", true, true, false, false, true},
        {"replace.after", true, true, true, false, true},
        {"journal.apply_complete.after", true, true, true, false, true},
        {"journal.files_applied.after", false, true, true, true, true},
        {"journal.complete.after", false, true, true, true, true},
        {"journal.terminal.after", false, true, true, true, true},
        {"journal.active_clear.after", false, false, true, true, true},
    };

    for (const auto& boundary : boundaries) {
        TemporaryDirectory temporary(std::string("replace-") + boundary.name);
        const auto plan = replacePlan(temporary, true);
        const auto planPath = savePlan(temporary, plan);
        const auto target = plan.installDir / "bin/app.txt";
        const auto backup = plan.backupDir / "bin/app.txt";

        LAU_REQUIRE(crashAt(helper, planPath, boundary.name, boundary.operationScoped) == kCrashExitCode);
        requireActiveJournalState(plan, boundary.activeRecorded, boundary.name);
        LAU_REQUIRE(readFile(target) == (boundary.mutationCompleted ? kNewContents : kOldContents));
        if (std::filesystem::exists(backup) != boundary.backupCompleted) {
            const auto detail = std::filesystem::is_regular_file(backup) ? "; contents=" + readFile(backup) : "";
            throw std::runtime_error("Unexpected backup state after checkpoint: " + std::string(boundary.name) +
                                     "; path=" + backup.string() + detail);
        }
        if (boundary.backupCompleted) {
            LAU_REQUIRE(readFile(backup) == kOldContents);
        }

        LAU_REQUIRE(recover(helper, planPath) == 0);
        requireActiveJournalCleared(plan, boundary.name);
        LAU_REQUIRE(readFile(target) == (boundary.expectedNewAfterNextStart ? kNewContents : kOldContents));
    }
}

void testUpdateLockLifecycle(const std::filesystem::path& helper) {
    TemporaryDirectory temporary("update-lock-lifecycle");
    const auto plan = replacePlan(temporary, true);
    const auto planPath = savePlan(temporary, plan);
    const auto target = plan.installDir / "bin/app.txt";
    const auto lockPath = plan.installDir / ".autoupdater" / "update.lock";

    auto fileSystem = autoupdater::createDefaultFileSystem();
    auto installRoot = fileSystem->openRoot(plan.installDir, autoupdater::RootAccess::ReadWrite, true,
                                            autoupdater::RootedDirectoryCreationMode::InstalledContent);
    LAU_REQUIRE(installRoot);
    auto held = installRoot.value()->acquireExclusiveLock(".autoupdater/update.lock");
    LAU_REQUIRE(held);
    LAU_REQUIRE(std::filesystem::is_regular_file(lockPath));

    // A second updater must fail before it can create or recover a journal.
    LAU_REQUIRE(expectLockBlocked(helper, planPath) == 0);
    LAU_REQUIRE(!std::filesystem::exists(activeJournal(plan.installDir)));
    LAU_REQUIRE(readFile(target) == kOldContents);
    held.value().reset();

    // Forced termination bypasses all destructors. The next process must still
    // acquire the kernel lock while reusing the persistent regular marker.
    LAU_REQUIRE(crashAt(helper, planPath, "journal.active.after", false) == kCrashExitCode);
    LAU_REQUIRE(std::filesystem::is_regular_file(lockPath));
    requireActiveJournalState(plan, true, "update lock owner crash");

    // Lock ownership is never inferred from marker contents. A live PID paired
    // with deliberately wrong start identity models PID reuse and must not turn
    // an unlocked marker into a permanent lock.
    writeFile(lockPath, "pid=" + std::to_string(processId()) + "\nstart=definitely-not-this-process\n");
    LAU_REQUIRE(recover(helper, planPath) == 0);
    requireActiveJournalCleared(plan, "update lock crash recovery");
    LAU_REQUIRE(std::filesystem::is_regular_file(lockPath));
    LAU_REQUIRE(readFile(target) == kOldContents);
}

void testRemoveBoundaries(const std::filesystem::path& helper) {
    const std::vector<ApplyBoundary> boundaries = {
        {"remove.before", true, true, false, false, true},
        {"remove.after", true, true, true, false, true},
        {"journal.apply_complete.after", true, true, true, false, true},
        {"journal.files_applied.after", false, true, true, true, true},
        {"journal.complete.after", false, true, true, true, true},
        {"journal.terminal.after", false, true, true, true, true},
        {"journal.active_clear.after", false, false, true, true, true},
    };

    for (const auto& boundary : boundaries) {
        TemporaryDirectory temporary(std::string("remove-") + boundary.name);
        const auto plan = removePlan(temporary);
        const auto planPath = savePlan(temporary, plan);
        const auto target = plan.installDir / "bin/old.txt";
        const auto backup = plan.backupDir / "bin/old.txt";

        LAU_REQUIRE(crashAt(helper, planPath, boundary.name, boundary.operationScoped) == kCrashExitCode);
        requireActiveJournalState(plan, boundary.activeRecorded, boundary.name);
        LAU_REQUIRE(std::filesystem::exists(target) != boundary.mutationCompleted);
        LAU_REQUIRE(readFile(backup) == kOldContents);

        LAU_REQUIRE(recover(helper, planPath) == 0);
        requireActiveJournalCleared(plan, boundary.name);
        LAU_REQUIRE(std::filesystem::exists(target) != boundary.expectedNewAfterNextStart);
        if (!boundary.expectedNewAfterNextStart) {
            LAU_REQUIRE(readFile(target) == kOldContents);
        }
    }
}

struct RollbackBoundary {
    const char* name;
    bool operationScoped;
    bool rollbackMutationCompleted;
};

void testRollbackReplaceBoundaries(const std::filesystem::path& helper) {
    const std::vector<RollbackBoundary> boundaries = {
        {"journal.rollback_summary.after", false, false}, {"journal.rollback_intent.after", true, false},
        {"rollback.replace.before", true, false},         {"rollback.replace.after", true, true},
        {"journal.rollback_complete.after", true, true},  {"journal.rolled_back.after", false, true},
    };

    for (const auto& boundary : boundaries) {
        TemporaryDirectory temporary(std::string("rollback-replace-") + boundary.name);
        const auto plan = replacePlan(temporary, true);
        const auto planPath = savePlan(temporary, plan);
        const auto target = plan.installDir / "bin/app.txt";

        LAU_REQUIRE(crashAt(helper, planPath, "replace.after", true) == kCrashExitCode);
        LAU_REQUIRE(readFile(target) == kNewContents);
        LAU_REQUIRE(crashAt(helper, planPath, boundary.name, boundary.operationScoped) == kCrashExitCode);
        requireActiveJournalState(plan, true, boundary.name);
        LAU_REQUIRE(readFile(target) == (boundary.rollbackMutationCompleted ? kOldContents : kNewContents));

        LAU_REQUIRE(recover(helper, planPath) == 0);
        requireActiveJournalCleared(plan, boundary.name);
        LAU_REQUIRE(readFile(target) == kOldContents);
    }
}

void testRollbackRemoveBoundaries(const std::filesystem::path& helper) {
    const std::vector<RollbackBoundary> boundaries = {
        {"journal.rollback_intent.after", true, false},
        {"rollback.remove.before", true, false},
        {"rollback.remove.after", true, true},
        {"journal.rollback_complete.after", true, true},
    };

    for (const auto& boundary : boundaries) {
        TemporaryDirectory temporary(std::string("rollback-remove-") + boundary.name);
        const auto plan = replacePlan(temporary, false);
        const auto planPath = savePlan(temporary, plan);
        const auto target = plan.installDir / "bin/app.txt";

        LAU_REQUIRE(crashAt(helper, planPath, "replace.after", true) == kCrashExitCode);
        LAU_REQUIRE(readFile(target) == kNewContents);
        LAU_REQUIRE(crashAt(helper, planPath, boundary.name, boundary.operationScoped) == kCrashExitCode);
        requireActiveJournalState(plan, true, boundary.name);
        LAU_REQUIRE(std::filesystem::exists(target) != boundary.rollbackMutationCompleted);

        LAU_REQUIRE(recover(helper, planPath) == 0);
        requireActiveJournalCleared(plan, boundary.name);
        LAU_REQUIRE(!std::filesystem::exists(target));
    }
}

void testRepeatedPlanReplaysTerminalReceipt(const std::filesystem::path& helper) {
    {
        TemporaryDirectory temporary("terminal-receipt-repeat");
        const auto plan = replacePlan(temporary, true);
        const auto planPath = savePlan(temporary, plan);
        const auto target = plan.installDir / "bin/app.txt";

        LAU_REQUIRE(recover(helper, planPath) == 0);
        LAU_REQUIRE(readFile(target) == kNewContents);
        const auto firstSnapshots = planSnapshotNames(plan);
        LAU_REQUIRE(firstSnapshots.size() == 1);

        LAU_REQUIRE(recover(helper, planPath) == 0);
        requireActiveJournalCleared(plan, "terminal receipt replay");
        LAU_REQUIRE(readFile(target) == kNewContents);
        const auto secondSnapshots = planSnapshotNames(plan);
        LAU_REQUIRE(secondSnapshots == firstSnapshots);
    }

    {
        TemporaryDirectory temporary("terminal-receipt-active-clear-crash");
        const auto plan = replacePlan(temporary, true);
        const auto planPath = savePlan(temporary, plan);
        const auto target = plan.installDir / "bin/app.txt";

        LAU_REQUIRE(crashAt(helper, planPath, "journal.active_clear.after", false) == kCrashExitCode);
        LAU_REQUIRE(!std::filesystem::exists(activeJournal(plan.installDir)));
        LAU_REQUIRE(std::filesystem::exists(plan.installDir / ".autoupdater" / "journal" / "terminal.json"));
        LAU_REQUIRE(readFile(target) == kNewContents);
        const auto firstSnapshots = planSnapshotNames(plan);
        LAU_REQUIRE(firstSnapshots.size() == 1);
        LAU_REQUIRE(recover(helper, planPath) == 0);
        requireActiveJournalCleared(plan, "terminal receipt after active clear crash");
        LAU_REQUIRE(readFile(target) == kNewContents);
        LAU_REQUIRE(planSnapshotNames(plan) == firstSnapshots);
    }
}

void testRollbackActiveClearBoundary(const std::filesystem::path& helper) {
    TemporaryDirectory temporary("rollback-active-clear");
    const auto plan = replacePlan(temporary, true);
    const auto planPath = savePlan(temporary, plan);
    const auto target = plan.installDir / "bin/app.txt";

    LAU_REQUIRE(crashAt(helper, planPath, "replace.after", true) == kCrashExitCode);
    LAU_REQUIRE(readFile(target) == kNewContents);
    LAU_REQUIRE(crashAt(helper, planPath, "journal.active_clear.after", false) == kCrashExitCode);
    LAU_REQUIRE(!std::filesystem::exists(activeJournal(plan.installDir)));
    LAU_REQUIRE(readFile(target) == kOldContents);
}

void testMultiOperationRollbackOrder(const std::filesystem::path& helper) {
    TemporaryDirectory temporary("multi-operation-rollback-order");
    const auto plan = multiReplacePlan(temporary);
    const auto planPath = savePlan(temporary, plan);
    const auto targetA = plan.installDir / "bin/a.txt";
    const auto targetB = plan.installDir / "bin/b.txt";

    LAU_REQUIRE(crashAtOperation(helper, planPath, "replace.after", 1) == kCrashExitCode);
    LAU_REQUIRE(readFile(targetA) == kNewAContents);
    LAU_REQUIRE(readFile(targetB) == kNewBContents);
    LAU_REQUIRE(crashAtOperation(helper, planPath, "rollback.replace.after", 1) == kCrashExitCode);
    requireActiveJournalState(plan, true, "rollback.replace.after operation 1");
    LAU_REQUIRE(readFile(targetA) == kNewAContents);
    LAU_REQUIRE(readFile(targetB) == kOldBContents);

    LAU_REQUIRE(recover(helper, planPath) == 0);
    requireActiveJournalCleared(plan, "multi-operation reverse rollback");
    LAU_REQUIRE(readFile(targetA) == kOldAContents);
    LAU_REQUIRE(readFile(targetB) == kOldBContents);
}

void testOperationErrorJournalBoundary(const std::filesystem::path& helper) {
    TemporaryDirectory temporary("operation-error-journal-boundary");
    const auto plan = replacePlan(temporary, true);
    const auto planPath = savePlan(temporary, plan);
    const auto target = plan.installDir / "bin/app.txt";
    const auto backup = plan.backupDir / "bin/app.txt";
    writeFile(plan.stagingDir / "bin/app.txt", "tampered-staged-content\n");

    LAU_REQUIRE(crashAt(helper, planPath, "journal.operation_error.after", true) == kCrashExitCode);
    requireActiveJournalState(plan, true, "journal.operation_error.after");
    LAU_REQUIRE(readFile(target) == kOldContents);
    LAU_REQUIRE(readFile(backup) == kOldContents);

    LAU_REQUIRE(recover(helper, planPath) == 0);
    requireActiveJournalCleared(plan, "journal.operation_error.after");
    LAU_REQUIRE(readFile(target) == kOldContents);
}

enum class RecoveryEvidenceDamage { MissingOperation, CorruptOperation, MissingBackup, CorruptBackup };

void testDamagedRecoveryEvidenceFailsClosed(const std::filesystem::path& helper) {
    const std::vector<RecoveryEvidenceDamage> damages = {
        RecoveryEvidenceDamage::MissingOperation,
        RecoveryEvidenceDamage::CorruptOperation,
        RecoveryEvidenceDamage::MissingBackup,
        RecoveryEvidenceDamage::CorruptBackup,
    };
    for (const auto damage : damages) {
        TemporaryDirectory temporary("damaged-recovery-evidence-" + std::to_string(static_cast<int>(damage)));
        const auto plan = replacePlan(temporary, true);
        const auto planPath = savePlan(temporary, plan);
        const auto target = plan.installDir / "bin/app.txt";
        const auto backup = plan.backupDir / "bin/app.txt";
        const auto interventionMarker = temporary.path() / "unused-launch-marker.txt";

        LAU_REQUIRE(crashAt(helper, planPath, "replace.after", true) == kCrashExitCode);
        LAU_REQUIRE(readFile(target) == kNewContents);
        const auto active = readActiveTransaction(plan);
        const auto operation =
            plan.installDir /
            std::filesystem::u8path(autoupdater::updater::ApplyJournalStore::operationPath(active.transactionId, 0));
        std::error_code error;
        switch (damage) {
        case RecoveryEvidenceDamage::MissingOperation:
            LAU_REQUIRE(std::filesystem::remove(operation, error));
            LAU_REQUIRE(!error);
            break;
        case RecoveryEvidenceDamage::CorruptOperation:
            writeFile(operation, "{not-valid-json");
            break;
        case RecoveryEvidenceDamage::MissingBackup:
            LAU_REQUIRE(std::filesystem::remove(backup, error));
            LAU_REQUIRE(!error);
            break;
        case RecoveryEvidenceDamage::CorruptBackup:
            writeFile(backup, "corrupt-backup\n");
            break;
        }

        LAU_REQUIRE(recoverForOperatorIntervention(helper, planPath, interventionMarker) == 0);
        requireActiveJournalState(plan, true, "damaged rollback evidence");
        LAU_REQUIRE(readFile(target) == kNewContents);
        const auto summary = readSummary(plan, active.transactionId);
        LAU_REQUIRE(summary.fileState == autoupdater::updater::JournalFileState::RecoveryFailed);
        LAU_REQUIRE(launchCount(interventionMarker) == 0);
    }
}

void testInjectedFailureApplyBoundaries(const std::filesystem::path& helper) {
    const std::vector<std::pair<const char*, bool>> boundaries = {
        {"journal.plan.after", false},
        {"journal.prepared.after", false},
        {"journal.operation_initial.after", true},
        {"backup.before", true},
        {"backup.after", true},
        {"journal.backup_durable.after", true},
        {"journal.preparation_complete.after", false},
        {"journal.active.after", false},
        {"journal.applying.after", false},
        {"journal.apply_intent.after", true},
        {"replace.before", true},
        {"replace.after", true},
        {"journal.apply_complete.after", true},
        {"journal.files_applied.after", false},
        {"journal.complete.after", false},
        {"journal.terminal.after", false},
        {"journal.active_clear.after", false},
    };

    for (const auto& boundary : boundaries) {
        TemporaryDirectory temporary(std::string("injected-replace-") + boundary.first);
        const auto plan = replacePlan(temporary, true);
        const auto planPath = savePlan(temporary, plan);
        const auto target = plan.installDir / "bin/app.txt";

        LAU_REQUIRE(failAt(helper, planPath, boundary.first, boundary.second) == 0);
        LAU_REQUIRE(recover(helper, planPath) == 0);
        requireActiveJournalCleared(plan, boundary.first);
        LAU_REQUIRE(readFile(target) == kNewContents);
    }
}

void testInjectedFailureOperationErrorBoundary(const std::filesystem::path& helper) {
    TemporaryDirectory temporary("injected-operation-error");
    const auto plan = replacePlan(temporary, true);
    const auto planPath = savePlan(temporary, plan);
    const auto target = plan.installDir / "bin/app.txt";
    writeFile(plan.stagingDir / "bin/app.txt", "tampered-staged-content\n");

    LAU_REQUIRE(failAt(helper, planPath, "journal.operation_error.after", true) == 0);
    requireActiveJournalCleared(plan, "injected journal.operation_error.after");
    LAU_REQUIRE(readFile(target) == kOldContents);

    writeFile(plan.stagingDir / "bin/app.txt", kNewContents);
    LAU_REQUIRE(recover(helper, planPath) == 0);
    requireActiveJournalCleared(plan, "retry after injected journal.operation_error.after");
    LAU_REQUIRE(readFile(target) == kNewContents);
}

void testInjectedFailureRemoveBoundaries(const std::filesystem::path& helper) {
    for (const auto* boundary : {"remove.before", "remove.after"}) {
        TemporaryDirectory temporary(std::string("injected-remove-") + boundary);
        const auto plan = removePlan(temporary);
        const auto planPath = savePlan(temporary, plan);
        const auto target = plan.installDir / "bin/old.txt";

        LAU_REQUIRE(failAt(helper, planPath, boundary, true) == 0);
        LAU_REQUIRE(recover(helper, planPath) == 0);
        requireActiveJournalCleared(plan, boundary);
        LAU_REQUIRE(!std::filesystem::exists(target));
    }
}

void testInjectedFailureRollbackReplaceBoundaries(const std::filesystem::path& helper) {
    const std::vector<std::pair<const char*, bool>> boundaries = {
        {"journal.rollback_summary.after", false}, {"journal.rollback_intent.after", true},
        {"rollback.replace.before", true},         {"rollback.replace.after", true},
        {"journal.rollback_complete.after", true}, {"journal.rolled_back.after", false},
    };

    for (const auto& boundary : boundaries) {
        TemporaryDirectory temporary(std::string("injected-rollback-replace-") + boundary.first);
        const auto plan = multiReplacePlan(temporary);
        const auto planPath = savePlan(temporary, plan);
        const auto targetA = plan.installDir / "bin/a.txt";
        const auto targetB = plan.installDir / "bin/b.txt";
        writeFile(plan.stagingDir / "bin/b.txt", "tampered-staged-content\n");

        const auto failed = boundary.second ? failAtOperation(helper, planPath, boundary.first, 0)
                                            : failAt(helper, planPath, boundary.first, false);
        LAU_REQUIRE(failed == 0);
        requireActiveJournalState(plan, true, boundary.first);

        LAU_REQUIRE(recover(helper, planPath) == 0);
        requireActiveJournalCleared(plan, boundary.first);
        LAU_REQUIRE(readFile(targetA) == kOldAContents);
        LAU_REQUIRE(readFile(targetB) == kOldBContents);

        writeFile(plan.stagingDir / "bin/b.txt", kNewBContents);
        LAU_REQUIRE(recover(helper, planPath) == 0);
        requireActiveJournalCleared(plan, std::string("retry after ") + boundary.first);
        LAU_REQUIRE(readFile(targetA) == kNewAContents);
        LAU_REQUIRE(readFile(targetB) == kNewBContents);
    }
}

void testInjectedFailureRollbackRemoveBoundaries(const std::filesystem::path& helper) {
    for (const auto* boundary : {"rollback.remove.before", "rollback.remove.after"}) {
        TemporaryDirectory temporary(std::string("injected-rollback-remove-") + boundary);
        const auto plan = multiReplaceWithMissingOriginalPlan(temporary);
        const auto planPath = savePlan(temporary, plan);
        const auto targetA = plan.installDir / "bin/a.txt";
        const auto targetB = plan.installDir / "bin/b.txt";
        writeFile(plan.stagingDir / "bin/b.txt", "tampered-staged-content\n");

        LAU_REQUIRE(failAtOperation(helper, planPath, boundary, 0) == 0);
        requireActiveJournalState(plan, true, boundary);

        LAU_REQUIRE(recover(helper, planPath) == 0);
        requireActiveJournalCleared(plan, boundary);
        LAU_REQUIRE(!std::filesystem::exists(targetA));
        LAU_REQUIRE(readFile(targetB) == kOldBContents);

        writeFile(plan.stagingDir / "bin/b.txt", kNewBContents);
        LAU_REQUIRE(recover(helper, planPath) == 0);
        requireActiveJournalCleared(plan, std::string("retry after ") + boundary);
        LAU_REQUIRE(readFile(targetA) == kNewAContents);
        LAU_REQUIRE(readFile(targetB) == kNewBContents);
    }
}

void testInjectedFailureRecoveryFailureBoundaries(const std::filesystem::path& helper) {
    const std::vector<std::pair<const char*, bool>> boundaries = {
        {"journal.rollback_failed.after", true},
        {"journal.recovery_failed.after", false},
    };

    for (const auto& boundary : boundaries) {
        TemporaryDirectory temporary(std::string("injected-recovery-failure-") + boundary.first);
        const auto plan = replacePlan(temporary, true);
        const auto planPath = savePlan(temporary, plan);
        const auto target = plan.installDir / "bin/app.txt";
        const auto backup = plan.backupDir / "bin/app.txt";

        LAU_REQUIRE(crashAt(helper, planPath, "replace.after", true) == kCrashExitCode);
        LAU_REQUIRE(readFile(target) == kNewContents);
        std::error_code error;
        LAU_REQUIRE(std::filesystem::remove(backup, error));
        LAU_REQUIRE(!error);

        LAU_REQUIRE(failAt(helper, planPath, boundary.first, boundary.second) == 0);
        requireActiveJournalState(plan, true, boundary.first);
        const auto active = readActiveTransaction(plan);
        const auto failedOperation = readOperation(plan, active.transactionId, 0);
        LAU_REQUIRE(failedOperation.rollbackState == autoupdater::updater::JournalRollbackState::Failed);
        LAU_REQUIRE(!failedOperation.error.empty());

        writeFile(backup, kOldContents);
        LAU_REQUIRE(recover(helper, planPath) == 0);
        requireActiveJournalCleared(plan, boundary.first);
        LAU_REQUIRE(readFile(target) == kOldContents);

        LAU_REQUIRE(recover(helper, planPath) == 0);
        requireActiveJournalCleared(plan, std::string("retry after ") + boundary.first);
        LAU_REQUIRE(readFile(target) == kNewContents);
    }
}

void testForcedTerminationRecoveryFailureBoundaries(const std::filesystem::path& helper) {
    const std::vector<std::pair<const char*, bool>> boundaries = {
        {"journal.rollback_failed.after", true},
        {"journal.recovery_failed.after", false},
    };

    for (const auto& boundary : boundaries) {
        TemporaryDirectory temporary(std::string("crash-recovery-failure-") + boundary.first);
        const auto plan = replacePlan(temporary, true);
        const auto planPath = savePlan(temporary, plan);
        const auto target = plan.installDir / "bin/app.txt";
        const auto backup = plan.backupDir / "bin/app.txt";
        const auto marker = temporary.path() / "unused-launch-marker.txt";

        LAU_REQUIRE(crashAt(helper, planPath, "replace.after", true) == kCrashExitCode);
        const auto active = readActiveTransaction(plan);
        std::error_code error;
        LAU_REQUIRE(std::filesystem::remove(backup, error));
        LAU_REQUIRE(!error);

        LAU_REQUIRE(crashAt(helper, planPath, boundary.first, boundary.second) == kCrashExitCode);
        requireActiveJournalState(plan, true, boundary.first);
        LAU_REQUIRE(readFile(target) == kNewContents);

        LAU_REQUIRE(recoverForOperatorIntervention(helper, planPath, marker) == 0);
        requireActiveJournalState(plan, true, std::string("fail-closed after ") + boundary.first);
        LAU_REQUIRE(readFile(target) == kNewContents);
        const auto summary = readSummary(plan, active.transactionId);
        const auto operation = readOperation(plan, active.transactionId, 0);
        LAU_REQUIRE(summary.fileState == autoupdater::updater::JournalFileState::RecoveryFailed);
        LAU_REQUIRE(!summary.rollbackError.empty());
        LAU_REQUIRE(operation.rollbackState == autoupdater::updater::JournalRollbackState::Failed);
        LAU_REQUIRE(!operation.error.empty());
        LAU_REQUIRE(launchCount(marker) == 0);
    }
}

void testInjectedFailureRestartBoundaries(const std::filesystem::path& helper) {
    for (const auto& boundary :
         std::vector<std::pair<const char*, std::size_t>>{{"journal.restart_intent.after", 0}, {"restart.after", 1}}) {
        TemporaryDirectory temporary(std::string("injected-restart-") + boundary.first);
        auto plan = replacePlan(temporary, true);
        plan.restartCommand = {"marker-launch"};
        const auto planPath = savePlan(temporary, plan);
        const auto marker = temporary.path() / "restart-launches.txt";

        LAU_REQUIRE(failAtWithMarker(helper, planPath, boundary.first, false, marker) == 0);
        LAU_REQUIRE(launchCount(marker) == boundary.second);
        LAU_REQUIRE(recoverWithMarker(helper, planPath, marker) == 0);
        requireActiveJournalCleared(plan, boundary.first);
        LAU_REQUIRE(launchCount(marker) == boundary.second);
        LAU_REQUIRE(recoverWithMarker(helper, planPath, marker) == 0);
        LAU_REQUIRE(launchCount(marker) == boundary.second);
    }

    {
        TemporaryDirectory temporary("injected-restart-failed-journal");
        auto plan = replacePlan(temporary, true);
        plan.restartCommand = {"marker-launch"};
        const auto planPath = savePlan(temporary, plan);
        const auto marker = temporary.path() / "restart-launches.txt";

        LAU_REQUIRE(failAtWithFailingMarker(helper, planPath, "journal.restart_failed.after", marker) == 0);
        LAU_REQUIRE(launchCount(marker) == 1);
        LAU_REQUIRE(recoverWithFailingMarker(helper, planPath, marker) == 0);
        requireActiveJournalCleared(plan, "injected journal.restart_failed.after");
        LAU_REQUIRE(launchCount(marker) == 1);
    }

    {
        TemporaryDirectory temporary("injected-terminal-reconciliation-failed");
        auto plan = replacePlan(temporary, true);
        plan.restartCommand = {"marker-launch"};
        const auto planPath = savePlan(temporary, plan);
        const auto target = plan.installDir / "bin/app.txt";
        const auto marker = temporary.path() / "restart-launches.txt";

        LAU_REQUIRE(crashAtWithMarker(helper, planPath, "restart.after", false, marker) == kCrashExitCode);
        writeFile(target, kPostRestartContents);
        LAU_REQUIRE(failAtWithMarker(helper, planPath, "journal.terminal_reconciliation_failed.after", false, marker) ==
                    0);
        requireActiveJournalState(plan, true, "injected journal.terminal_reconciliation_failed.after");
        LAU_REQUIRE(launchCount(marker) == 1);

        writeFile(target, kNewContents);
        LAU_REQUIRE(recoverWithMarker(helper, planPath, marker) == 0);
        requireActiveJournalCleared(plan, "retry after injected terminal reconciliation failure");
        LAU_REQUIRE(readFile(target) == kNewContents);
        LAU_REQUIRE(launchCount(marker) == 1);
    }
}

enum class ActiveEvidenceDamage { MissingSummary, CorruptSummary, MissingPlan, CorruptPlan };

void testDamagedActiveTransactionRecordsFailClosed(const std::filesystem::path& helper) {
    {
        TemporaryDirectory temporary("corrupt-active-record");
        const auto plan = replacePlan(temporary, true);
        const auto planPath = savePlan(temporary, plan);
        const auto target = plan.installDir / "bin/app.txt";
        constexpr const char* corruptActive = "{not-valid-json";

        LAU_REQUIRE(crashAt(helper, planPath, "journal.active.after", false) == kCrashExitCode);
        writeFile(activeJournal(plan.installDir), corruptActive);
        LAU_REQUIRE(expectBlocked(helper, planPath) == 0);
        LAU_REQUIRE(readFile(activeJournal(plan.installDir)) == corruptActive);
        LAU_REQUIRE(readFile(target) == kOldContents);
    }

    for (const auto damage : {ActiveEvidenceDamage::MissingSummary, ActiveEvidenceDamage::CorruptSummary,
                              ActiveEvidenceDamage::MissingPlan, ActiveEvidenceDamage::CorruptPlan}) {
        TemporaryDirectory temporary("damaged-active-record-" + std::to_string(static_cast<int>(damage)));
        const auto plan = replacePlan(temporary, true);
        const auto planPath = savePlan(temporary, plan);
        const auto target = plan.installDir / "bin/app.txt";

        LAU_REQUIRE(crashAt(helper, planPath, "replace.after", true) == kCrashExitCode);
        const auto active = readActiveTransaction(plan);
        const bool summaryDamage =
            damage == ActiveEvidenceDamage::MissingSummary || damage == ActiveEvidenceDamage::CorruptSummary;
        const bool missing =
            damage == ActiveEvidenceDamage::MissingSummary || damage == ActiveEvidenceDamage::MissingPlan;
        const auto damagedPath =
            summaryDamage ? summaryJournal(plan, active.transactionId) : planJournal(plan, active.transactionId);
        if (missing) {
            std::error_code error;
            LAU_REQUIRE(std::filesystem::remove(damagedPath, error));
            LAU_REQUIRE(!error);
        } else {
            writeFile(damagedPath, "{not-valid-json");
        }

        LAU_REQUIRE(expectBlocked(helper, planPath) == 0);
        requireActiveJournalState(plan, true, "damaged active transaction record");
        LAU_REQUIRE(readActiveTransaction(plan).transactionId == active.transactionId);
        LAU_REQUIRE(readFile(target) == kNewContents);
    }

    struct InvalidSummaryState {
        autoupdater::updater::JournalFileState fileState;
        autoupdater::updater::JournalRestartState restartState;
        bool restartErrorRequired;
        const char* name;
    };
    const std::vector<InvalidSummaryState> invalidStates = {
        {autoupdater::updater::JournalFileState::Applying, autoupdater::updater::JournalRestartState::Intent, false,
         "applying-intent"},
        {autoupdater::updater::JournalFileState::Applying, autoupdater::updater::JournalRestartState::Launched, false,
         "applying-launched"},
        {autoupdater::updater::JournalFileState::RolledBack, autoupdater::updater::JournalRestartState::OutcomeUnknown,
         true, "rolled-back-outcome-unknown"},
        {autoupdater::updater::JournalFileState::Complete, autoupdater::updater::JournalRestartState::NotAttempted,
         false, "complete-not-attempted"},
    };
    for (const auto& state : invalidStates) {
        TemporaryDirectory temporary(std::string("semantically-invalid-summary-") + state.name);
        auto plan = replacePlan(temporary, true);
        plan.restartCommand = {"marker-launch"};
        const auto planPath = savePlan(temporary, plan);
        const auto target = plan.installDir / "bin/app.txt";

        LAU_REQUIRE(crashAt(helper, planPath, "replace.after", true) == kCrashExitCode);
        const auto active = readActiveTransaction(plan);
        auto summary = readSummary(plan, active.transactionId);
        summary.fileState = state.fileState;
        summary.restartState = state.restartState;
        summary.restartError = state.restartErrorRequired
                                   ? autoupdater::updater::JournalError{"ApplyLaunchFailed", "injected outcome unknown"}
                                   : autoupdater::updater::JournalError{};
        const auto serialized = autoupdater::updater::serializeApplyJournalSummary(summary);
        LAU_REQUIRE(serialized);
        writeFile(summaryJournal(plan, active.transactionId), serialized.value());

        LAU_REQUIRE(expectBlocked(helper, planPath) == 0);
        requireActiveJournalState(plan, true, state.name);
        LAU_REQUIRE(readFile(target) == kNewContents);
    }
}

void testRecoveryFailureRetryCrashClearsStaleError(const std::filesystem::path& helper) {
    TemporaryDirectory temporary("recovery-failure-retry-crash");
    const auto plan = replacePlan(temporary, true);
    const auto planPath = savePlan(temporary, plan);
    const auto target = plan.installDir / "bin/app.txt";
    const auto backup = plan.backupDir / "bin/app.txt";
    const auto marker = temporary.path() / "unused-launch-marker.txt";

    LAU_REQUIRE(crashAt(helper, planPath, "replace.after", true) == kCrashExitCode);
    const auto active = readActiveTransaction(plan);
    std::error_code error;
    LAU_REQUIRE(std::filesystem::remove(backup, error));
    LAU_REQUIRE(!error);
    LAU_REQUIRE(recoverForOperatorIntervention(helper, planPath, marker) == 0);
    auto summary = readSummary(plan, active.transactionId);
    LAU_REQUIRE(summary.fileState == autoupdater::updater::JournalFileState::RecoveryFailed);
    LAU_REQUIRE(!summary.rollbackError.empty());

    writeFile(backup, kOldContents);
    LAU_REQUIRE(crashAt(helper, planPath, "journal.rollback_summary.after", false) == kCrashExitCode);
    requireActiveJournalState(plan, true, "RecoveryFailed retry rollback summary");
    summary = readSummary(plan, active.transactionId);
    LAU_REQUIRE(summary.fileState == autoupdater::updater::JournalFileState::RollingBack);
    LAU_REQUIRE(summary.rollbackError.empty());
    LAU_REQUIRE(readFile(target) == kNewContents);

    LAU_REQUIRE(recover(helper, planPath) == 0);
    requireActiveJournalCleared(plan, "RecoveryFailed retry after rollback summary crash");
    LAU_REQUIRE(readFile(target) == kOldContents);
}

void testCorruptTerminalBlocksNewApply(const std::filesystem::path& helper) {
    TemporaryDirectory temporary("corrupt-terminal-record");
    const auto plan = replacePlan(temporary, true);
    const auto planPath = savePlan(temporary, plan);
    const auto target = plan.installDir / "bin/app.txt";

    LAU_REQUIRE(recover(helper, planPath) == 0);
    requireActiveJournalCleared(plan, "initial apply before corrupt terminal");
    LAU_REQUIRE(readFile(target) == kNewContents);
    const auto snapshots = planSnapshotNames(plan);
    LAU_REQUIRE(snapshots.size() == 1);

    constexpr const char* corruptTerminal = "{not-valid-json";
    writeFile(terminalJournal(plan), corruptTerminal);
    auto nextPlan = plan;
    nextPlan.toVersion = "3.0.0";
    nextPlan.releaseId = "blocked-by-corrupt-terminal";
    nextPlan.backupDir = temporary.path() / "backup-next";
    writeFile(nextPlan.stagingDir / "bin/app.txt", kThirdContents);
    nextPlan.operations[0].sha256 = sha256(kThirdContents);
    nextPlan.operations[0].size = std::string(kThirdContents).size();
    const auto nextPlanPath = savePlanAs(temporary, nextPlan, "apply-plan-next.json");

    LAU_REQUIRE(expectBlocked(helper, nextPlanPath) == 0);
    requireActiveJournalCleared(nextPlan, "corrupt terminal blocks new apply");
    LAU_REQUIRE(readFile(target) == kNewContents);
    LAU_REQUIRE(readFile(terminalJournal(nextPlan)) == corruptTerminal);
    LAU_REQUIRE(planSnapshotNames(nextPlan) == snapshots);
}

void testInterruptedRestartIsNotRepeated(const std::filesystem::path& helper) {
    {
        TemporaryDirectory temporary("restart-intent-interrupted");
        auto plan = replacePlan(temporary, true);
        plan.restartCommand = {"marker-launch"};
        const auto planPath = savePlan(temporary, plan);
        const auto target = plan.installDir / "bin/app.txt";
        const auto marker = temporary.path() / "restart-launches.txt";

        LAU_REQUIRE(crashAtWithMarker(helper, planPath, "journal.restart_intent.after", false, marker) ==
                    kCrashExitCode);
        LAU_REQUIRE(readFile(target) == kNewContents);
        LAU_REQUIRE(launchCount(marker) == 0);
        const auto active = readActiveTransaction(plan);

        LAU_REQUIRE(recoverWithMarker(helper, planPath, marker) == 0);
        requireActiveJournalCleared(plan, "journal.restart_intent.after");
        LAU_REQUIRE(readFile(target) == kNewContents);
        LAU_REQUIRE(launchCount(marker) == 0);
        const auto summary = readSummary(plan, active.transactionId);
        LAU_REQUIRE(summary.fileState == autoupdater::updater::JournalFileState::Complete);
        LAU_REQUIRE(summary.restartState == autoupdater::updater::JournalRestartState::OutcomeUnknown);
        LAU_REQUIRE(recoverWithMarker(helper, planPath, marker) == 0);
        LAU_REQUIRE(launchCount(marker) == 0);
    }

    {
        TemporaryDirectory temporary("restart-launch-failed");
        auto plan = replacePlan(temporary, true);
        plan.restartCommand = {"marker-launch"};
        const auto planPath = savePlan(temporary, plan);
        const auto target = plan.installDir / "bin/app.txt";
        const auto marker = temporary.path() / "restart-launches.txt";

        LAU_REQUIRE(recoverWithFailingMarker(helper, planPath, marker) == 0);
        requireActiveJournalCleared(plan, "restart launcher failure");
        LAU_REQUIRE(readFile(target) == kNewContents);
        LAU_REQUIRE(launchCount(marker) == 1);
        const auto terminal = readTerminalTransaction(plan);
        const auto summary = readSummary(plan, terminal.transactionId);
        LAU_REQUIRE(summary.fileState == autoupdater::updater::JournalFileState::Complete);
        LAU_REQUIRE(summary.restartState == autoupdater::updater::JournalRestartState::Failed);
        LAU_REQUIRE(recoverWithFailingMarker(helper, planPath, marker) == 0);
        LAU_REQUIRE(launchCount(marker) == 1);
    }

    {
        TemporaryDirectory temporary("restart-failure-journal-interrupted");
        auto plan = replacePlan(temporary, true);
        plan.restartCommand = {"marker-launch"};
        const auto planPath = savePlan(temporary, plan);
        const auto target = plan.installDir / "bin/app.txt";
        const auto marker = temporary.path() / "restart-launches.txt";

        LAU_REQUIRE(crashAtWithFailingMarker(helper, planPath, "journal.restart_failed.after", marker) ==
                    kCrashExitCode);
        LAU_REQUIRE(readFile(target) == kNewContents);
        LAU_REQUIRE(launchCount(marker) == 1);
        const auto active = readActiveTransaction(plan);
        auto summary = readSummary(plan, active.transactionId);
        LAU_REQUIRE(summary.fileState == autoupdater::updater::JournalFileState::Complete);
        LAU_REQUIRE(summary.restartState == autoupdater::updater::JournalRestartState::Failed);

        LAU_REQUIRE(recoverWithFailingMarker(helper, planPath, marker) == 0);
        requireActiveJournalCleared(plan, "journal.restart_failed.after");
        LAU_REQUIRE(readFile(target) == kNewContents);
        LAU_REQUIRE(launchCount(marker) == 1);
        summary = readSummary(plan, active.transactionId);
        LAU_REQUIRE(summary.fileState == autoupdater::updater::JournalFileState::Complete);
        LAU_REQUIRE(summary.restartState == autoupdater::updater::JournalRestartState::Failed);
    }

    {
        TemporaryDirectory temporary("restart-outcome-unknown");
        auto plan = replacePlan(temporary, true);
        plan.restartCommand = {"marker-launch"};
        const auto planPath = savePlan(temporary, plan);
        const auto target = plan.installDir / "bin/app.txt";
        const auto marker = temporary.path() / "restart-launches.txt";

        LAU_REQUIRE(crashAtWithMarker(helper, planPath, "restart.after", false, marker) == kCrashExitCode);
        LAU_REQUIRE(readFile(target) == kNewContents);
        LAU_REQUIRE(launchCount(marker) == 1);
        const auto active = readActiveTransaction(plan);

        LAU_REQUIRE(recoverWithMarker(helper, planPath, marker) == 0);
        requireActiveJournalCleared(plan, "restart.after");
        LAU_REQUIRE(readFile(target) == kNewContents);
        LAU_REQUIRE(launchCount(marker) == 1);
        const auto summary = readSummary(plan, active.transactionId);
        LAU_REQUIRE(summary.fileState == autoupdater::updater::JournalFileState::Complete);
        LAU_REQUIRE(summary.restartState == autoupdater::updater::JournalRestartState::OutcomeUnknown);
        LAU_REQUIRE(recoverWithMarker(helper, planPath, marker) == 0);
        LAU_REQUIRE(launchCount(marker) == 1);
    }

    {
        TemporaryDirectory temporary("restart-outcome-unknown-mismatch");
        auto plan = replacePlan(temporary, true);
        plan.restartCommand = {"marker-launch"};
        const auto planPath = savePlan(temporary, plan);
        const auto target = plan.installDir / "bin/app.txt";
        const auto backup = plan.backupDir / "bin/app.txt";
        const auto marker = temporary.path() / "restart-launches.txt";

        LAU_REQUIRE(crashAtWithMarker(helper, planPath, "restart.after", false, marker) == kCrashExitCode);
        LAU_REQUIRE(launchCount(marker) == 1);
        const auto active = readActiveTransaction(plan);
        writeFile(target, kPostRestartContents);

        LAU_REQUIRE(crashAtWithMarker(helper, planPath, "journal.terminal_reconciliation_failed.after", false,
                                      marker) == kCrashExitCode);
        requireActiveJournalState(plan, true, "journal.terminal_reconciliation_failed.after");
        LAU_REQUIRE(readFile(target) == kPostRestartContents);
        LAU_REQUIRE(readFile(backup) == kOldContents);
        LAU_REQUIRE(launchCount(marker) == 1);
        auto summary = readSummary(plan, active.transactionId);
        LAU_REQUIRE(summary.fileState == autoupdater::updater::JournalFileState::FilesApplied);
        LAU_REQUIRE(summary.restartState == autoupdater::updater::JournalRestartState::OutcomeUnknown);

        LAU_REQUIRE(recoverForOperatorIntervention(helper, planPath, marker) == 0);
        requireActiveJournalState(plan, true, "restart.after terminal reconciliation");
        LAU_REQUIRE(readFile(target) == kPostRestartContents);
        LAU_REQUIRE(readFile(backup) == kOldContents);
        LAU_REQUIRE(launchCount(marker) == 1);
        summary = readSummary(plan, active.transactionId);
        LAU_REQUIRE(summary.fileState == autoupdater::updater::JournalFileState::FilesApplied);
        LAU_REQUIRE(summary.restartState == autoupdater::updater::JournalRestartState::OutcomeUnknown);
    }

    {
        TemporaryDirectory temporary("restart-active-clear-crash");
        auto plan = replacePlan(temporary, true);
        plan.restartCommand = {"marker-launch"};
        const auto planPath = savePlan(temporary, plan);
        const auto target = plan.installDir / "bin/app.txt";
        const auto marker = temporary.path() / "restart-launches.txt";

        LAU_REQUIRE(crashAtWithMarker(helper, planPath, "journal.active_clear.after", false, marker) == kCrashExitCode);
        LAU_REQUIRE(!std::filesystem::exists(activeJournal(plan.installDir)));
        LAU_REQUIRE(readFile(target) == kNewContents);
        LAU_REQUIRE(launchCount(marker) == 1);

        LAU_REQUIRE(recoverWithMarker(helper, planPath, marker) == 0);
        LAU_REQUIRE(!std::filesystem::exists(activeJournal(plan.installDir)));
        LAU_REQUIRE(readFile(target) == kNewContents);
        LAU_REQUIRE(launchCount(marker) == 1);
    }
}

void testPublicRollbackCrashRecovery(const std::filesystem::path& helper) {
    {
        TemporaryDirectory terminalCrash("public-rollback-forward-terminal-crash");
        auto interruptedForward = replacePlan(terminalCrash, true);
        interruptedForward.releaseId = "public-rollback-forward-terminal-crash";
        const auto interruptedPath = savePlanAs(terminalCrash, interruptedForward, "forward-plan.json");
        const auto interruptedTarget = interruptedForward.installDir / "bin/app.txt";

        LAU_REQUIRE(crashAt(helper, interruptedPath, "journal.terminal.after", false) == kCrashExitCode);
        requireActiveJournalState(interruptedForward, true, "forward terminal publication");
        LAU_REQUIRE(readFile(interruptedTarget) == kNewContents);
        const auto interruptedTerminal = readTerminalTransaction(interruptedForward);

        autoupdater::ApplyPlan interruptedRequest;
        interruptedRequest.intent = autoupdater::ApplyPlanIntent::Rollback;
        interruptedRequest.rollbackOf = autoupdater::ApplyTransactionReference{
            interruptedTerminal.transactionId, interruptedTerminal.planDigest};
        interruptedRequest.appId = interruptedForward.appId;
        interruptedRequest.fromVersion = interruptedForward.toVersion;
        interruptedRequest.releaseId = interruptedForward.releaseId;
        interruptedRequest.installDir = interruptedForward.installDir;
        interruptedRequest.stagingDir = interruptedForward.backupDir;
        interruptedRequest.backupDir =
            autoupdater::util::defaultStagingRoot(interruptedForward.installDir) / "backup" / "rollback" /
            autoupdater::util::pathFromUtf8(interruptedTerminal.transactionId);
        const auto interruptedRequestPath =
            savePlanAs(terminalCrash, interruptedRequest, "rollback-request.json");

        LAU_REQUIRE(recover(helper, interruptedRequestPath) == 0);
        requireActiveJournalCleared(interruptedRequest, "rollback after forward terminal publication");
        LAU_REQUIRE(readFile(interruptedTarget) == kOldContents);
        LAU_REQUIRE(readTerminalTransaction(interruptedRequest).transactionId !=
                    interruptedTerminal.transactionId);
    }

    TemporaryDirectory temporary("public-rollback-crash-recovery");
    auto forward = replacePlan(temporary, true);
    forward.releaseId = "public-rollback-crash-recovery";
    const auto forwardPath = savePlanAs(temporary, forward, "forward-plan.json");
    const auto target = forward.installDir / "bin/app.txt";

    LAU_REQUIRE(recover(helper, forwardPath) == 0);
    LAU_REQUIRE(readFile(target) == kNewContents);
    const auto forwardTerminal = readTerminalTransaction(forward);

    autoupdater::ApplyPlan request;
    request.intent = autoupdater::ApplyPlanIntent::Rollback;
    request.rollbackOf =
        autoupdater::ApplyTransactionReference{forwardTerminal.transactionId, forwardTerminal.planDigest};
    request.appId = forward.appId;
    request.fromVersion = forward.toVersion;
    request.releaseId = forward.releaseId;
    request.installDir = forward.installDir;
    request.stagingDir = forward.backupDir;
    request.backupDir = autoupdater::util::defaultStagingRoot(forward.installDir) / "backup" / "rollback" /
                        autoupdater::util::pathFromUtf8(forwardTerminal.transactionId);
    const auto requestPath = savePlanAs(temporary, request, "rollback-request.json");

    LAU_REQUIRE(crashAt(helper, requestPath, "replace.after", true) == kCrashExitCode);
    requireActiveJournalState(request, true, "public rollback replace.after");
    LAU_REQUIRE(readFile(target) == kOldContents);
    LAU_REQUIRE(readFile(request.backupDir / "bin/app.txt") == kNewContents);

    LAU_REQUIRE(recover(helper, requestPath) == 0);
    requireActiveJournalCleared(request, "public rollback compensation recovery");
    LAU_REQUIRE(readFile(target) == kOldContents);
    const auto terminalAfterRecovery = readTerminalTransaction(forward);
    LAU_REQUIRE(terminalAfterRecovery.transactionId != forwardTerminal.transactionId);

    LAU_REQUIRE(recover(helper, requestPath) == 0);
    LAU_REQUIRE(readFile(target) == kOldContents);
    const auto rollbackTerminal = readTerminalTransaction(request);
    LAU_REQUIRE(rollbackTerminal.transactionId == terminalAfterRecovery.transactionId);
    LAU_REQUIRE(rollbackTerminal.planDigest == terminalAfterRecovery.planDigest);
    const auto rollbackSnapshot = autoupdater::ApplyPlan::parse(
        readFile(planJournal(request, rollbackTerminal.transactionId)));
    LAU_REQUIRE(rollbackSnapshot);
    LAU_REQUIRE(rollbackSnapshot.value().intent == autoupdater::ApplyPlanIntent::Rollback);
    LAU_REQUIRE(rollbackSnapshot.value().rollbackOf.has_value());
    LAU_REQUIRE(rollbackSnapshot.value().rollbackOf->transactionId == forwardTerminal.transactionId);
    LAU_REQUIRE(rollbackSnapshot.value().rollbackOf->planDigest == forwardTerminal.planDigest);
}

} // namespace

void testApplyExecutorRecoversAfterForcedTermination() {
    const auto helper = crashHelperPath();
    std::error_code error;
    LAU_REQUIRE(std::filesystem::is_regular_file(helper, error));
    LAU_REQUIRE(!error);

    testReplaceAndJournalBoundaries(helper);
    testUpdateLockLifecycle(helper);
    testRemoveBoundaries(helper);
    testRollbackReplaceBoundaries(helper);
    testRollbackRemoveBoundaries(helper);
    testRollbackActiveClearBoundary(helper);
    testMultiOperationRollbackOrder(helper);
    testOperationErrorJournalBoundary(helper);
    testDamagedRecoveryEvidenceFailsClosed(helper);
    testInjectedFailureApplyBoundaries(helper);
    testInjectedFailureOperationErrorBoundary(helper);
    testInjectedFailureRemoveBoundaries(helper);
    testInjectedFailureRollbackReplaceBoundaries(helper);
    testInjectedFailureRollbackRemoveBoundaries(helper);
    testInjectedFailureRecoveryFailureBoundaries(helper);
    testForcedTerminationRecoveryFailureBoundaries(helper);
    testInjectedFailureRestartBoundaries(helper);
    testDamagedActiveTransactionRecordsFailClosed(helper);
    testRecoveryFailureRetryCrashClearsStaleError(helper);
    testCorruptTerminalBlocksNewApply(helper);
    testRepeatedPlanReplaysTerminalReceipt(helper);
    testInterruptedRestartIsNotRepeated(helper);
    testPublicRollbackCrashRecovery(helper);
}
