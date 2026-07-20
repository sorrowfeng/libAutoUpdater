#include "ApplyExecutor.h"
#include "DownloadExecutor.h"
#include "DownloadResumeStore.h"
#include "LocalSnapshotBuilder.h"
#include "TestCommon.h"

#include "libAutoUpdater/interfaces/IFileSystem.h"
#include "libAutoUpdater/interfaces/IHashProvider.h"
#include "libAutoUpdater/interfaces/INetworkClient.h"
#include "libAutoUpdater/interfaces/IStateStore.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
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

    autoupdater::Result<autoupdater::TextResponse> getText(const std::string&, const autoupdater::NetworkOptions&,
                                                           std::uint64_t,
                                                           autoupdater::CancellationToken&) noexcept override {
        return autoupdater::Result<autoupdater::TextResponse>::fail(
            {autoupdater::ErrorCode::NetworkError, "Text request is not supported by this test client"});
    }

    autoupdater::Result<autoupdater::DownloadResult>
    downloadToFile(const std::string& url, autoupdater::IRootedFile& target, const autoupdater::NetworkOptions&,
                   std::uint64_t, const std::optional<autoupdater::DownloadResumeInfo>&, autoupdater::ProgressCallback,
                   autoupdater::CancellationToken&) noexcept override {
        called = true;
        ++calls;
        auto written = target.write(contents_.data(), contents_.size());
        if (!written) {
            return autoupdater::Result<autoupdater::DownloadResult>::fail(written.error());
        }
        autoupdater::DownloadResult result;
        result.response.statusCode = 200;
        result.response.effectiveUrl = url;
        result.bytesWritten = contents_.size();
        return autoupdater::Result<autoupdater::DownloadResult>::ok(result);
    }

    bool called = false;
    int calls = 0;

  private:
    std::string contents_;
};

class SwappingDownloadClient final : public autoupdater::INetworkClient {
  public:
    SwappingDownloadClient(std::filesystem::path originalParent, std::filesystem::path movedParent,
                           std::filesystem::path outside, std::string contents)
        : originalParent_(std::move(originalParent)), movedParent_(std::move(movedParent)),
          outside_(std::move(outside)), contents_(std::move(contents)) {}

    autoupdater::Result<autoupdater::TextResponse> getText(const std::string&, const autoupdater::NetworkOptions&,
                                                           std::uint64_t,
                                                           autoupdater::CancellationToken&) noexcept override {
        return autoupdater::Result<autoupdater::TextResponse>::fail(
            {autoupdater::ErrorCode::NetworkError, "Text request is not supported by this test client"});
    }

