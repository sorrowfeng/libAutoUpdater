#include "TestCommon.h"

#include "libAutoUpdater/interfaces/IFileSystem.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef _WIN32
#include <aclapi.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace {

std::uint64_t processId() noexcept {
#ifdef _WIN32
    return static_cast<std::uint64_t>(GetCurrentProcessId());
#else
    return static_cast<std::uint64_t>(::getpid());
#endif
}

class TemporaryDirectory final {
  public:
    explicit TemporaryDirectory(const std::string& name) {
        static std::atomic<std::uint64_t> sequence{0};
        path_ = std::filesystem::temp_directory_path() / "libAutoUpdater-filesystem-tests" /
                (std::to_string(processId()) + "-" + std::to_string(sequence.fetch_add(1)) + "-" + name);
        std::error_code error;
        std::filesystem::remove_all(path_, error);
        error.clear();
        std::filesystem::create_directories(path_, error);
        if (error) {
            throw std::runtime_error("Failed to create filesystem test directory: " + error.message());
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
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    LAU_REQUIRE(output.good());
    output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    LAU_REQUIRE(output.good());
    output.flush();
    LAU_REQUIRE(output.good());
    output.close();
    LAU_REQUIRE(!output.fail());
}

std::string readFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    LAU_REQUIRE(input.good());
    const std::string contents((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    LAU_REQUIRE(!input.bad());
    return contents;
}

void requireNoWriteTemporaries(const std::filesystem::path& directory) {
    for (const auto& entry : std::filesystem::recursive_directory_iterator(directory)) {
        const auto name = entry.path().filename().string();
        LAU_REQUIRE(name.find(".autoupdater-write-") != 0);
        LAU_REQUIRE(name.find(".autoupdater-tmp-") != 0);
        LAU_REQUIRE(name.find(".autoupdater-private-") != 0);
    }
}

#ifdef _WIN32

struct DaclSnapshot {
    std::vector<unsigned char> bytes;
    bool protectedDacl = false;
};

bool supportsPersistentAcls(const std::filesystem::path& path) {
    std::array<wchar_t, MAX_PATH> volumePath{};
    if (!GetVolumePathNameW(path.c_str(), volumePath.data(), static_cast<DWORD>(volumePath.size()))) {
        return false;
    }
    DWORD flags = 0;
    return GetVolumeInformationW(volumePath.data(), nullptr, 0, nullptr, nullptr, &flags, nullptr, 0) != FALSE &&
           (flags & FILE_PERSISTENT_ACLS) != 0;
}

DaclSnapshot readDacl(const std::filesystem::path& path) {
    std::wstring mutablePath = path.native();
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    PACL dacl = nullptr;
    const DWORD queried = GetNamedSecurityInfoW(mutablePath.data(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr,
                                                nullptr, &dacl, nullptr, &descriptor);
    LAU_REQUIRE(queried == ERROR_SUCCESS);
    SECURITY_DESCRIPTOR_CONTROL control = 0;
    DWORD revision = 0;
    LAU_REQUIRE(GetSecurityDescriptorControl(descriptor, &control, &revision) != FALSE);
    DaclSnapshot snapshot;
    snapshot.protectedDacl = (control & SE_DACL_PROTECTED) != 0;
    if (dacl) {
        const auto* begin = reinterpret_cast<const unsigned char*>(dacl);
        snapshot.bytes.assign(begin, begin + dacl->AclSize);
    }
    LocalFree(descriptor);
    return snapshot;
}

void protectExistingDacl(const std::filesystem::path& path) {
    std::wstring mutablePath = path.native();
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    PACL dacl = nullptr;
    const DWORD queried = GetNamedSecurityInfoW(mutablePath.data(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr,
                                                nullptr, &dacl, nullptr, &descriptor);
    LAU_REQUIRE(queried == ERROR_SUCCESS);
    const DWORD applied = SetNamedSecurityInfoW(mutablePath.data(), SE_FILE_OBJECT,
                                                DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                                                nullptr, nullptr, dacl, nullptr);
    LocalFree(descriptor);
    LAU_REQUIRE(applied == ERROR_SUCCESS);
}

#endif

} // namespace

void testStdFileSystemAtomicReplacementPreservesTargetOnFailure() {
    TemporaryDirectory temporary("replace");
    const auto source = temporary.path() / "source.txt";
    const auto target = temporary.path() / "target.txt";
    auto fileSystem = autoupdater::createDefaultFileSystem();

    writeFile(target, "trusted-old-content");
    const auto missingSource = fileSystem->renameOrReplace(source, target);
    LAU_REQUIRE(!missingSource);
    LAU_REQUIRE(missingSource.error().code == autoupdater::ErrorCode::FileSystemError);
    LAU_REQUIRE(readFile(target) == "trusted-old-content");

    writeFile(source, "complete-new-content");
    const auto replaced = fileSystem->renameOrReplace(source, target);
    LAU_REQUIRE(replaced);
    LAU_REQUIRE(!std::filesystem::exists(source));
    LAU_REQUIRE(readFile(target) == "complete-new-content");

    LAU_REQUIRE(fileSystem->renameOrReplace(target, target));
    LAU_REQUIRE(readFile(target) == "complete-new-content");
}

void testStdFileSystemAtomicTextWritesPreserveContentAndPermissions() {
    TemporaryDirectory temporary("write");
    const auto target = temporary.path() / "settings.json";
    auto fileSystem = autoupdater::createDefaultFileSystem();

    LAU_REQUIRE(fileSystem->writeText(target, "first"));
    LAU_REQUIRE(readFile(target) == "first");

#ifndef _WIN32
    const auto privatePermissions = std::filesystem::status(target).permissions() & std::filesystem::perms::mask;
    LAU_REQUIRE(privatePermissions == (std::filesystem::perms::owner_read | std::filesystem::perms::owner_write));
    constexpr auto expectedPermissions =
        std::filesystem::perms::owner_read | std::filesystem::perms::owner_write | std::filesystem::perms::group_read;
    std::error_code permissionError;
    std::filesystem::permissions(target, expectedPermissions, std::filesystem::perm_options::replace, permissionError);
    LAU_REQUIRE(!permissionError);
#else
    std::optional<DaclSnapshot> expectedDacl;
    if (supportsPersistentAcls(target)) {
        protectExistingDacl(target);
        expectedDacl = readDacl(target);
        LAU_REQUIRE(expectedDacl->protectedDacl);
    }
#endif

    const auto replacedText = fileSystem->writeText(target, "second-complete-value");
    if (!replacedText) {
        throw std::runtime_error("Atomic text replacement failed: " + replacedText.error().message);
    }
    LAU_REQUIRE(readFile(target) == "second-complete-value");
#ifndef _WIN32
    const auto actualPermissions = std::filesystem::status(target).permissions() & std::filesystem::perms::mask;
    LAU_REQUIRE(actualPermissions == expectedPermissions);
#else
    if (expectedDacl) {
        const auto actualDacl = readDacl(target);
        LAU_REQUIRE(actualDacl.protectedDacl == expectedDacl->protectedDacl);
        LAU_REQUIRE(actualDacl.bytes == expectedDacl->bytes);
    }
#endif
    requireNoWriteTemporaries(temporary.path());

    const auto directoryTarget = temporary.path() / "must-survive";
    std::filesystem::create_directory(directoryTarget);
    writeFile(directoryTarget / "marker.txt", "preserved");
    const auto rejected = fileSystem->writeText(directoryTarget, "must-not-replace-directory");
    LAU_REQUIRE(!rejected);
    LAU_REQUIRE(readFile(directoryTarget / "marker.txt") == "preserved");
    requireNoWriteTemporaries(temporary.path());
}

void testStdFileSystemCopyPublishesOnlyCompleteFiles() {
    TemporaryDirectory temporary("copy");
    const auto source = temporary.path() / "source.bin";
    const auto target = temporary.path() / "target.bin";
    auto fileSystem = autoupdater::createDefaultFileSystem();

    writeFile(target, "old-target");
    const auto missingSource = fileSystem->copyFile(source, target, true);
    LAU_REQUIRE(!missingSource);
    LAU_REQUIRE(readFile(target) == "old-target");

    writeFile(source, "complete-source-payload");
#ifndef _WIN32
    constexpr auto sourcePermissions =
        std::filesystem::perms::owner_read | std::filesystem::perms::owner_write | std::filesystem::perms::group_read;
    std::error_code permissionError;
    std::filesystem::permissions(source, sourcePermissions, std::filesystem::perm_options::replace, permissionError);
    LAU_REQUIRE(!permissionError);
#else
    std::optional<DaclSnapshot> expectedDacl;
    if (supportsPersistentAcls(target)) {
        protectExistingDacl(target);
        expectedDacl = readDacl(target);
    }
#endif
    const auto copied = fileSystem->copyFile(source, target, true);
    if (!copied) {
        throw std::runtime_error("Atomic copy replacement failed: " + copied.error().message);
    }
    LAU_REQUIRE(readFile(source) == "complete-source-payload");
    LAU_REQUIRE(readFile(target) == "complete-source-payload");
#ifndef _WIN32
    const auto targetPermissions = std::filesystem::status(target).permissions() & std::filesystem::perms::mask;
    LAU_REQUIRE(targetPermissions == sourcePermissions);
#else
    if (expectedDacl) {
        const auto actualDacl = readDacl(target);
        LAU_REQUIRE(actualDacl.protectedDacl == expectedDacl->protectedDacl);
        LAU_REQUIRE(actualDacl.bytes == expectedDacl->bytes);
    }
#endif

    writeFile(source, "newer-source");
    const auto noOverwrite = fileSystem->copyFile(source, target, false);
    LAU_REQUIRE(!noOverwrite);
    LAU_REQUIRE(readFile(target) == "complete-source-payload");
    requireNoWriteTemporaries(temporary.path());
}

void testRootedPermissionCopyPreservesNativeState() {
    TemporaryDirectory temporary("native-permissions");
    const auto sourcePath = temporary.path() / "source.bin";
    const auto targetPath = temporary.path() / "target.bin";
    writeFile(sourcePath, "source");
    writeFile(targetPath, "target");

#ifndef _WIN32
    constexpr auto sourcePermissions =
        std::filesystem::perms::owner_read | std::filesystem::perms::owner_write | std::filesystem::perms::group_read;
    constexpr auto targetPermissions = std::filesystem::perms::owner_read | std::filesystem::perms::owner_write;
    std::error_code permissionError;
    std::filesystem::permissions(sourcePath, sourcePermissions, std::filesystem::perm_options::replace,
                                 permissionError);
    LAU_REQUIRE(!permissionError);
    std::filesystem::permissions(targetPath, targetPermissions, std::filesystem::perm_options::replace,
                                 permissionError);
    LAU_REQUIRE(!permissionError);
#else
    if (!supportsPersistentAcls(sourcePath)) {
        return;
    }
    protectExistingDacl(sourcePath);
    const auto sourceDacl = readDacl(sourcePath);
    const auto originalTargetDacl = readDacl(targetPath);
    LAU_REQUIRE(sourceDacl.protectedDacl);
    LAU_REQUIRE(!originalTargetDacl.protectedDacl);
#endif

    auto fileSystem = autoupdater::createDefaultFileSystem();
    auto root = fileSystem->openRoot(temporary.path(), autoupdater::RootAccess::ReadWrite, false,
                                     autoupdater::RootedDirectoryCreationMode::InstalledContent);
    LAU_REQUIRE(root);
    auto source = root.value()->openRegularFile("source.bin", autoupdater::RootedFileOpenMode::ReadOnly,
                                                autoupdater::RootedDirectoryCreationMode::InstalledContent);
    LAU_REQUIRE(source && source.value().exists());
    auto target = root.value()->openRegularFile("target.bin", autoupdater::RootedFileOpenMode::ReadOnly,
                                                autoupdater::RootedDirectoryCreationMode::InstalledContent);
    LAU_REQUIRE(target && target.value().exists());
    auto targetMetadata = target.value().file->metadata();
    LAU_REQUIRE(targetMetadata);
    LAU_REQUIRE(target.value().file->close());

    const auto publish = [&](const std::string& name, const autoupdater::RootedEntryExpectation& expectation) {
        auto replacement =
            root.value()->createAtomicReplacement(name, autoupdater::RootedDirectoryCreationMode::InstalledContent);
        LAU_REQUIRE(replacement);
        LAU_REQUIRE(replacement.value()->file().write("restored", 8));
        LAU_REQUIRE(replacement.value()->file().copyPermissionsFrom(*source.value().file));
        LAU_REQUIRE(replacement.value()->commit(expectation));
        LAU_REQUIRE(replacement.value()->discard());
    };
    publish("target.bin", autoupdater::RootedEntryExpectation::matching(targetMetadata.value()));
    publish("restored.bin", autoupdater::RootedEntryExpectation::missing());
    LAU_REQUIRE(source.value().file->close());

    LAU_REQUIRE(readFile(targetPath) == "restored");
    LAU_REQUIRE(readFile(temporary.path() / "restored.bin") == "restored");
#ifndef _WIN32
    const auto mask =
        std::filesystem::perms::owner_all | std::filesystem::perms::group_all | std::filesystem::perms::others_all;
    LAU_REQUIRE((std::filesystem::status(targetPath).permissions() & mask) == sourcePermissions);
    LAU_REQUIRE((std::filesystem::status(temporary.path() / "restored.bin").permissions() & mask) == sourcePermissions);
#else
    for (const auto& restored : {targetPath, temporary.path() / "restored.bin"}) {
        const auto restoredDacl = readDacl(restored);
        LAU_REQUIRE(restoredDacl.protectedDacl == sourceDacl.protectedDacl);
        LAU_REQUIRE(restoredDacl.bytes == sourceDacl.bytes);
    }
#endif
    requireNoWriteTemporaries(temporary.path());
}
