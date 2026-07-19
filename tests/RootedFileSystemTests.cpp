#include "ApplyExecutor.h"
#include "DownloadExecutor.h"
#include "LocalSnapshotBuilder.h"
#include "TestCommon.h"

#include "libAutoUpdater/interfaces/IFileSystem.h"
#include "libAutoUpdater/interfaces/IHashProvider.h"
#include "libAutoUpdater/interfaces/INetworkClient.h"

#include <array>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <winioctl.h>
#endif

namespace {

std::filesystem::path makeTestRoot(const std::string& name) {
    const auto root = std::filesystem::temp_directory_path() / std::filesystem::u8path(name);
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    ec.clear();
    std::filesystem::create_directories(root, ec);
    LAU_REQUIRE(!ec);
    return root;
}

void writeFile(const std::filesystem::path& path, const std::string& contents) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    LAU_REQUIRE(!ec);
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    LAU_REQUIRE(static_cast<bool>(output));
    output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    LAU_REQUIRE(static_cast<bool>(output));
}

std::string readFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    LAU_REQUIRE(static_cast<bool>(input));
    std::ostringstream output;
    output << input.rdbuf();
    LAU_REQUIRE(!input.bad());
    return output.str();
}

std::filesystem::path findAtomicTemporaryPayload(const std::filesystem::path& root) {
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        const auto name = entry.path().filename().string();
        const auto parentName = entry.path().parent_path().filename().string();
        if (name.find(".autoupdater-tmp-") == 0 ||
            (name == "payload" && parentName.find(".autoupdater-private-") == 0)) {
            return entry.path();
        }
    }
    return {};
}

#ifdef _WIN32

struct MountPointReparseData {
    DWORD tag;
    WORD dataLength;
    WORD reserved;
    WORD substituteOffset;
    WORD substituteLength;
    WORD printOffset;
    WORD printLength;
    wchar_t pathBuffer[1];
};