    autoupdater::Result<autoupdater::DownloadResult>
    downloadToFile(const std::string& url, autoupdater::IRootedFile& target, const autoupdater::NetworkOptions&,
                   std::uint64_t, const std::optional<autoupdater::DownloadResumeInfo>&, autoupdater::ProgressCallback,
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
            result.response.statusCode = 200;
            result.response.effectiveUrl = url;
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

class RecordingStateStore : public autoupdater::IStateStore, public autoupdater::IPendingUpdateCompareAndSet {
  public:
    autoupdater::Result<void> saveLastAcceptedVersion(const autoupdater::Version&,
                                                      const std::string&) noexcept override {
        return autoupdater::Result<void>::ok();
    }

    autoupdater::Result<std::optional<autoupdater::Version>> loadLastAcceptedVersion() noexcept override {
        return autoupdater::Result<std::optional<autoupdater::Version>>::ok(std::nullopt);
    }

    autoupdater::Result<std::string> loadLastAcceptedReleaseId() noexcept override {
        return autoupdater::Result<std::string>::ok({});
    }

    autoupdater::Result<void> commitHealthyVersion(const autoupdater::Version&, const std::string&,
                                                   const std::optional<autoupdater::PendingUpdate>&) noexcept override {
        return autoupdater::Result<void>::ok();
    }

    autoupdater::Result<void> savePendingUpdate(const autoupdater::PendingUpdate&) noexcept override {
        return autoupdater::Result<void>::ok();
    }

    autoupdater::Result<std::optional<autoupdater::PendingUpdate>> loadPendingUpdate() noexcept override {
        return autoupdater::Result<std::optional<autoupdater::PendingUpdate>>::ok(std::nullopt);
    }

    autoupdater::Result<void> clearPendingUpdate() noexcept override {
        return autoupdater::Result<void>::ok();
    }

    autoupdater::Result<void> clearPendingUpdateIfMatches(const autoupdater::PendingUpdate&) noexcept override {
        return autoupdater::Result<void>::ok();
    }

    autoupdater::Result<void> saveDownloadResume(const autoupdater::DownloadResumeState& state) noexcept override {
        if (failSave) {
            return autoupdater::Result<void>::fail(
                {autoupdater::ErrorCode::StateStoreError, "Injected resume save failure"});
        }
        saved.push_back(state);
        return autoupdater::Result<void>::ok();
    }

    autoupdater::Result<std::optional<autoupdater::DownloadResumeState>>
    loadDownloadResume(const std::string& key) noexcept override {
        if (failLoad) {
            return autoupdater::Result<std::optional<autoupdater::DownloadResumeState>>::fail(
                {autoupdater::ErrorCode::StateStoreError, "Injected resume load failure"});
        }
        if (loaded && loaded->key == key) {
            return autoupdater::Result<std::optional<autoupdater::DownloadResumeState>>::ok(loaded);
        }
        return autoupdater::Result<std::optional<autoupdater::DownloadResumeState>>::ok(std::nullopt);
    }

    autoupdater::Result<void> clearDownloadResume(const std::string& key) noexcept override {
        if (failClear) {
            return autoupdater::Result<void>::fail(
                {autoupdater::ErrorCode::StateStoreError, "Injected resume clear failure"});
        }
        cleared.push_back(key);
        return autoupdater::Result<void>::ok();
    }

    std::optional<autoupdater::DownloadResumeState> loaded;
    std::vector<autoupdater::DownloadResumeState> saved;
    std::vector<std::string> cleared;
    bool failSave = false;
    bool failLoad = false;
    bool failClear = false;
};

class BatchRecordingStateStore final : public RecordingStateStore,
                                       public autoupdater::detail::IDownloadResumeBatchStore {
  public:
    autoupdater::Result<std::vector<autoupdater::DownloadResumeState>>
    loadDownloadResumeBatch(const autoupdater::detail::DownloadResumeScope& scope,
                            const std::vector<std::string>& keys) noexcept override {
        ++batchLoadCalls;
        loadedScope = scope;
        requestedKeys = keys;
        if (failBatchLoad) {
            return autoupdater::Result<std::vector<autoupdater::DownloadResumeState>>::fail(
                {autoupdater::ErrorCode::StateStoreError, "Injected resume batch load failure"});
        }
        return autoupdater::Result<std::vector<autoupdater::DownloadResumeState>>::ok(batchLoaded);
    }

    autoupdater::Result<void> applyDownloadResumeBatch(const autoupdater::detail::DownloadResumeScope& scope,
                                                       const std::vector<autoupdater::DownloadResumeState>& upserts,
                                                       const std::vector<std::string>& clears) noexcept override {
        ++batchApplyCalls;
        appliedScope = scope;
        appliedUpserts = upserts;
        appliedClears = clears;
        if (failBatchApply) {
            return autoupdater::Result<void>::fail(
                {autoupdater::ErrorCode::StateStoreError, "Injected resume batch apply failure"});
        }
        return autoupdater::Result<void>::ok();
    }

    int batchLoadCalls = 0;
    int batchApplyCalls = 0;
    bool failBatchLoad = false;
    bool failBatchApply = false;
    autoupdater::detail::DownloadResumeScope loadedScope;
    autoupdater::detail::DownloadResumeScope appliedScope;
    std::vector<std::string> requestedKeys;
    std::vector<autoupdater::DownloadResumeState> batchLoaded;
    std::vector<autoupdater::DownloadResumeState> appliedUpserts;
    std::vector<std::string> appliedClears;
};

class RedirectingValidatorDownloadClient final : public autoupdater::INetworkClient {
  public:
    RedirectingValidatorDownloadClient(std::string initialUrl, std::string finalUrl, std::string contents)
        : initialUrl_(std::move(initialUrl)), finalUrl_(std::move(finalUrl)), contents_(std::move(contents)) {}

    autoupdater::Result<autoupdater::TextResponse> getText(const std::string&, const autoupdater::NetworkOptions&,
                                                           std::uint64_t,
                                                           autoupdater::CancellationToken&) noexcept override {
        return autoupdater::Result<autoupdater::TextResponse>::fail(
            {autoupdater::ErrorCode::NetworkError, "Text request is not supported by this test client"});
    }

    autoupdater::Result<autoupdater::DownloadResult>
    downloadToFile(const std::string& url, autoupdater::IRootedFile& target, const autoupdater::NetworkOptions&,
                   std::uint64_t, const std::optional<autoupdater::DownloadResumeInfo>& resume,
                   autoupdater::ProgressCallback, autoupdater::CancellationToken&) noexcept override {
        requests.push_back(url);
        resumes.push_back(resume);
        autoupdater::DownloadResult result;
        result.response.effectiveUrl = url;
        if (url == initialUrl_) {
            result.response.statusCode = 302;
            result.response.headers.push_back({"Location", finalUrl_});
            return autoupdater::Result<autoupdater::DownloadResult>::ok(std::move(result));
        }
        if (url != finalUrl_) {
            return autoupdater::Result<autoupdater::DownloadResult>::fail(
                {autoupdater::ErrorCode::NetworkError, "Unexpected download URL"});
        }

        auto written = target.write(contents_.data(), contents_.size());
        if (!written) {
            return autoupdater::Result<autoupdater::DownloadResult>::fail(written.error());
        }
        result.response.statusCode = 200;
        result.etag = "final-etag";
        result.lastModified = "Wed, 01 Jul 2026 12:00:00 GMT";
        result.bytesWritten = contents_.size();
        return autoupdater::Result<autoupdater::DownloadResult>::ok(std::move(result));
    }

    std::vector<std::string> requests;
    std::vector<std::optional<autoupdater::DownloadResumeInfo>> resumes;

  private:
    std::string initialUrl_;
    std::string finalUrl_;
    std::string contents_;
};

class FailingPartialDownloadClient final : public autoupdater::INetworkClient {
  public:
    explicit FailingPartialDownloadClient(std::string partial) : partial_(std::move(partial)) {}

    autoupdater::Result<autoupdater::TextResponse> getText(const std::string&, const autoupdater::NetworkOptions&,
                                                           std::uint64_t,
                                                           autoupdater::CancellationToken&) noexcept override {
        return autoupdater::Result<autoupdater::TextResponse>::fail(
            {autoupdater::ErrorCode::NetworkError, "Text request is not supported by this test client"});
    }

    autoupdater::Result<autoupdater::DownloadResult>
    downloadToFile(const std::string&, autoupdater::IRootedFile& target, const autoupdater::NetworkOptions&,
                   std::uint64_t, const std::optional<autoupdater::DownloadResumeInfo>& resume,
                   autoupdater::ProgressCallback, autoupdater::CancellationToken&) noexcept override {
        resumes.push_back(resume);
        auto written = target.write(partial_.data(), partial_.size());
        if (!written) {
            return autoupdater::Result<autoupdater::DownloadResult>::fail(written.error());
        }
        return autoupdater::Result<autoupdater::DownloadResult>::fail(
            {autoupdater::ErrorCode::NetworkError, "Injected transport failure"});
    }

    std::vector<std::optional<autoupdater::DownloadResumeInfo>> resumes;

  private:
    std::string partial_;
};

class FailingStreamHashProvider final : public autoupdater::IHashProvider {
  public:
    FailingStreamHashProvider() : delegate_(autoupdater::createDefaultHashProvider()) {}

    autoupdater::Result<std::string> sha256File(const std::filesystem::path& path) noexcept override {
        return delegate_->sha256File(path);
    }

    autoupdater::Result<std::string> sha256Bytes(std::string_view data) noexcept override {
        return delegate_->sha256Bytes(data);
    }

    autoupdater::Result<std::string> sha256Stream(autoupdater::IRootedFile&) noexcept override {
        ++streamCalls;
        return autoupdater::Result<std::string>::fail(
            {autoupdater::ErrorCode::FileSystemError, "Injected rooted hash read failure"});
    }

    int streamCalls = 0;

  private:
    std::shared_ptr<autoupdater::IHashProvider> delegate_;
};

autoupdater::Result<void> copyForPostPublishFault(autoupdater::IRootedFile& source,
                                                  autoupdater::IRootedFile& destination) {
    auto rewound = source.seek(0);
    if (!rewound) {
        return rewound;
    }
    std::array<unsigned char, 64 * 1024> buffer{};
    for (;;) {
        auto read = source.read(buffer.data(), buffer.size());
        if (!read) {
            return autoupdater::Result<void>::fail(read.error());
        }
        if (read.value() == 0) {
            return autoupdater::Result<void>::ok();
        }
        auto written = destination.write(buffer.data(), read.value());
        if (!written) {
            return written;
        }
    }
}

struct PostPublishFaultState {
    int replaceCalls = 0;
    bool failReconciliation = false;
};

class PostPublishFaultDirectory final : public autoupdater::IRootedDirectory {
  public:
    PostPublishFaultDirectory(std::unique_ptr<autoupdater::IRootedDirectory> inner,
                              std::shared_ptr<PostPublishFaultState> state)
        : inner_(std::move(inner)), state_(std::move(state)) {}

    autoupdater::Result<autoupdater::RootedOpenResult>
    openRegularFile(const std::string& relativePath, autoupdater::RootedFileOpenMode mode,
                    autoupdater::RootedDirectoryCreationMode directoryMode) noexcept override {
        return inner_->openRegularFile(relativePath, mode, directoryMode);
    }

    autoupdater::Result<std::unique_ptr<autoupdater::IRootedTemporaryFile>>
    createAtomicReplacement(const std::string& relativePath,
                            autoupdater::RootedDirectoryCreationMode directoryMode) noexcept override {
        return inner_->createAtomicReplacement(relativePath, directoryMode);
    }

    autoupdater::Result<void>
    replaceWithOpenedFile(autoupdater::IRootedFile& source, const std::string& relativePath,
                          const autoupdater::RootedEntryExpectation& expectation) noexcept override {
        ++state_->replaceCalls;
        if (state_->replaceCalls == 1) {
            auto sourceMetadata = source.metadata();
            if (!sourceMetadata) {
                return autoupdater::Result<void>::fail(sourceMetadata.error());
            }
            auto temporary =
                inner_->createAtomicReplacement(relativePath, autoupdater::RootedDirectoryCreationMode::Private);
            if (!temporary) {
                return autoupdater::Result<void>::fail(temporary.error());
            }
            auto copied = copyForPostPublishFault(source, temporary.value()->file());
            if (!copied) {
                (void)temporary.value()->discard();
                return copied;
            }
            if (sourceMetadata.value().permissions != std::filesystem::perms::unknown) {
                auto permissions = temporary.value()->file().setPermissions(sourceMetadata.value().permissions);
                if (!permissions) {
                    (void)temporary.value()->discard();
                    return permissions;
                }
            }
            auto committed = temporary.value()->commit(expectation);
            auto discarded = temporary.value()->discard();
            if (!committed) {
                return committed;
            }
            if (!discarded) {
                return discarded;
            }
            return autoupdater::Result<void>::fail(
                {autoupdater::ErrorCode::FileSystemError, "Injected post-publish replacement failure"});
        }
        if (state_->failReconciliation) {
            return autoupdater::Result<void>::fail(
                {autoupdater::ErrorCode::FileSystemError, "Injected reconciliation replacement failure"});
        }
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
    std::shared_ptr<PostPublishFaultState> state_;
};

class PostPublishFaultFileSystem final : public autoupdater::IFileSystem {
  public:
    PostPublishFaultFileSystem(std::shared_ptr<autoupdater::IFileSystem> inner,
                               std::shared_ptr<PostPublishFaultState> state)
        : inner_(std::move(inner)), state_(std::move(state)) {}

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
            std::make_unique<PostPublishFaultDirectory>(std::move(opened.value()), state_);
        return autoupdater::Result<std::unique_ptr<autoupdater::IRootedDirectory>>::ok(std::move(wrapped));
    }

  private:
    std::shared_ptr<autoupdater::IFileSystem> inner_;
    std::shared_ptr<PostPublishFaultState> state_;
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
    download.url = "https://updates.example.test/artifact";
    decision.downloads.push_back(std::move(download));
    return decision;
}

std::string resumeKeyFor(const autoupdater::Config& config, const autoupdater::UpdateDecision& decision,
                         std::size_t downloadIndex = 0) {
    auto releaseKey = autoupdater::detail::downloadResumeReleaseKey(config, decision);
    LAU_REQUIRE(releaseKey);
    LAU_REQUIRE(downloadIndex < decision.downloads.size());
    auto resourceKey =
        autoupdater::detail::downloadResumeResourceKey(releaseKey.value(), decision.downloads[downloadIndex]);
    LAU_REQUIRE(resourceKey);
    return resourceKey.value();
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
        const auto root = makeTestRoot("libAutoUpdater-rooted-explicit-discard");
        auto openedRoot = fileSystem->openRoot(root, autoupdater::RootAccess::ReadWrite, false);
        LAU_REQUIRE(openedRoot);

        auto abandoned = openedRoot.value()->createAtomicReplacement("abandoned.bin");
        LAU_REQUIRE(abandoned);
        LAU_REQUIRE(abandoned.value()->file().write("discarded", 9));
        LAU_REQUIRE(abandoned.value()->publishStatus().publication == autoupdater::RootedPublication::NotPublished);
        LAU_REQUIRE(abandoned.value()->discard());
        LAU_REQUIRE(abandoned.value()->discard());
        LAU_REQUIRE(!std::filesystem::exists(root / "abandoned.bin"));

        auto published = openedRoot.value()->createAtomicReplacement("published.bin");
        LAU_REQUIRE(published);
        LAU_REQUIRE(published.value()->file().write("published", 9));
        LAU_REQUIRE(published.value()->commit(autoupdater::RootedEntryExpectation::missing()));
        const auto status = published.value()->publishStatus();
        LAU_REQUIRE(status.publication == autoupdater::RootedPublication::Published);
        LAU_REQUIRE(status.namespaceDurable);
        LAU_REQUIRE(published.value()->discard());
        LAU_REQUIRE(published.value()->discard());
        LAU_REQUIRE(readFile(root / "published.bin") == "published");
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
        config.manifestUrl = "https://updates.example.test/manifest.json";
        config.tempDir = staging;
        config.retry.maxRetries = 0;
        config.network.enableResume = false;
        config.security.allowedBaseUrls = {"https://updates.example.test/"};
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
        config.manifestUrl = "https://updates.example.test/manifest.json";
        config.tempDir = staging;
        config.retry.maxRetries = 0;
        config.network.enableResume = false;
        config.security.allowedBaseUrls = {"https://updates.example.test/"};
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
        config.manifestUrl = "https://updates.example.test/manifest.json";
        config.tempDir = staging;
        config.retry.maxRetries = 0;
        config.network.enableResume = false;
        config.security.allowedBaseUrls = {"https://updates.example.test/"};
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

void testDownloadExecutorKeepsValidatorsBoundToTheirResource() {
    auto fileSystem = autoupdater::createDefaultFileSystem();
    auto hash = autoupdater::createDefaultHashProvider();
    const std::string initialUrl = "https://updates.example.test/artifacts/file.bin";
    const std::string finalUrl = "https://cdn.example.test/releases/file.bin";

    {
        const auto root = makeTestRoot("libAutoUpdater-download-redirect-validator-binding");
        const auto staging = root / "staging";
        const std::string contents = "downloaded";
        autoupdater::Config config;
        config.manifestUrl = "https://updates.example.test/manifest.json";
        config.tempDir = staging;
        config.retry.maxRetries = 0;
        config.security.allowedBaseUrls = {"https://updates.example.test/", "https://cdn.example.test/"};
        auto decision = oneFileDecision(contents);
        decision.downloads[0].url = initialUrl;
        const auto resumeKey = resumeKeyFor(config, decision);
        RedirectingValidatorDownloadClient network(initialUrl, finalUrl, contents);
        RecordingStateStore stateStore;
        autoupdater::CancellationToken cancel;

        auto result =
            autoupdater::executeDownloads(config, decision, network, *fileSystem, *hash, &stateStore, {}, cancel);
        LAU_REQUIRE(result);
        LAU_REQUIRE(network.requests == std::vector<std::string>({initialUrl, finalUrl}));
        LAU_REQUIRE(network.resumes.size() == 2);
        LAU_REQUIRE(!network.resumes[0]);
        LAU_REQUIRE(!network.resumes[1]);
        LAU_REQUIRE(stateStore.saved.empty());
        LAU_REQUIRE(stateStore.cleared == std::vector<std::string>({resumeKey}));
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }

    {
        const auto root = makeTestRoot("libAutoUpdater-download-failed-validator-reset");
        const auto staging = root / "staging";
        writeFile(staging / "managed/file.bin.download", "partial");
        autoupdater::Config config;
        config.manifestUrl = "https://updates.example.test/manifest.json";
        config.tempDir = staging;
        config.retry.maxRetries = 0;
        config.security.allowedBaseUrls = {"https://updates.example.test/"};
        auto decision = oneFileDecision("complete-payload");
        decision.downloads[0].url = initialUrl;
        const auto resumeKey = resumeKeyFor(config, decision);
        RecordingStateStore stateStore;
        stateStore.loaded = autoupdater::DownloadResumeState{resumeKey, 7, "old-etag", "Tue, 30 Jun 2026 12:00:00 GMT",
                                                             decision.downloads[0].file.sha256};
        FailingPartialDownloadClient network("-tail");
        autoupdater::CancellationToken cancel;

        auto result =
            autoupdater::executeDownloads(config, decision, network, *fileSystem, *hash, &stateStore, {}, cancel);
        LAU_REQUIRE(!result);
        LAU_REQUIRE(result.error().code == autoupdater::ErrorCode::NetworkError);
        LAU_REQUIRE(network.resumes.size() == 1);
        LAU_REQUIRE(network.resumes[0]);
        LAU_REQUIRE(network.resumes[0]->etag == "old-etag");
        LAU_REQUIRE(network.resumes[0]->lastModified == "Tue, 30 Jun 2026 12:00:00 GMT");
        LAU_REQUIRE(stateStore.saved.size() == 1);
        LAU_REQUIRE(stateStore.saved[0].key == resumeKey);
        LAU_REQUIRE(stateStore.saved[0].offset == 12);
        LAU_REQUIRE(stateStore.saved[0].etag.empty());
        LAU_REQUIRE(stateStore.saved[0].lastModified.empty());
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }
}

void testDownloadResumeUsesOpaqueStableResourceKeys() {
    autoupdater::Config config;
    config.appId = "resume-key-test";
    config.channel = "stable";
    config.platform = "windows";
    config.arch = "x64";

    auto decision = oneFileDecision("complete-payload");
    decision.checkResult.remoteVersion = autoupdater::Version(2, 1, 0);
    decision.checkResult.releaseId = "release-21";
    decision.downloads[0].url = "https://cdn.example.test/releases/file.bin?token=secret-a&expires=1";
    const auto first = resumeKeyFor(config, decision);

    auto rotatedCredentials = decision;
    rotatedCredentials.downloads[0].url = "https://cdn.example.test/releases/file.bin?token=secret-b&expires=2";
    const auto rotated = resumeKeyFor(config, rotatedCredentials);
    LAU_REQUIRE(first == rotated);
    LAU_REQUIRE(first.size() == 64);
    LAU_REQUIRE(first.find_first_not_of("0123456789abcdef") == std::string::npos);
    LAU_REQUIRE(first.find("secret") == std::string::npos);
    LAU_REQUIRE(first.find("https") == std::string::npos);

    auto differentPath = decision;
    differentPath.downloads[0].url = "https://cdn.example.test/releases/other.bin?token=secret-a";
    LAU_REQUIRE(resumeKeyFor(config, differentPath) != first);

    auto differentContent = oneFileDecision("different-payload");
    differentContent.checkResult = decision.checkResult;
    differentContent.downloads[0].url = decision.downloads[0].url;
    LAU_REQUIRE(resumeKeyFor(config, differentContent) != first);

    auto differentRelease = decision;
    differentRelease.checkResult.releaseId = "release-22";
    LAU_REQUIRE(resumeKeyFor(config, differentRelease) != first);
}

void testDownloadExecutorBatchesResumePersistence() {
    auto fileSystem = autoupdater::createDefaultFileSystem();
    auto hash = autoupdater::createDefaultHashProvider();
    const auto configFor = [](const std::filesystem::path& staging) {
        autoupdater::Config config;
        config.appId = "resume-batch-test";
        config.platform = "windows";
        config.arch = "x64";
        config.manifestUrl = "https://updates.example.test/manifest.json";
        config.tempDir = staging;
        config.retry.maxRetries = 0;
        config.security.allowedBaseUrls = {"https://updates.example.test/"};
        return config;
    };

    {
        const auto root = makeTestRoot("libAutoUpdater-download-resume-batch-success");
        auto config = configFor(root / "staging");
        const std::string contents = "complete-payload";
        auto decision = oneFileDecision(contents);
        decision.checkResult.remoteVersion = autoupdater::Version(3, 0, 0);
        decision.checkResult.releaseId = "release-30";
        auto second = decision.downloads.front();
        second.file.path = "managed/second.bin";
        second.url = "https://updates.example.test/second";
        decision.downloads.push_back(std::move(second));
        std::vector<std::string> expectedKeys = {resumeKeyFor(config, decision, 0), resumeKeyFor(config, decision, 1)};

        BatchRecordingStateStore stateStore;
        StaticDownloadClient network(contents);
        autoupdater::CancellationToken cancel;
        const auto result =
            autoupdater::executeDownloads(config, decision, network, *fileSystem, *hash, &stateStore, {}, cancel);

        LAU_REQUIRE(result);
        LAU_REQUIRE(network.calls == 2);
        LAU_REQUIRE(stateStore.batchLoadCalls == 1);
        LAU_REQUIRE(stateStore.batchApplyCalls == 1);
        LAU_REQUIRE(stateStore.requestedKeys == expectedKeys);
        LAU_REQUIRE(stateStore.appliedUpserts.empty());
        std::sort(expectedKeys.begin(), expectedKeys.end());
        LAU_REQUIRE(stateStore.appliedClears == expectedKeys);
        LAU_REQUIRE(stateStore.saved.empty());
        LAU_REQUIRE(stateStore.cleared.empty());
        LAU_REQUIRE(stateStore.loadedScope.releaseKey == stateStore.appliedScope.releaseKey);
        LAU_REQUIRE(stateStore.loadedScope.nowUnixSeconds > 0);
        LAU_REQUIRE(stateStore.loadedScope.maxAgeSeconds == 7 * 24 * 60 * 60);
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }

    {
        const auto root = makeTestRoot("libAutoUpdater-download-resume-batch-failure");
        auto config = configFor(root / "staging");
        auto decision = oneFileDecision("complete-payload");
        decision.checkResult.remoteVersion = autoupdater::Version(3, 0, 1);
        decision.checkResult.releaseId = "release-301";
        const auto expectedKey = resumeKeyFor(config, decision);

        BatchRecordingStateStore stateStore;
        FailingPartialDownloadClient network("partial");
        autoupdater::CancellationToken cancel;
        const auto result =
            autoupdater::executeDownloads(config, decision, network, *fileSystem, *hash, &stateStore, {}, cancel);

        LAU_REQUIRE(!result);
        LAU_REQUIRE(result.error().code == autoupdater::ErrorCode::NetworkError);
        LAU_REQUIRE(stateStore.batchLoadCalls == 1);
        LAU_REQUIRE(stateStore.batchApplyCalls == 1);
        LAU_REQUIRE(stateStore.appliedClears.empty());
        LAU_REQUIRE(stateStore.appliedUpserts.size() == 1);
        LAU_REQUIRE(stateStore.appliedUpserts[0].key == expectedKey);
        LAU_REQUIRE(stateStore.appliedUpserts[0].offset == 7);
        LAU_REQUIRE(stateStore.appliedUpserts[0].sha256 == decision.downloads[0].file.sha256);
        LAU_REQUIRE(stateStore.saved.empty());
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }

    {
        const auto root = makeTestRoot("libAutoUpdater-download-resume-batch-apply-failure");
        auto config = configFor(root / "staging");
        const std::string contents = "complete-payload";
        auto decision = oneFileDecision(contents);
        BatchRecordingStateStore stateStore;
        stateStore.failBatchApply = true;
        StaticDownloadClient network(contents);
        autoupdater::CancellationToken cancel;
        const auto result =
            autoupdater::executeDownloads(config, decision, network, *fileSystem, *hash, &stateStore, {}, cancel);

        LAU_REQUIRE(!result);
        LAU_REQUIRE(result.error().code == autoupdater::ErrorCode::StateStoreError);
        LAU_REQUIRE(stateStore.batchLoadCalls == 1);
        LAU_REQUIRE(stateStore.batchApplyCalls == 1);
        LAU_REQUIRE(readFile(config.tempDir / "managed/file.bin") == contents);
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }

    {
        const auto root = makeTestRoot("libAutoUpdater-download-resume-batch-load-failure");
        auto config = configFor(root / "staging");
        const std::string contents = "complete-payload";
        auto decision = oneFileDecision(contents);
        BatchRecordingStateStore stateStore;
        stateStore.failBatchLoad = true;
        StaticDownloadClient network(contents);
        autoupdater::CancellationToken cancel;
        const auto result =
            autoupdater::executeDownloads(config, decision, network, *fileSystem, *hash, &stateStore, {}, cancel);

        LAU_REQUIRE(!result);
        LAU_REQUIRE(result.error().code == autoupdater::ErrorCode::StateStoreError);
        LAU_REQUIRE(stateStore.batchLoadCalls == 1);
        LAU_REQUIRE(stateStore.batchApplyCalls == 0);
        LAU_REQUIRE(!network.called);
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }
}

void testDownloadExecutorPropagatesResumePersistenceFailures() {
    auto fileSystem = autoupdater::createDefaultFileSystem();
    auto hash = autoupdater::createDefaultHashProvider();

    const auto configFor = [](const std::filesystem::path& staging) {
        autoupdater::Config config;
        config.manifestUrl = "https://updates.example.test/manifest.json";
        config.tempDir = staging;
        config.retry.maxRetries = 0;
        config.security.allowedBaseUrls = {"https://updates.example.test/"};
        return config;
    };

    {
        const auto root = makeTestRoot("libAutoUpdater-download-resume-load-failure");
        const auto staging = root / "staging";
        writeFile(staging / "managed/file.bin.download", "partial");
        auto config = configFor(staging);
        auto decision = oneFileDecision("partial-complete");
        RecordingStateStore stateStore;
        stateStore.failLoad = true;
        StaticDownloadClient network("unused");
        autoupdater::CancellationToken cancel;

        const auto result =
            autoupdater::executeDownloads(config, decision, network, *fileSystem, *hash, &stateStore, {}, cancel);

        LAU_REQUIRE(!result);
        LAU_REQUIRE(result.error().code == autoupdater::ErrorCode::StateStoreError);
        LAU_REQUIRE(!network.called);
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }

    {
        const auto root = makeTestRoot("libAutoUpdater-download-resume-save-failure");
        const auto staging = root / "staging";
        auto config = configFor(staging);
        auto decision = oneFileDecision("complete-payload");
        RecordingStateStore stateStore;
        stateStore.failSave = true;
        FailingPartialDownloadClient network("partial");
        autoupdater::CancellationToken cancel;

        const auto result =
            autoupdater::executeDownloads(config, decision, network, *fileSystem, *hash, &stateStore, {}, cancel);

        LAU_REQUIRE(!result);
        LAU_REQUIRE(result.error().code == autoupdater::ErrorCode::NetworkError);
        LAU_REQUIRE(result.error().message.find("failed to save download resume state") != std::string::npos);
        LAU_REQUIRE(result.error().message.find("StateStoreError") != std::string::npos);
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }

    {
        const auto root = makeTestRoot("libAutoUpdater-download-resume-clear-failure");
        const auto staging = root / "staging";
        const std::string contents = "complete-payload";
        auto config = configFor(staging);
        auto decision = oneFileDecision(contents);
        const auto resumeKey = resumeKeyFor(config, decision);
        RecordingStateStore stateStore;
        stateStore.failClear = true;
        StaticDownloadClient network(contents);
        autoupdater::CancellationToken cancel;

        const auto failedClear =
            autoupdater::executeDownloads(config, decision, network, *fileSystem, *hash, &stateStore, {}, cancel);

        LAU_REQUIRE(!failedClear);
        LAU_REQUIRE(failedClear.error().code == autoupdater::ErrorCode::StateStoreError);
        LAU_REQUIRE(readFile(staging / "managed/file.bin") == contents);
        LAU_REQUIRE(stateStore.saved.empty());

        stateStore.failClear = false;
        StaticDownloadClient mustNotRun("unused");
        const auto reconciled =
            autoupdater::executeDownloads(config, decision, mustNotRun, *fileSystem, *hash, &stateStore, {}, cancel);
        LAU_REQUIRE(reconciled);
        LAU_REQUIRE(!mustNotRun.called);
        LAU_REQUIRE(stateStore.cleared == std::vector<std::string>({resumeKey}));
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }
}

void testDownloadExecutorReportsHashAndPublicationFailures() {
    const auto configFor = [](const std::filesystem::path& staging, int retries = 0) {
        autoupdater::Config config;
        config.manifestUrl = "https://updates.example.test/manifest.json";
        config.tempDir = staging;
        config.retry.maxRetries = retries;
        config.security.allowedBaseUrls = {"https://updates.example.test/"};
        return config;
    };

    {
        const auto root = makeTestRoot("libAutoUpdater-download-hash-read-failure");
        const auto staging = root / "staging";
        auto config = configFor(staging, 2);
        const std::string contents = "complete-payload";
        auto decision = oneFileDecision(contents);
        const auto resumeKey = resumeKeyFor(config, decision);
        auto fileSystem = autoupdater::createDefaultFileSystem();
        FailingStreamHashProvider hash;
        StaticDownloadClient network(contents);
        RecordingStateStore stateStore;
        autoupdater::CancellationToken cancel;

        const auto result =
            autoupdater::executeDownloads(config, decision, network, *fileSystem, hash, &stateStore, {}, cancel);

        LAU_REQUIRE(!result);
        LAU_REQUIRE(result.error().code == autoupdater::ErrorCode::FileSystemError);
        LAU_REQUIRE(result.error().message.find("Injected rooted hash read failure") != std::string::npos);
        LAU_REQUIRE(hash.streamCalls == 1);
        LAU_REQUIRE(network.calls == 1);
        LAU_REQUIRE(!std::filesystem::exists(staging / "managed/file.bin"));
        LAU_REQUIRE(!std::filesystem::exists(staging / "managed/file.bin.download"));
        LAU_REQUIRE(stateStore.cleared == std::vector<std::string>({resumeKey}));
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }

    {
        const auto root = makeTestRoot("libAutoUpdater-download-size-mismatch-retry");
        const auto staging = root / "staging";
        auto config = configFor(staging, 1);
        auto decision = oneFileDecision("expected-longer-payload");
        const auto resumeKey = resumeKeyFor(config, decision);
        auto fileSystem = autoupdater::createDefaultFileSystem();
        auto hash = autoupdater::createDefaultHashProvider();
        StaticDownloadClient network("short");
        RecordingStateStore stateStore;
        autoupdater::CancellationToken cancel;

        const auto result =
            autoupdater::executeDownloads(config, decision, network, *fileSystem, *hash, &stateStore, {}, cancel);

        LAU_REQUIRE(!result);
        LAU_REQUIRE(result.error().code == autoupdater::ErrorCode::HashMismatch);
        LAU_REQUIRE(network.calls == 2);
        LAU_REQUIRE(!std::filesystem::exists(staging / "managed/file.bin"));
        LAU_REQUIRE(!std::filesystem::exists(staging / "managed/file.bin.download"));
        LAU_REQUIRE(stateStore.cleared == std::vector<std::string>({resumeKey, resumeKey}));
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }

    {
        const auto root = makeTestRoot("libAutoUpdater-download-post-publish-reconcile");
        const auto staging = root / "staging";
        auto config = configFor(staging, 2);
        const std::string contents = "complete-payload";
        auto decision = oneFileDecision(contents);
        const auto resumeKey = resumeKeyFor(config, decision);
        auto hash = autoupdater::createDefaultHashProvider();
        auto fault = std::make_shared<PostPublishFaultState>();
        PostPublishFaultFileSystem fileSystem(autoupdater::createDefaultFileSystem(), fault);
        StaticDownloadClient network(contents);
        RecordingStateStore stateStore;
        autoupdater::CancellationToken cancel;

        const auto result =
            autoupdater::executeDownloads(config, decision, network, fileSystem, *hash, &stateStore, {}, cancel);

        LAU_REQUIRE(result);
        LAU_REQUIRE(network.calls == 1);
        LAU_REQUIRE(fault->replaceCalls == 2);
        LAU_REQUIRE(readFile(staging / "managed/file.bin") == contents);
        LAU_REQUIRE(!std::filesystem::exists(staging / "managed/file.bin.download"));
        LAU_REQUIRE(stateStore.cleared == std::vector<std::string>({resumeKey}));
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }

    {
        const auto root = makeTestRoot("libAutoUpdater-download-post-publish-reconcile-failure");
        const auto staging = root / "staging";
        auto config = configFor(staging, 2);
        const std::string contents = "complete-payload";
        auto decision = oneFileDecision(contents);
        const auto resumeKey = resumeKeyFor(config, decision);
        auto hash = autoupdater::createDefaultHashProvider();
        auto fault = std::make_shared<PostPublishFaultState>();
        fault->failReconciliation = true;
        PostPublishFaultFileSystem fileSystem(autoupdater::createDefaultFileSystem(), fault);
        StaticDownloadClient network(contents);
        RecordingStateStore stateStore;
        autoupdater::CancellationToken cancel;

        const auto result =
            autoupdater::executeDownloads(config, decision, network, fileSystem, *hash, &stateStore, {}, cancel);

        LAU_REQUIRE(!result);
        LAU_REQUIRE(result.error().code == autoupdater::ErrorCode::FileSystemError);
        LAU_REQUIRE(result.error().message.find("Injected post-publish replacement failure") != std::string::npos);
        LAU_REQUIRE(result.error().message.find("failed to durably reconcile") != std::string::npos);
        LAU_REQUIRE(result.error().message.find("Injected reconciliation replacement failure") != std::string::npos);
        LAU_REQUIRE(network.calls == 1);
        LAU_REQUIRE(fault->replaceCalls == 2);
        LAU_REQUIRE(readFile(staging / "managed/file.bin") == contents);
        LAU_REQUIRE(!std::filesystem::exists(staging / "managed/file.bin.download"));
        LAU_REQUIRE(stateStore.cleared == std::vector<std::string>({resumeKey}));
        std::error_code error;
        std::filesystem::remove_all(root, error);
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

    const auto rollbackInstall = root / "rollback-install";
    const auto rollbackStaging = root / "rollback-staging";
    const auto rollbackBackup = root / "rollback-backup";
    writeFile(rollbackInstall / "bin/app", "rollback-old");
    writeFile(rollbackStaging / "bin/app", "rollback-new");
    const auto rollbackMode =
        std::filesystem::perms::owner_all | std::filesystem::perms::group_read | std::filesystem::perms::group_exec;
    std::filesystem::permissions(rollbackInstall / "bin/app", rollbackMode, std::filesystem::perm_options::replace);
    auto rollbackHash = hash->sha256Bytes("rollback-new");
    LAU_REQUIRE(rollbackHash);
    autoupdater::ApplyPlan rollbackPlan;
    rollbackPlan.installDir = rollbackInstall;
    rollbackPlan.stagingDir = rollbackStaging;
    rollbackPlan.backupDir = rollbackBackup;
    rollbackPlan.releaseId = "permission-rollback";
    rollbackPlan.operations.push_back(
        {autoupdater::ApplyOperationType::Replace, "bin/app", "bin/app", rollbackHash.value(), 12});
    rollbackPlan.operations.push_back(
        {autoupdater::ApplyOperationType::Replace, "missing", "missing", rollbackHash.value(), 12});
    auto rollbackResult = autoupdater::updater::executeApplyPlan(rollbackPlan);
    LAU_REQUIRE(!rollbackResult);
    LAU_REQUIRE(readFile(rollbackInstall / "bin/app") == "rollback-old");
    LAU_REQUIRE((std::filesystem::status(rollbackInstall / "bin/app").permissions() & modeMask) == rollbackMode);
    LAU_REQUIRE((std::filesystem::status(rollbackBackup / "bin/app").permissions() & modeMask) == rollbackMode);

    const auto mismatchInstall = root / "mismatch-install";
    const auto mismatchStaging = root / "mismatch-staging";
    const auto mismatchBackup = root / "mismatch-backup";
    writeFile(mismatchInstall / "bin/app", "rollback-old");
    writeFile(mismatchStaging / "bin/app", "rollback-new");
    writeFile(mismatchBackup / "bin/app", "rollback-old");
    std::filesystem::permissions(mismatchInstall / "bin/app", rollbackMode, std::filesystem::perm_options::replace);
    std::filesystem::permissions(mismatchBackup / "bin/app", std::filesystem::perms::owner_read,
                                 std::filesystem::perm_options::replace);
    autoupdater::ApplyPlan mismatchPlan;
    mismatchPlan.installDir = mismatchInstall;
    mismatchPlan.stagingDir = mismatchStaging;
    mismatchPlan.backupDir = mismatchBackup;
    mismatchPlan.releaseId = "permission-mismatch";
    mismatchPlan.operations.push_back(
        {autoupdater::ApplyOperationType::Replace, "bin/app", "bin/app", rollbackHash.value(), 12});
    auto mismatchResult = autoupdater::updater::executeApplyPlan(mismatchPlan);
    LAU_REQUIRE(!mismatchResult);
    LAU_REQUIRE(mismatchResult.error().code == autoupdater::ErrorCode::SecurityPolicyViolation);
    LAU_REQUIRE(readFile(mismatchInstall / "bin/app") == "rollback-old");
    LAU_REQUIRE((std::filesystem::status(mismatchInstall / "bin/app").permissions() & modeMask) == rollbackMode);

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
#endif
}