void createDirectoryLink(const std::filesystem::path& link, const std::filesystem::path& target) {
    std::error_code ec;
    std::filesystem::create_directories(link.parent_path(), ec);
    LAU_REQUIRE(!ec);

    LAU_REQUIRE(CreateDirectoryW(link.c_str(), nullptr) != FALSE);
    const auto absoluteTarget = std::filesystem::absolute(target).lexically_normal().native();
    std::wstring substitute;
    if (absoluteTarget.rfind(L"\\\\", 0) == 0) {
        substitute = L"\\??\\UNC\\" + absoluteTarget.substr(2);
    } else {
        substitute = L"\\??\\" + absoluteTarget;
    }
    const auto substituteBytes = substitute.size() * sizeof(wchar_t);
    const auto printBytes = absoluteTarget.size() * sizeof(wchar_t);
    const auto bufferSize =
        offsetof(MountPointReparseData, pathBuffer) + substituteBytes + sizeof(wchar_t) + printBytes + sizeof(wchar_t);
    LAU_REQUIRE(bufferSize - 8 <= USHRT_MAX);

    std::vector<unsigned char> storage(bufferSize);
    auto* data = reinterpret_cast<MountPointReparseData*>(storage.data());
    data->tag = IO_REPARSE_TAG_MOUNT_POINT;
    data->dataLength = static_cast<WORD>(bufferSize - 8);
    data->substituteOffset = 0;
    data->substituteLength = static_cast<WORD>(substituteBytes);
    data->printOffset = static_cast<WORD>(substituteBytes + sizeof(wchar_t));
    data->printLength = static_cast<WORD>(printBytes);
    std::memcpy(data->pathBuffer, substitute.data(), substituteBytes);
    std::memcpy(reinterpret_cast<unsigned char*>(data->pathBuffer) + data->printOffset, absoluteTarget.data(),
                printBytes);

    const HANDLE handle = CreateFileW(link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                      FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    LAU_REQUIRE(handle != INVALID_HANDLE_VALUE);
    DWORD returned = 0;
    const BOOL linked = DeviceIoControl(handle, FSCTL_SET_REPARSE_POINT, data, static_cast<DWORD>(bufferSize), nullptr,
                                        0, &returned, nullptr);
    CloseHandle(handle);
    LAU_REQUIRE(linked != FALSE);
}

void createLeafLink(const std::filesystem::path& link, const std::filesystem::path& target) {
    std::error_code ec;
    std::filesystem::create_directories(link.parent_path(), ec);
    LAU_REQUIRE(!ec);
    constexpr DWORD allowUnprivilegedCreate = 0x2;
    if (CreateSymbolicLinkW(link.c_str(), target.c_str(), allowUnprivilegedCreate)) {
        return;
    }
    createDirectoryLink(link, target.parent_path());
}

#else

void createDirectoryLink(const std::filesystem::path& link, const std::filesystem::path& target) {
    std::error_code ec;
    std::filesystem::create_directories(link.parent_path(), ec);
    LAU_REQUIRE(!ec);
    std::filesystem::create_directory_symlink(target, link, ec);
    LAU_REQUIRE(!ec);
}

void createLeafLink(const std::filesystem::path& link, const std::filesystem::path& target) {
    std::error_code ec;
    std::filesystem::create_directories(link.parent_path(), ec);
    LAU_REQUIRE(!ec);
    std::filesystem::create_symlink(target, link, ec);
    LAU_REQUIRE(!ec);
}

#endif

enum class LinkRole { Source, Target, Backup };

std::string roleName(LinkRole role) {
    switch (role) {
    case LinkRole::Source:
        return "source";
    case LinkRole::Target:
        return "target";
    case LinkRole::Backup:
        return "backup";
    }
    return "unknown";
}

void runApplyLinkCase(LinkRole role, bool leaf) {
    const auto root = makeTestRoot("libAutoUpdater-rooted-apply-" + roleName(role) + (leaf ? "-leaf" : "-parent"));
    const auto install = root / "install";
    const auto staging = root / "staging";
    const auto backup = root / "backup";
    const auto outside = root / "outside";
    writeFile(install / "managed/file.bin", "old");
    writeFile(staging / "managed/file.bin", "new");
    writeFile(outside / "sentinel.txt", "outside");
    writeFile(outside / "source.bin", "new");

    const auto linkedRoot = role == LinkRole::Source ? staging : (role == LinkRole::Target ? install : backup);
    const auto linkedPath = leaf ? linkedRoot / "managed/file.bin" : linkedRoot / "managed";
    std::error_code ec;
    if (leaf) {
        std::filesystem::remove_all(linkedPath, ec);
        ec.clear();
        createLeafLink(linkedPath, outside / "source.bin");
    } else {
        std::filesystem::remove_all(linkedPath, ec);
        ec.clear();
        createDirectoryLink(linkedPath, outside);
    }

    auto hash = autoupdater::createDefaultHashProvider();
    auto expected = hash->sha256Bytes("new");
    LAU_REQUIRE(expected);
    autoupdater::ApplyPlan plan;
    plan.installDir = install;
    plan.stagingDir = staging;
    plan.backupDir = backup;
    plan.releaseId = "link-test";
    plan.operations.push_back(
        {autoupdater::ApplyOperationType::Replace, "managed/file.bin", "managed/file.bin", expected.value(), 3});

    const auto result = autoupdater::updater::executeApplyPlan(plan);
    LAU_REQUIRE(!result);
    LAU_REQUIRE(readFile(outside / "sentinel.txt") == "outside");
    LAU_REQUIRE(readFile(outside / "source.bin") == "new");
    if (role != LinkRole::Target) {
        LAU_REQUIRE(readFile(install / "managed/file.bin") == "old");
    }
    if (role == LinkRole::Target || role == LinkRole::Backup) {
        LAU_REQUIRE(!std::filesystem::exists(outside / "file.bin"));
    }

    std::filesystem::remove_all(root, ec);
}

void runApplyRootLinkCase(LinkRole role) {
    const auto root = makeTestRoot("libAutoUpdater-rooted-root-link-" + roleName(role));
    const auto installReal = root / "install-real";
    const auto stagingReal = root / "staging-real";
    const auto backupReal = root / "backup-real";
    writeFile(installReal / "managed/file.bin", "old");
    writeFile(stagingReal / "managed/file.bin", "new");
    std::filesystem::create_directories(backupReal);

    auto install = installReal;
    auto staging = stagingReal;
    auto backup = backupReal;
    const auto link = root / (roleName(role) + "-link");
    if (role == LinkRole::Source) {
        createDirectoryLink(link, stagingReal);
        staging = link;
    } else if (role == LinkRole::Target) {
        createDirectoryLink(link, installReal);
        install = link;
    } else {
        createDirectoryLink(link, backupReal);
        backup = link;
    }

    auto hash = autoupdater::createDefaultHashProvider();
    auto expected = hash->sha256Bytes("new");
    LAU_REQUIRE(expected);
    autoupdater::ApplyPlan plan;
    plan.installDir = install;
    plan.stagingDir = staging;
    plan.backupDir = backup;
    plan.releaseId = "root-link-test";
    plan.operations.push_back(
        {autoupdater::ApplyOperationType::Replace, "managed/file.bin", "managed/file.bin", expected.value(), 3});
    const auto result = autoupdater::updater::executeApplyPlan(plan);
    LAU_REQUIRE(!result);
    LAU_REQUIRE(readFile(installReal / "managed/file.bin") == "old");

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

class SwappingHashProvider final : public autoupdater::IHashProvider {
  public:
    SwappingHashProvider(std::filesystem::path path, std::filesystem::path moved)
        : path_(std::move(path)), moved_(std::move(moved)), delegate_(autoupdater::createDefaultHashProvider()) {}

    autoupdater::Result<std::string> sha256File(const std::filesystem::path& path) noexcept override {
        return delegate_->sha256File(path);
    }

    autoupdater::Result<std::string> sha256Bytes(std::string_view data) noexcept override {
        return delegate_->sha256Bytes(data);
    }

    autoupdater::Result<std::string> sha256Stream(autoupdater::IRootedFile& file) noexcept override {
        if (!swapped_) {
            std::error_code ec;
            std::filesystem::rename(path_, moved_, ec);
            if (ec) {
                return autoupdater::Result<std::string>::fail(
                    {autoupdater::ErrorCode::FileSystemError, "Failed to swap snapshot path: " + ec.message()});
            }
            std::ofstream replacement(path_, std::ios::binary | std::ios::trunc);
            if (!replacement) {
                return autoupdater::Result<std::string>::fail(
                    {autoupdater::ErrorCode::FileSystemError, "Failed to create swapped snapshot path"});
            }
            replacement << "replacement";
            replacement.close();
            swapped_ = true;
        }
        return delegate_->sha256Stream(file);
    }

  private:
    std::filesystem::path path_;
    std::filesystem::path moved_;
    std::shared_ptr<autoupdater::IHashProvider> delegate_;
    bool swapped_ = false;
};

class StaticDownloadClient final : public autoupdater::INetworkClient {
  public:
    explicit StaticDownloadClient(std::string contents) : contents_(std::move(contents)) {}

    autoupdater::Result<std::string> getText(const std::string&, const autoupdater::NetworkOptions&,
                                             autoupdater::CancellationToken&) noexcept override {
        return autoupdater::Result<std::string>::fail(
            {autoupdater::ErrorCode::NetworkError, "Text request is not supported by this test client"});
    }

    autoupdater::Result<autoupdater::DownloadResult>
    downloadToFile(const std::string&, autoupdater::IRootedFile& target, const autoupdater::NetworkOptions&,
                   const std::optional<autoupdater::DownloadResumeInfo>&, autoupdater::ProgressCallback,
                   autoupdater::CancellationToken&) noexcept override {
        called = true;
        auto written = target.write(contents_.data(), contents_.size());
        if (!written) {
            return autoupdater::Result<autoupdater::DownloadResult>::fail(written.error());
        }
        autoupdater::DownloadResult result;
        result.bytesWritten = contents_.size();
        return autoupdater::Result<autoupdater::DownloadResult>::ok(result);
    }

    bool called = false;

  private:
    std::string contents_;
};

class SwappingDownloadClient final : public autoupdater::INetworkClient {
  public:
    SwappingDownloadClient(std::filesystem::path originalParent, std::filesystem::path movedParent,
                           std::filesystem::path outside, std::string contents)
        : originalParent_(std::move(originalParent)), movedParent_(std::move(movedParent)),
          outside_(std::move(outside)), contents_(std::move(contents)) {}

    autoupdater::Result<std::string> getText(const std::string&, const autoupdater::NetworkOptions&,
                                             autoupdater::CancellationToken&) noexcept override {
        return autoupdater::Result<std::string>::fail(
            {autoupdater::ErrorCode::NetworkError, "Text request is not supported by this test client"});
    }

    autoupdater::Result<autoupdater::DownloadResult>
    downloadToFile(const std::string&, autoupdater::IRootedFile& target, const autoupdater::NetworkOptions&,
                   const std::optional<autoupdater::DownloadResumeInfo>&, autoupdater::ProgressCallback,
                   autoupdater::CancellationToken&) noexcept override {
        try {
            std::error_code ec;
            std::filesystem::rename(originalParent_, movedParent_, ec);
            if (ec) {
                return autoupdater::Result<autoupdater::DownloadResult>::fail(
                    {autoupdater::ErrorCode::FileSystemError, "Failed to swap download parent: " + ec.message()});
            }
            createDirectoryLink(originalParent_, outside_);
            auto written = target.write(contents_.data(), contents_.size());
            if (!written) {
                return autoupdater::Result<autoupdater::DownloadResult>::fail(written.error());
            }
            autoupdater::DownloadResult result;
            result.bytesWritten = contents_.size();
            return autoupdater::Result<autoupdater::DownloadResult>::ok(result);
        } catch (const std::exception& error) {
            return autoupdater::Result<autoupdater::DownloadResult>::fail(
                {autoupdater::ErrorCode::FileSystemError, error.what()});
        }
    }

  private:
    std::filesystem::path originalParent_;
    std::filesystem::path movedParent_;
    std::filesystem::path outside_;
    std::string contents_;
};

autoupdater::UpdateDecision oneFileDecision(const std::string& contents) {
    auto hash = autoupdater::createDefaultHashProvider();
    auto expected = hash->sha256Bytes(contents);
    LAU_REQUIRE(expected);
    autoupdater::UpdateDecision decision;
    autoupdater::PlannedDownload download;
    download.file.path = "managed/file.bin";
    download.file.sha256 = expected.value();
    download.file.size = contents.size();
    download.url = "test://artifact";
    decision.downloads.push_back(std::move(download));
    return decision;
}

} // namespace

void testApplyExecutorRejectsSourceTargetBackupLinks() {
    for (const auto role : {LinkRole::Source, LinkRole::Target, LinkRole::Backup}) {
        runApplyLinkCase(role, false);
        runApplyLinkCase(role, true);
        runApplyRootLinkCase(role);
    }

    const auto root = makeTestRoot("libAutoUpdater-rooted-journal-link");
    const auto install = root / "install";
    const auto outside = root / "outside";
    std::filesystem::create_directories(install);
    writeFile(outside / "sentinel.txt", "outside");
    createDirectoryLink(install / ".autoupdater", outside);
    autoupdater::ApplyPlan plan;
    plan.installDir = install;
    plan.stagingDir = root / "staging";
    plan.backupDir = root / "backup";
    plan.releaseId = "journal-link";
    auto result = autoupdater::updater::executeApplyPlan(plan);
    LAU_REQUIRE(!result);
    LAU_REQUIRE(readFile(outside / "sentinel.txt") == "outside");
    LAU_REQUIRE(!std::filesystem::exists(outside / "journal"));
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

void testRootedFileSystemPinsHandlesAndRejectsSwaps() {
    auto fileSystem = autoupdater::createDefaultFileSystem();

    {
        const auto root = makeTestRoot("libAutoUpdater-rooted-unsafe-paths");
        auto openedRoot = fileSystem->openRoot(root, autoupdater::RootAccess::ReadWrite, false);
        LAU_REQUIRE(openedRoot);
        std::vector<std::string> unsafe = {"../escape", "C:/absolute", "//server/share", "name:stream",
                                           "CON",       "file.",       "file ",          "a//b"};
        unsafe.emplace_back("nul\0suffix", 10);
        for (const auto& path : unsafe) {
            auto opened = openedRoot.value()->openRegularFile(path, autoupdater::RootedFileOpenMode::OpenOrCreate);
            LAU_REQUIRE(!opened);
        }
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }

    {
        const auto root = makeTestRoot("libAutoUpdater-rooted-source-swap");
        writeFile(root / "source.bin", "original");
        auto openedRoot = fileSystem->openRoot(root, autoupdater::RootAccess::ReadOnly, false);
        LAU_REQUIRE(openedRoot);
        auto source = openedRoot.value()->openRegularFile("source.bin", autoupdater::RootedFileOpenMode::ReadOnly);
        LAU_REQUIRE(source && source.value().exists());
        std::filesystem::rename(root / "source.bin", root / "moved.bin");
        writeFile(root / "source.bin", "replacement");
        std::array<char, 32> buffer{};
        auto read = source.value().file->read(buffer.data(), buffer.size());
        LAU_REQUIRE(read);
        LAU_REQUIRE(std::string(buffer.data(), read.value()) == "original");
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }

    {
        const auto root = makeTestRoot("libAutoUpdater-rooted-parent-swap");
        const auto outside = root / "outside";
        std::filesystem::create_directories(root / "managed");
        std::filesystem::create_directories(outside);
        auto openedRoot = fileSystem->openRoot(root, autoupdater::RootAccess::ReadWrite, false);
        LAU_REQUIRE(openedRoot);
        auto temporary = openedRoot.value()->createAtomicReplacement(
            "managed/file.bin", autoupdater::RootedDirectoryCreationMode::InstalledContent);
        LAU_REQUIRE(temporary);
        LAU_REQUIRE(temporary.value()->file().write("new", 3));
        LAU_REQUIRE(temporary.value()->file().flush());
        std::error_code swapError;
        std::filesystem::rename(root / "managed", root / "pinned", swapError);
        if (!swapError) {
            createDirectoryLink(root / "managed", outside);
            auto committed = temporary.value()->commit(autoupdater::RootedEntryExpectation::missing());
            LAU_REQUIRE(!committed);
            temporary.value().reset();
            LAU_REQUIRE(!std::filesystem::exists(outside / "file.bin"));
            for (const auto& entry : std::filesystem::directory_iterator(root / "pinned")) {
                LAU_REQUIRE(entry.path().filename().string().find(".autoupdater-tmp-") != 0);
                LAU_REQUIRE(entry.path().filename().string().find(".autoupdater-private-") != 0);
            }
        } else {
#ifdef _WIN32
            // NTFS may deny renaming a directory that contains an opened file;
            // that kernel-enforced refusal is itself a safe outcome.
            LAU_REQUIRE(swapError.value() == ERROR_ACCESS_DENIED || swapError.value() == ERROR_SHARING_VIOLATION);
            temporary.value().reset();
            LAU_REQUIRE(!std::filesystem::exists(outside / "file.bin"));
#else
            LAU_REQUIRE(!swapError);
#endif
        }
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }

    {
        const auto root = makeTestRoot("libAutoUpdater-rooted-leaf-swap");
        writeFile(root / "target.bin", "old");
        auto openedRoot = fileSystem->openRoot(root, autoupdater::RootAccess::ReadWrite, false);
        LAU_REQUIRE(openedRoot);
        auto target = openedRoot.value()->openRegularFile("target.bin", autoupdater::RootedFileOpenMode::ReadOnly);
        LAU_REQUIRE(target && target.value().exists());
        auto metadata = target.value().file->metadata();
        LAU_REQUIRE(metadata);
        target.value().file.reset();
        auto temporary = openedRoot.value()->createAtomicReplacement("target.bin");
        LAU_REQUIRE(temporary);
        LAU_REQUIRE(temporary.value()->file().write("new", 3));
        LAU_REQUIRE(temporary.value()->file().flush());
        std::filesystem::rename(root / "target.bin", root / "old-target.bin");
        writeFile(root / "target.bin", "intruder");
        auto committed = temporary.value()->commit(autoupdater::RootedEntryExpectation::matching(metadata.value()));
        LAU_REQUIRE(!committed);
        LAU_REQUIRE(readFile(root / "target.bin") == "intruder");
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }

    {
        const auto root = makeTestRoot("libAutoUpdater-rooted-readonly");
        writeFile(root / "source.bin", "source");
        auto openedRoot = fileSystem->openRoot(root, autoupdater::RootAccess::ReadOnly, false);
        LAU_REQUIRE(openedRoot);
        auto source = openedRoot.value()->openRegularFile("source.bin", autoupdater::RootedFileOpenMode::ReadOnly);
        LAU_REQUIRE(source && source.value().exists());
        auto replaced = openedRoot.value()->replaceWithOpenedFile(*source.value().file, "target.bin",
                                                                  autoupdater::RootedEntryExpectation::missing());
        LAU_REQUIRE(!replaced);
        LAU_REQUIRE(readFile(root / "source.bin") == "source");
        LAU_REQUIRE(!std::filesystem::exists(root / "target.bin"));
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }

    {
        const auto root = makeTestRoot("libAutoUpdater-rooted-missing-target-race");
        auto openedRoot = fileSystem->openRoot(root, autoupdater::RootAccess::ReadWrite, false);
        LAU_REQUIRE(openedRoot);
        auto temporary = openedRoot.value()->createAtomicReplacement("target.bin");
        LAU_REQUIRE(temporary);
        LAU_REQUIRE(temporary.value()->file().write("verified", 8));
        LAU_REQUIRE(temporary.value()->file().flush());
        writeFile(root / "target.bin", "intruder");
        auto committed = temporary.value()->commit(autoupdater::RootedEntryExpectation::missing());
        LAU_REQUIRE(!committed);
        LAU_REQUIRE(readFile(root / "target.bin") == "intruder");
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }

#ifndef _WIN32
    {
        const auto root = makeTestRoot("libAutoUpdater-rooted-source-name-substitution");
        auto openedRoot = fileSystem->openRoot(root, autoupdater::RootAccess::ReadWrite, false);
        LAU_REQUIRE(openedRoot);
        auto temporary = openedRoot.value()->createAtomicReplacement("target.bin");
        LAU_REQUIRE(temporary);
        LAU_REQUIRE(temporary.value()->file().write("verified", 8));
        LAU_REQUIRE(temporary.value()->file().flush());
        const auto payload = findAtomicTemporaryPayload(root);
        LAU_REQUIRE(!payload.empty());
        const auto movedPayload = payload.parent_path() / "opened-payload";
        std::filesystem::rename(payload, movedPayload);
        writeFile(payload, "malicious");

        auto committed = temporary.value()->commit(autoupdater::RootedEntryExpectation::missing());
        LAU_REQUIRE(!committed);
        LAU_REQUIRE(!std::filesystem::exists(root / "target.bin"));
        temporary.value().reset();
        LAU_REQUIRE(readFile(payload) == "malicious");
        LAU_REQUIRE(readFile(movedPayload) == "verified");
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }

    {
        const auto root = makeTestRoot("libAutoUpdater-rooted-namespace-name-substitution");
        auto openedRoot = fileSystem->openRoot(root, autoupdater::RootAccess::ReadWrite, false);
        LAU_REQUIRE(openedRoot);
        auto temporary = openedRoot.value()->createAtomicReplacement("target.bin");
        LAU_REQUIRE(temporary);
        LAU_REQUIRE(temporary.value()->file().write("verified", 8));
        LAU_REQUIRE(temporary.value()->file().flush());
        const auto payload = findAtomicTemporaryPayload(root);
        LAU_REQUIRE(!payload.empty());
        const auto namespacePath = payload.parent_path();
        const auto movedNamespace = root / "moved-private-namespace";
        std::filesystem::rename(namespacePath, movedNamespace);
        std::filesystem::create_directory(namespacePath);

        auto committed = temporary.value()->commit(autoupdater::RootedEntryExpectation::missing());
        LAU_REQUIRE(committed);
        temporary.value().reset();
        LAU_REQUIRE(readFile(root / "target.bin") == "verified");
        LAU_REQUIRE(std::filesystem::is_directory(namespacePath));
        LAU_REQUIRE(std::filesystem::is_directory(movedNamespace));
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }
#endif

    {
        const auto root = makeTestRoot("libAutoUpdater-rooted-temp-hardlink");
        writeFile(root / "outside-original.bin", "outside");
        std::error_code ec;
        std::filesystem::create_hard_link(root / "outside-original.bin", root / "writable-link.bin", ec);
        LAU_REQUIRE(!ec);
        auto openedRoot = fileSystem->openRoot(root, autoupdater::RootAccess::ReadWrite, false);
        LAU_REQUIRE(openedRoot);
        auto writableLink =
            openedRoot.value()->openRegularFile("writable-link.bin", autoupdater::RootedFileOpenMode::ReadWrite);
        LAU_REQUIRE(!writableLink);
        auto temporary = openedRoot.value()->createAtomicReplacement("target.bin");
        LAU_REQUIRE(temporary);
        LAU_REQUIRE(temporary.value()->file().write("new", 3));
        LAU_REQUIRE(temporary.value()->file().flush());
        const auto temporaryPath = findAtomicTemporaryPayload(root);
        LAU_REQUIRE(!temporaryPath.empty());
        ec.clear();
        std::filesystem::create_hard_link(temporaryPath, root / "outside-link.bin", ec);
        if (!ec) {
            auto committed = temporary.value()->commit(autoupdater::RootedEntryExpectation::missing());
            LAU_REQUIRE(!committed);
            LAU_REQUIRE(!std::filesystem::exists(root / "target.bin"));
            temporary.value().reset();
            LAU_REQUIRE(readFile(root / "outside-link.bin") == "new");
        } else {
#ifdef _WIN32
            // Some Windows filesystems treat the exclusive temp handle as a
            // hard-link sharing conflict. That refusal is already safe.
            temporary.value().reset();
            LAU_REQUIRE(!std::filesystem::exists(root / "target.bin"));
#else
            LAU_REQUIRE(!ec);
#endif
        }
        std::filesystem::remove_all(root, ec);
    }

#ifdef _WIN32
    {
        const auto root = makeTestRoot("libAutoUpdater-rooted-exclusive-temp");
        auto openedRoot = fileSystem->openRoot(root, autoupdater::RootAccess::ReadWrite, false);
        LAU_REQUIRE(openedRoot);
        auto temporary = openedRoot.value()->createAtomicReplacement("target.bin");
        LAU_REQUIRE(temporary);
        LAU_REQUIRE(temporary.value()->file().write("verified", 8));
        LAU_REQUIRE(temporary.value()->file().flush());

        const auto temporaryPath = findAtomicTemporaryPayload(root);
        LAU_REQUIRE(!temporaryPath.empty());
        const HANDLE competingWriter =
            CreateFileW(temporaryPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        const auto competingWriterError = GetLastError();
        if (competingWriter != INVALID_HANDLE_VALUE) {
            CloseHandle(competingWriter);
        }
        LAU_REQUIRE(competingWriter == INVALID_HANDLE_VALUE);
        LAU_REQUIRE(competingWriterError == ERROR_SHARING_VIOLATION);
        const auto movedTemporary = root / "moved-temp.bin";
        LAU_REQUIRE(MoveFileExW(temporaryPath.c_str(), movedTemporary.c_str(), MOVEFILE_REPLACE_EXISTING) == FALSE);
        LAU_REQUIRE(!std::filesystem::exists(movedTemporary));
        LAU_REQUIRE(DeleteFileW(temporaryPath.c_str()) == FALSE);

        auto committed = temporary.value()->commit(autoupdater::RootedEntryExpectation::missing());
        LAU_REQUIRE(committed);
        temporary.value().reset();
        LAU_REQUIRE(readFile(root / "target.bin") == "verified");
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }

    {
        const auto root = makeTestRoot("libAutoUpdater-rooted-hostile-writer");
        writeFile(root / "download.bin", "partial");
        const HANDLE hostileWriter = CreateFileW((root / "download.bin").c_str(), GENERIC_WRITE,
                                                 FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                                 OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        LAU_REQUIRE(hostileWriter != INVALID_HANDLE_VALUE);
        auto openedRoot = fileSystem->openRoot(root, autoupdater::RootAccess::ReadWrite, false);
        LAU_REQUIRE(openedRoot);
        auto writable = openedRoot.value()->openRegularFile("download.bin", autoupdater::RootedFileOpenMode::ReadWrite);
        LAU_REQUIRE(!writable);
        CloseHandle(hostileWriter);
        LAU_REQUIRE(readFile(root / "download.bin") == "partial");
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }

    {
        const auto root = makeTestRoot("libAutoUpdater-rooted-target-delete-race");
        writeFile(root / "target.bin", "old");
        auto openedRoot = fileSystem->openRoot(root, autoupdater::RootAccess::ReadWrite, false);
        LAU_REQUIRE(openedRoot);
        auto target = openedRoot.value()->openRegularFile("target.bin", autoupdater::RootedFileOpenMode::ReadOnly);
        LAU_REQUIRE(target && target.value().exists());
        auto targetMetadata = target.value().file->metadata();
        LAU_REQUIRE(targetMetadata);
        target.value().file.reset();

        auto temporary = openedRoot.value()->createAtomicReplacement("target.bin");
        LAU_REQUIRE(temporary);
        LAU_REQUIRE(temporary.value()->file().write("new", 3));
        LAU_REQUIRE(temporary.value()->file().flush());
        const HANDLE hostileDelete = CreateFileW((root / "target.bin").c_str(), DELETE | FILE_READ_ATTRIBUTES,
                                                 FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                                 OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        LAU_REQUIRE(hostileDelete != INVALID_HANDLE_VALUE);
        auto blocked = temporary.value()->commit(autoupdater::RootedEntryExpectation::matching(targetMetadata.value()));
        LAU_REQUIRE(!blocked);
        CloseHandle(hostileDelete);
        LAU_REQUIRE(readFile(root / "target.bin") == "old");

        auto committed =
            temporary.value()->commit(autoupdater::RootedEntryExpectation::matching(targetMetadata.value()));
        LAU_REQUIRE(committed);
        temporary.value().reset();
        LAU_REQUIRE(readFile(root / "target.bin") == "new");
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }

    {
        const auto root = makeTestRoot("libAutoUpdater-rooted-remove-delete-race");
        writeFile(root / "target.bin", "old");
        auto openedRoot = fileSystem->openRoot(root, autoupdater::RootAccess::ReadWrite, false);
        LAU_REQUIRE(openedRoot);
        auto target = openedRoot.value()->openRegularFile("target.bin", autoupdater::RootedFileOpenMode::ReadOnly);
        LAU_REQUIRE(target && target.value().exists());
        auto targetMetadata = target.value().file->metadata();
        LAU_REQUIRE(targetMetadata);
        target.value().file.reset();

        const HANDLE hostileDelete = CreateFileW((root / "target.bin").c_str(), DELETE | FILE_READ_ATTRIBUTES,
                                                 FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                                 OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        LAU_REQUIRE(hostileDelete != INVALID_HANDLE_VALUE);
        auto blocked = openedRoot.value()->removeRegularFile(
            "target.bin", autoupdater::RootedEntryExpectation::matching(targetMetadata.value()));
        LAU_REQUIRE(!blocked);
        CloseHandle(hostileDelete);
        LAU_REQUIRE(readFile(root / "target.bin") == "old");

        auto removed = openedRoot.value()->removeRegularFile(
            "target.bin", autoupdater::RootedEntryExpectation::matching(targetMetadata.value()));
        LAU_REQUIRE(removed);
        LAU_REQUIRE(!std::filesystem::exists(root / "target.bin"));
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }
#endif

    {
        const auto root = makeTestRoot("libAutoUpdater-rooted-remove-leaf-swap");
        writeFile(root / "managed.bin", "expected");
        auto openedRoot = fileSystem->openRoot(root, autoupdater::RootAccess::ReadWrite, false);
        LAU_REQUIRE(openedRoot);
        auto managed = openedRoot.value()->openRegularFile("managed.bin", autoupdater::RootedFileOpenMode::ReadOnly);
        LAU_REQUIRE(managed && managed.value().exists());
        auto metadata = managed.value().file->metadata();
        LAU_REQUIRE(metadata);
        managed.value().file.reset();
        std::filesystem::rename(root / "managed.bin", root / "expected.bin");
        writeFile(root / "managed.bin", "intruder");
        auto removed = openedRoot.value()->removeRegularFile(
            "managed.bin", autoupdater::RootedEntryExpectation::matching(metadata.value()));
        LAU_REQUIRE(!removed);
        LAU_REQUIRE(readFile(root / "managed.bin") == "intruder");
        LAU_REQUIRE(readFile(root / "expected.bin") == "expected");
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }

    {
        const auto root = makeTestRoot("libAutoUpdater-rooted-remove-hardlink");
        writeFile(root / "outside.bin", "outside");
        std::error_code ec;
        std::filesystem::create_hard_link(root / "outside.bin", root / "managed.bin", ec);
        LAU_REQUIRE(!ec);
        auto openedRoot = fileSystem->openRoot(root, autoupdater::RootAccess::ReadWrite, false);
        LAU_REQUIRE(openedRoot);
        auto managed = openedRoot.value()->openRegularFile("managed.bin", autoupdater::RootedFileOpenMode::ReadOnly);
        LAU_REQUIRE(managed && managed.value().exists());
        auto metadata = managed.value().file->metadata();
        LAU_REQUIRE(metadata);
        managed.value().file.reset();
        auto removed = openedRoot.value()->removeRegularFile(
            "managed.bin", autoupdater::RootedEntryExpectation::matching(metadata.value()));
        LAU_REQUIRE(removed);
        LAU_REQUIRE(!std::filesystem::exists(root / "managed.bin"));
        LAU_REQUIRE(readFile(root / "outside.bin") == "outside");
        std::filesystem::remove_all(root, ec);
    }
}

void testLocalSnapshotUsesOneOpenedFileHandle() {
    auto fileSystem = autoupdater::createDefaultFileSystem();

    {
        const auto root = makeTestRoot("libAutoUpdater-snapshot-link");
        const auto install = root / "install";
        const auto outside = root / "outside";
        writeFile(outside / "file.bin", "outside");
        std::filesystem::create_directories(install);
        createDirectoryLink(install / "managed", outside);
        autoupdater::Config config;
        config.installDir = install;
        autoupdater::Manifest manifest;
        manifest.files.push_back({"managed/file.bin", {}, {}, 0});
        auto hash = autoupdater::createDefaultHashProvider();
        auto snapshot = autoupdater::buildLocalSnapshot(config, manifest, *fileSystem, *hash);
        LAU_REQUIRE(!snapshot);
        LAU_REQUIRE(readFile(outside / "file.bin") == "outside");
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }

    {
        const auto root = makeTestRoot("libAutoUpdater-snapshot-swap");
        const auto install = root / "install";
        const auto path = install / "managed/file.bin";
        const auto moved = install / "managed/original.bin";
        writeFile(path, "original");
        autoupdater::Config config;
        config.installDir = install;
        autoupdater::Manifest manifest;
        manifest.files.push_back({"managed/file.bin", {}, {}, 0});
        SwappingHashProvider hash(path, moved);
        auto expectedHash = hash.sha256Bytes("original");
        LAU_REQUIRE(expectedHash);
        auto snapshot = autoupdater::buildLocalSnapshot(config, manifest, *fileSystem, hash);
        LAU_REQUIRE(snapshot);
        LAU_REQUIRE(snapshot.value().files.size() == 1);
        LAU_REQUIRE(snapshot.value().files.front().sha256 == expectedHash.value());
        LAU_REQUIRE(snapshot.value().files.front().size == 8);
        LAU_REQUIRE(readFile(path) == "replacement");
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }
}

void testDownloadExecutorContainsSwapsAndHardLinks() {
    auto fileSystem = autoupdater::createDefaultFileSystem();
    auto hash = autoupdater::createDefaultHashProvider();

    {
        const auto root = makeTestRoot("libAutoUpdater-download-parent-swap");
        const auto staging = root / "staging";
        const auto outside = root / "outside";
        std::filesystem::create_directories(staging / "managed");
        writeFile(outside / "sentinel.txt", "outside");
        autoupdater::Config config;
        config.tempDir = staging;
        config.retry.maxRetries = 0;
        config.network.enableResume = false;
        auto decision = oneFileDecision("downloaded");
        SwappingDownloadClient network(staging / "managed", staging / "pinned", outside, "downloaded");
        autoupdater::CancellationToken cancel;
        auto result = autoupdater::executeDownloads(config, decision, network, *fileSystem, *hash, nullptr, {}, cancel);
        LAU_REQUIRE(!result);
        LAU_REQUIRE(readFile(outside / "sentinel.txt") == "outside");
        LAU_REQUIRE(!std::filesystem::exists(outside / "file.bin"));
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }

    {
        const auto root = makeTestRoot("libAutoUpdater-download-hardlink");
        const auto staging = root / "staging";
        const auto outside = root / "outside.bin";
        writeFile(outside, "outside-data");
        std::filesystem::create_directories(staging / "managed");
        std::error_code ec;
        std::filesystem::create_hard_link(outside, staging / "managed/file.bin.download", ec);
        LAU_REQUIRE(!ec);
        autoupdater::Config config;
        config.tempDir = staging;
        config.retry.maxRetries = 0;
        config.network.enableResume = false;
        auto decision = oneFileDecision("downloaded");
        StaticDownloadClient network("downloaded");
        autoupdater::CancellationToken cancel;
        auto result = autoupdater::executeDownloads(config, decision, network, *fileSystem, *hash, nullptr, {}, cancel);
        LAU_REQUIRE(!result);
        LAU_REQUIRE(!network.called);
        LAU_REQUIRE(readFile(outside) == "outside-data");
        std::filesystem::remove_all(root, ec);
    }

    {
        const auto root = makeTestRoot("libAutoUpdater-download-hash-mismatch-cleanup");
        const auto staging = root / "staging";
        autoupdater::Config config;
        config.tempDir = staging;
        config.retry.maxRetries = 0;
        config.network.enableResume = false;
        auto decision = oneFileDecision("expected");
        StaticDownloadClient network("tampered");
        autoupdater::CancellationToken cancel;
        auto result = autoupdater::executeDownloads(config, decision, network, *fileSystem, *hash, nullptr, {}, cancel);
        LAU_REQUIRE(!result);
        LAU_REQUIRE(result.error().code == autoupdater::ErrorCode::HashMismatch);
        LAU_REQUIRE(!std::filesystem::exists(staging / "managed/file.bin"));
        LAU_REQUIRE(!std::filesystem::exists(staging / "managed/file.bin.download"));
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }
}

void testApplyExecutorUsesSafePosixPermissions() {
#ifndef _WIN32
    const auto root = makeTestRoot("libAutoUpdater-apply-permissions");
    const auto install = root / "install";
    const auto staging = root / "staging";
    const auto backup = root / "backup";
    writeFile(install / "bin/existing", "old");
    writeFile(install / "bin/readonly", "remove-me");
    writeFile(staging / "bin/existing", "new-existing");
    writeFile(staging / "new/deep/file", "new-file");
    std::filesystem::permissions(install / "bin/existing",
                                 std::filesystem::perms::owner_all | std::filesystem::perms::group_read |
                                     std::filesystem::perms::group_exec | std::filesystem::perms::others_read |
                                     std::filesystem::perms::others_exec,
                                 std::filesystem::perm_options::replace);
    std::filesystem::permissions(install / "bin/readonly",
                                 std::filesystem::perms::owner_read | std::filesystem::perms::group_read |
                                     std::filesystem::perms::others_read,
                                 std::filesystem::perm_options::replace);
    const auto untrustedMode = std::filesystem::perms::owner_all | std::filesystem::perms::group_all |
                               std::filesystem::perms::others_all | std::filesystem::perms::set_uid |
                               std::filesystem::perms::set_gid | std::filesystem::perms::sticky_bit;
    std::filesystem::permissions(staging / "bin/existing", untrustedMode, std::filesystem::perm_options::replace);
    std::filesystem::permissions(staging / "new/deep/file", untrustedMode, std::filesystem::perm_options::replace);

    auto hash = autoupdater::createDefaultHashProvider();
    auto existingHash = hash->sha256Bytes("new-existing");
    auto newHash = hash->sha256Bytes("new-file");
    LAU_REQUIRE(existingHash && newHash);
    autoupdater::ApplyPlan plan;
    plan.installDir = install;
    plan.stagingDir = staging;
    plan.backupDir = backup;
    plan.releaseId = "permissions";
    plan.operations.push_back(
        {autoupdater::ApplyOperationType::Replace, "bin/existing", "bin/existing", existingHash.value(), 12});
    plan.operations.push_back(
        {autoupdater::ApplyOperationType::Replace, "new/deep/file", "new/deep/file", newHash.value(), 8});
    plan.operations.push_back({autoupdater::ApplyOperationType::Remove, {}, "bin/readonly", {}, 0});
    auto result = autoupdater::updater::executeApplyPlan(plan);
    LAU_REQUIRE(result);

    constexpr auto modeMask = std::filesystem::perms::owner_all | std::filesystem::perms::group_all |
                              std::filesystem::perms::others_all | std::filesystem::perms::set_uid |
                              std::filesystem::perms::set_gid | std::filesystem::perms::sticky_bit;
    const auto existingMode = std::filesystem::status(install / "bin/existing").permissions() & modeMask;
    const auto newMode = std::filesystem::status(install / "new/deep/file").permissions() & modeMask;
    const auto installDirectoryMode = std::filesystem::status(install / "new/deep").permissions() & modeMask;
    const auto privateDirectoryMode = std::filesystem::status(backup / "bin").permissions() & modeMask;
    LAU_REQUIRE(existingMode == (std::filesystem::perms::owner_all | std::filesystem::perms::group_read |
                                 std::filesystem::perms::group_exec | std::filesystem::perms::others_read |
                                 std::filesystem::perms::others_exec));
    LAU_REQUIRE(newMode == (std::filesystem::perms::owner_read | std::filesystem::perms::owner_write |
                            std::filesystem::perms::group_read | std::filesystem::perms::others_read));
    LAU_REQUIRE(installDirectoryMode == (std::filesystem::perms::owner_all | std::filesystem::perms::group_read |
                                         std::filesystem::perms::group_exec | std::filesystem::perms::others_read |
                                         std::filesystem::perms::others_exec));
    LAU_REQUIRE(privateDirectoryMode == std::filesystem::perms::owner_all);
    LAU_REQUIRE(!std::filesystem::exists(install / "bin/readonly"));
    LAU_REQUIRE(readFile(backup / "bin/readonly") == "remove-me");

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
#endif
}
