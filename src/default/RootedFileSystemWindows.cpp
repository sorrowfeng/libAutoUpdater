#include "default/RootedFileSystemFactory.h"

#ifdef _WIN32

#include "util/PathUtil.h"
#include "util/Sha256.h"

#include <windows.h>
#include <winternl.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <climits>
#include <cstddef>
#include <cstring>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <vector>

namespace autoupdater {

namespace {

#ifndef FILE_OPEN_REPARSE_POINT
#define FILE_OPEN_REPARSE_POINT 0x00200000
#endif

constexpr NTSTATUS kStatusObjectNameNotFound = static_cast<NTSTATUS>(0xC0000034L);
constexpr NTSTATUS kStatusObjectPathNotFound = static_cast<NTSTATUS>(0xC000003AL);
constexpr NTSTATUS kStatusNoSuchFile = static_cast<NTSTATUS>(0xC000000FL);
constexpr NTSTATUS kStatusObjectNameCollision = static_cast<NTSTATUS>(0xC0000035L);
constexpr NTSTATUS kStatusSharingViolation = static_cast<NTSTATUS>(0xC0000043L);
constexpr NTSTATUS kStatusFileIsADirectory = static_cast<NTSTATUS>(0xC00000BAL);
constexpr NTSTATUS kStatusInvalidInfoClass = static_cast<NTSTATUS>(0xC0000003L);
constexpr NTSTATUS kStatusInvalidParameter = static_cast<NTSTATUS>(0xC000000DL);
constexpr NTSTATUS kStatusNotSupported = static_cast<NTSTATUS>(0xC00000BBL);
constexpr ULONG_PTR kFileOpened = 1;
constexpr ULONG_PTR kFileCreated = 2;

using NtCreateFileFunction = NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK,
                                              PLARGE_INTEGER, ULONG, ULONG, ULONG, ULONG, PVOID, ULONG);
using NtSetInformationFileFunction = NTSTATUS(NTAPI*)(HANDLE, PIO_STATUS_BLOCK, PVOID, ULONG, FILE_INFORMATION_CLASS);
using RtlNtStatusToDosErrorFunction = ULONG(NTAPI*)(NTSTATUS);

struct NativeFunctions {
    NtCreateFileFunction ntCreateFile = nullptr;
    NtSetInformationFileFunction ntSetInformationFile = nullptr;
    RtlNtStatusToDosErrorFunction statusToDosError = nullptr;
};

const NativeFunctions& nativeFunctions() {
    static const NativeFunctions functions = [] {
        NativeFunctions result;
        const auto module = GetModuleHandleW(L"ntdll.dll");
        if (module) {
            result.ntCreateFile = reinterpret_cast<NtCreateFileFunction>(GetProcAddress(module, "NtCreateFile"));
            result.ntSetInformationFile =
                reinterpret_cast<NtSetInformationFileFunction>(GetProcAddress(module, "NtSetInformationFile"));
            result.statusToDosError =
                reinterpret_cast<RtlNtStatusToDosErrorFunction>(GetProcAddress(module, "RtlNtStatusToDosError"));
        }
        return result;
    }();
    return functions;
}

class Handle {
  public:
    Handle() = default;
    explicit Handle(HANDLE value) : value_(value) {}
    ~Handle() {
        reset();
    }

    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;

    Handle(Handle&& other) noexcept : value_(std::exchange(other.value_, INVALID_HANDLE_VALUE)) {}
    Handle& operator=(Handle&& other) noexcept {
        if (this != &other) {
            reset();
            value_ = std::exchange(other.value_, INVALID_HANDLE_VALUE);
        }
        return *this;
    }

    HANDLE get() const noexcept {
        return value_;
    }

    explicit operator bool() const noexcept {
        return value_ != INVALID_HANDLE_VALUE && value_ != nullptr;
    }

    HANDLE release() noexcept {
        return std::exchange(value_, INVALID_HANDLE_VALUE);
    }

    void reset(HANDLE value = INVALID_HANDLE_VALUE) noexcept {
        if (*this) {
            CloseHandle(value_);
        }
        value_ = value;
    }

  private:
    HANDLE value_ = INVALID_HANDLE_VALUE;
};

Result<void> windowsFailure(const std::string& action, DWORD code = GetLastError()) {
    return Result<void>::fail(
        {ErrorCode::FileSystemError, action + ": " + std::system_category().message(static_cast<int>(code))});
}

template <class T> Result<T> windowsFailureValue(const std::string& action, DWORD code = GetLastError()) {
    return Result<T>::fail(
        {ErrorCode::FileSystemError, action + ": " + std::system_category().message(static_cast<int>(code))});
}

DWORD statusToError(NTSTATUS status) {
    const auto& functions = nativeFunctions();
    return functions.statusToDosError ? functions.statusToDosError(status) : ERROR_GEN_FAILURE;
}

bool isMissingStatus(NTSTATUS status) {
    return status == kStatusObjectNameNotFound || status == kStatusObjectPathNotFound || status == kStatusNoSuchFile;
}

Result<Handle> duplicateHandle(HANDLE source) {
    HANDLE duplicate = INVALID_HANDLE_VALUE;
    if (!DuplicateHandle(GetCurrentProcess(), source, GetCurrentProcess(), &duplicate, 0, FALSE,
                         DUPLICATE_SAME_ACCESS)) {
        return windowsFailureValue<Handle>("Failed to duplicate rooted directory handle");
    }
    return Result<Handle>::ok(Handle(duplicate));
}

NTSTATUS createRelative(HANDLE parent, const std::wstring& name, ACCESS_MASK access, ULONG disposition, ULONG options,
                        Handle& output, ULONG shareAccess = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        IO_STATUS_BLOCK* statusBlock = nullptr) {
    const auto& functions = nativeFunctions();
    if (!functions.ntCreateFile || name.empty() || name.size() > (USHRT_MAX / sizeof(wchar_t))) {
        return static_cast<NTSTATUS>(0xC00000BBL); // STATUS_NOT_SUPPORTED
    }

    UNICODE_STRING unicodeName{};
    unicodeName.Buffer = const_cast<PWSTR>(name.data());
    unicodeName.Length = static_cast<USHORT>(name.size() * sizeof(wchar_t));
    unicodeName.MaximumLength = unicodeName.Length;

    OBJECT_ATTRIBUTES attributes{};
    InitializeObjectAttributes(&attributes, &unicodeName, OBJ_CASE_INSENSITIVE, parent, nullptr);

    IO_STATUS_BLOCK localStatus{};
    HANDLE handle = INVALID_HANDLE_VALUE;
    const auto result = functions.ntCreateFile(
        &handle, access, &attributes, statusBlock ? statusBlock : &localStatus, nullptr, FILE_ATTRIBUTE_NORMAL,
        shareAccess, disposition, options | FILE_OPEN_REPARSE_POINT | FILE_SYNCHRONOUS_IO_NONALERT, nullptr, 0);
    if (result >= 0) {
        output.reset(handle);
    }
    return result;
}

Result<void> ensureDirectoryHandle(HANDLE handle, const std::string& context) {
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    if (!GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &attributes, sizeof(attributes))) {
        return windowsFailure("Failed to inspect " + context);
    }
    if ((attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        return Result<void>::fail({ErrorCode::PathTraversalRejected, context + " contains a reparse point"});
    }
    if ((attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        return Result<void>::fail({ErrorCode::FileSystemError, context + " is not a directory"});
    }
    return Result<void>::ok();
}

Result<void> ensureRegularHandle(HANDLE handle, const std::string& context) {
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    if (!GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &attributes, sizeof(attributes))) {
        return windowsFailure("Failed to inspect " + context);
    }
    if ((attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        return Result<void>::fail({ErrorCode::PathTraversalRejected, context + " is a reparse point"});
    }
    if ((attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        return Result<void>::fail({ErrorCode::FileSystemError, context + " is not a regular file"});
    }
    return Result<void>::ok();
}

Result<void> ensureSingleLinkHandle(HANDLE handle, const std::string& context) {
    BY_HANDLE_FILE_INFORMATION information{};
    if (!GetFileInformationByHandle(handle, &information)) {
        return windowsFailure("Failed to inspect " + context);
    }
    if (information.nNumberOfLinks != 1) {
        return Result<void>::fail({ErrorCode::SecurityPolicyViolation, context + " has multiple hard links"});
    }
    return Result<void>::ok();
}

Result<std::vector<std::wstring>> managedComponents(const std::string& relativePath) {
    auto valid = util::validateManagedPath(relativePath);
    if (!valid) {
        return Result<std::vector<std::wstring>>::fail(valid.error());
    }

    std::vector<std::wstring> components;
    const auto path = util::pathFromUtf8(relativePath);
    for (const auto& component : path) {
        const auto text = component.native();
        if (!text.empty()) {
            components.push_back(text);
        }
    }
    if (components.empty()) {
        return Result<std::vector<std::wstring>>::fail({ErrorCode::PathTraversalRejected, "Managed path is empty"});
    }
    return Result<std::vector<std::wstring>>::ok(std::move(components));
}

ACCESS_MASK directoryAccess(RootAccess access) {
    // Child opens and mutations perform their own ACL checks. The directory
    // handle is a namespace anchor. Writable roots additionally request the
    // directory rights required to flush their own namespace mutations.
    auto desired = FILE_TRAVERSE | FILE_READ_ATTRIBUTES | SYNCHRONIZE;
    if (access == RootAccess::ReadWrite) {
        desired |= FILE_ADD_FILE | FILE_ADD_SUBDIRECTORY;
    }
    return desired;
}

Result<std::string> fileIdentity(HANDLE handle);

std::mutex& directoryDurabilityCacheMutex() {
    static std::mutex mutex;
    return mutex;
}

std::unordered_set<std::string>& directoryDurabilityCache() {
    static std::unordered_set<std::string> cache;
    return cache;
}

Result<void> persistDirectoryBoundary(HANDLE parent, HANDLE child) {
    auto parentIdentity = fileIdentity(parent);
    if (!parentIdentity) {
        return Result<void>::fail(parentIdentity.error());
    }
    auto childIdentity = fileIdentity(child);
    if (!childIdentity) {
        return Result<void>::fail(childIdentity.error());
    }
    const auto key = parentIdentity.value() + ">" + childIdentity.value();
    {
        std::lock_guard<std::mutex> lock(directoryDurabilityCacheMutex());
        if (directoryDurabilityCache().find(key) != directoryDurabilityCache().end()) {
            return Result<void>::ok();
        }
    }
    if (!FlushFileBuffers(child) || !FlushFileBuffers(parent)) {
        return windowsFailure("Failed to persist rooted directory boundary");
    }
    {
        std::lock_guard<std::mutex> lock(directoryDurabilityCacheMutex());
        directoryDurabilityCache().insert(key);
    }
    return Result<void>::ok();
}

Result<Handle> openChildDirectory(HANDLE parent, const std::wstring& name, RootAccess access, bool create,
                                  bool persistNamespace = false) {
    Handle child;
    IO_STATUS_BLOCK statusBlock{};
    const auto status =
        createRelative(parent, name, directoryAccess(access), create ? FILE_OPEN_IF : FILE_OPEN, FILE_DIRECTORY_FILE,
                       child, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, &statusBlock);
    if (status < 0) {
        if (!create && isMissingStatus(status)) {
            return Result<Handle>::ok(Handle());
        }
        return windowsFailureValue<Handle>("Failed to open rooted directory component", statusToError(status));
    }
    auto checked = ensureDirectoryHandle(child.get(), "Managed directory");
    if (!checked) {
        return Result<Handle>::fail(checked.error());
    }
    if (create && persistNamespace) {
        if (statusBlock.Information != kFileOpened && statusBlock.Information != kFileCreated) {
            return Result<Handle>::fail({ErrorCode::FileSystemError, "Unexpected rooted directory creation result"});
        }
        // Persist an entry even when another process won the missing-to-create
        // race. The entry may have been created immediately before this open
        // and therefore cannot yet be assumed durable.
        auto persisted = persistDirectoryBoundary(parent, child.get());
        if (!persisted) {
            return Result<Handle>::fail(persisted.error());
        }
    }
    return Result<Handle>::ok(std::move(child));
}

Result<Handle> traverseParent(HANDLE root, RootAccess access, const std::vector<std::wstring>& components,
                              bool create) {
    auto currentResult = duplicateHandle(root);
    if (!currentResult) {
        return currentResult;
    }
    auto current = std::move(currentResult.value());
    for (std::size_t index = 0; index + 1 < components.size(); ++index) {
        const auto componentAccess =
            create ? RootAccess::ReadWrite : (index + 2 == components.size() ? access : RootAccess::ReadOnly);
        auto next = openChildDirectory(current.get(), components[index], componentAccess, create, create);
        if (!next) {
            return next;
        }
        if (!next.value()) {
            return Result<Handle>::ok(Handle());
        }
        current = std::move(next.value());
    }
    return Result<Handle>::ok(std::move(current));
}

Result<std::string> fileIdentity(HANDLE handle) {
    BY_HANDLE_FILE_INFORMATION information{};
    if (!GetFileInformationByHandle(handle, &information)) {
        return windowsFailureValue<std::string>("Failed to read rooted file identity");
    }
    std::ostringstream stream;
    stream << std::hex << std::setfill('0') << std::setw(8) << information.dwVolumeSerialNumber << ':' << std::setw(8)
           << information.nFileIndexHigh << std::setw(8) << information.nFileIndexLow;
    return Result<std::string>::ok(stream.str());
}

Result<Handle> reopenDirectoryForMutation(HANDLE current, HANDLE parent, const std::wstring& currentName,
                                          const std::filesystem::path& filesystemRoot) {
    auto expectedIdentity = fileIdentity(current);
    if (!expectedIdentity) {
        return Result<Handle>::fail(expectedIdentity.error());
    }

    Handle writable;
    if (parent != INVALID_HANDLE_VALUE && parent != nullptr) {
        const auto status = createRelative(parent, currentName, directoryAccess(RootAccess::ReadWrite), FILE_OPEN,
                                           FILE_DIRECTORY_FILE, writable);
        if (status < 0) {
            return windowsFailureValue<Handle>("Failed to reopen rooted directory for durable creation",
                                               statusToError(status));
        }
    } else {
        HANDLE handle = CreateFileW(filesystemRoot.c_str(), directoryAccess(RootAccess::ReadWrite),
                                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                    FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (handle == INVALID_HANDLE_VALUE) {
            return windowsFailureValue<Handle>("Failed to reopen filesystem root for durable creation");
        }
        writable.reset(handle);
    }

    auto checked = ensureDirectoryHandle(writable.get(), "Writable rooted directory");
    if (!checked) {
        return Result<Handle>::fail(checked.error());
    }
    auto actualIdentity = fileIdentity(writable.get());
    if (!actualIdentity) {
        return Result<Handle>::fail(actualIdentity.error());
    }
    if (actualIdentity.value() != expectedIdentity.value()) {
        return Result<Handle>::fail(
            {ErrorCode::SecurityPolicyViolation, "Rooted directory changed while preparing durable creation"});
    }
    return Result<Handle>::ok(std::move(writable));
}

NTSTATUS renameHandleRelative(HANDLE source, HANDLE targetParent, const std::wstring& targetName,
                              bool replaceIfExists) {
    const auto& functions = nativeFunctions();
    constexpr auto notSupported = static_cast<NTSTATUS>(0xC00000BBL);
    if (!functions.ntSetInformationFile || targetName.empty()) {
        return notSupported;
    }

    const auto nameBytes = targetName.size() * sizeof(wchar_t);
    constexpr auto headerBytes = offsetof(FILE_RENAME_INFO, FileName);
    if (nameBytes > ULONG_MAX - headerBytes) {
        return notSupported;
    }
    const auto bufferBytes = headerBytes + nameBytes;
    const auto storageElements = (bufferBytes + sizeof(std::max_align_t) - 1) / sizeof(std::max_align_t);
    std::vector<std::max_align_t> storage(storageElements);
    auto* rename = reinterpret_cast<FILE_RENAME_INFO*>(storage.data());
    std::memset(rename, 0, bufferBytes);
    rename->ReplaceIfExists = replaceIfExists ? TRUE : FALSE;
    rename->RootDirectory = targetParent;
    rename->FileNameLength = static_cast<DWORD>(nameBytes);
    std::memcpy(rename->FileName, targetName.data(), nameBytes);

    IO_STATUS_BLOCK statusBlock{};
    return functions.ntSetInformationFile(source, &statusBlock, rename, static_cast<ULONG>(bufferBytes),
                                          static_cast<FILE_INFORMATION_CLASS>(10));
}

NTSTATUS replaceHandleRelative(HANDLE source, HANDLE targetParent, const std::wstring& targetName) {
    const auto& functions = nativeFunctions();
    if (!functions.ntSetInformationFile || targetName.empty()) {
        return kStatusNotSupported;
    }

    struct RenameInformationEx {
        ULONG flags;
        HANDLE rootDirectory;
        ULONG fileNameLength;
        WCHAR fileName[1];
    };
    constexpr ULONG kReplaceIfExists = 0x00000001UL;
    constexpr ULONG kPosixSemantics = 0x00000002UL;
    constexpr auto kFileRenameInformationEx = static_cast<FILE_INFORMATION_CLASS>(65);
    const auto nameBytes = targetName.size() * sizeof(wchar_t);
    constexpr auto headerBytes = offsetof(RenameInformationEx, fileName);
    if (nameBytes > ULONG_MAX - headerBytes) {
        return kStatusInvalidParameter;
    }
    const auto bufferBytes = headerBytes + nameBytes;
    const auto storageElements = (bufferBytes + sizeof(std::max_align_t) - 1) / sizeof(std::max_align_t);
    std::vector<std::max_align_t> storage(storageElements);
    auto* rename = reinterpret_cast<RenameInformationEx*>(storage.data());
    std::memset(rename, 0, bufferBytes);
    rename->flags = kReplaceIfExists | kPosixSemantics;
    rename->rootDirectory = targetParent;
    rename->fileNameLength = static_cast<ULONG>(nameBytes);
    std::memcpy(rename->fileName, targetName.data(), nameBytes);

    IO_STATUS_BLOCK statusBlock{};
    const auto result = functions.ntSetInformationFile(source, &statusBlock, rename, static_cast<ULONG>(bufferBytes),
                                                       kFileRenameInformationEx);
    if (result == kStatusInvalidInfoClass || result == kStatusInvalidParameter || result == kStatusNotSupported) {
        // The legacy operation is still a single namespace mutation. It may
        // reject an open destination on older Windows, in which case apply
        // fails closed without first moving the current target away.
        return renameHandleRelative(source, targetParent, targetName, true);
    }
    return result;
}

std::wstring removalQuarantineName() {
    static std::atomic<std::uint64_t> sequence{0};
    const auto seed = std::to_string(GetCurrentProcessId()) + ":" +
                      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ":" +
                      std::to_string(sequence.fetch_add(1, std::memory_order_relaxed));
    return util::pathFromUtf8(".autoupdater-removed-" + util::sha256Bytes(seed).substr(0, 24)).native();
}

class WindowsRootedDirectory;

class WindowsRootedFile final : public IRootedFile {
  public:
    WindowsRootedFile(WindowsRootedDirectory* owner, Handle file, Handle parent, std::wstring name,
                      bool deleteOnDestroy = false, bool protectWrites = false)
        : owner_(owner), file_(std::move(file)), parent_(std::move(parent)), name_(std::move(name)),
          deleteOnDestroy_(deleteOnDestroy), protectWrites_(protectWrites) {}

    ~WindowsRootedFile() override {
        if (deleteOnDestroy_ && file_) {
            FILE_DISPOSITION_INFO disposition{};
            disposition.DeleteFile = TRUE;
            SetFileInformationByHandle(file_.get(), FileDispositionInfo, &disposition, sizeof(disposition));
        }
    }

    Result<std::size_t> read(void* buffer, std::size_t size) noexcept override {
        const auto chunk = static_cast<DWORD>(std::min<std::size_t>(size, static_cast<std::size_t>(MAXDWORD)));
        DWORD readBytes = 0;
        if (!ReadFile(file_.get(), buffer, chunk, &readBytes, nullptr)) {
            return windowsFailureValue<std::size_t>("Failed to read rooted file");
        }
        return Result<std::size_t>::ok(static_cast<std::size_t>(readBytes));
    }

    Result<void> write(const void* data, std::size_t size) noexcept override {
        if (protectWrites_) {
            auto singleLink = ensureSingleLinkHandle(file_.get(), "Writable managed file");
            if (!singleLink) {
                return singleLink;
            }
        }
        const auto* bytes = static_cast<const unsigned char*>(data);
        std::size_t offset = 0;
        while (offset < size) {
            const auto chunk =
                static_cast<DWORD>(std::min<std::size_t>(size - offset, static_cast<std::size_t>(MAXDWORD)));
            DWORD written = 0;
            if (!WriteFile(file_.get(), bytes + offset, chunk, &written, nullptr) || written == 0) {
                return windowsFailure("Failed to write rooted file");
            }
            offset += written;
        }
        return Result<void>::ok();
    }

    Result<void> seek(std::uint64_t offset) noexcept override {
        LARGE_INTEGER target{};
        target.QuadPart = static_cast<LONGLONG>(offset);
        if (offset > static_cast<std::uint64_t>(LLONG_MAX) ||
            !SetFilePointerEx(file_.get(), target, nullptr, FILE_BEGIN)) {
            return windowsFailure("Failed to seek rooted file");
        }
        return Result<void>::ok();
    }

    Result<void> truncate(std::uint64_t size) noexcept override {
        if (protectWrites_) {
            auto singleLink = ensureSingleLinkHandle(file_.get(), "Writable managed file");
            if (!singleLink) {
                return singleLink;
            }
        }
        auto moved = seek(size);
        if (!moved) {
            return moved;
        }
        if (!SetEndOfFile(file_.get())) {
            return windowsFailure("Failed to truncate rooted file");
        }
        return Result<void>::ok();
    }

    Result<void> flush() noexcept override {
        if (protectWrites_) {
            auto singleLink = ensureSingleLinkHandle(file_.get(), "Writable managed file");
            if (!singleLink) {
                return singleLink;
            }
        }
        if (!FlushFileBuffers(file_.get())) {
            return windowsFailure("Failed to flush rooted file");
        }
        return Result<void>::ok();
    }

    Result<RootedFileMetadata> metadata() noexcept override {
        LARGE_INTEGER size{};
        if (!GetFileSizeEx(file_.get(), &size) || size.QuadPart < 0) {
            return windowsFailureValue<RootedFileMetadata>("Failed to read rooted file size");
        }
        auto identity = fileIdentity(file_.get());
        if (!identity) {
            return Result<RootedFileMetadata>::fail(identity.error());
        }
        RootedFileMetadata result;
        result.size = static_cast<std::uint64_t>(size.QuadPart);
        result.identity = std::move(identity.value());
        return Result<RootedFileMetadata>::ok(std::move(result));
    }

    Result<void> setPermissions(std::filesystem::perms) noexcept override {
        if (protectWrites_) {
            auto singleLink = ensureSingleLinkHandle(file_.get(), "Writable managed file");
            if (!singleLink) {
                return singleLink;
            }
        }
        return Result<void>::ok();
    }

    WindowsRootedDirectory* owner() const noexcept {
        return owner_;
    }
    HANDLE handle() const noexcept {
        return file_.get();
    }
    HANDLE parentHandle() const noexcept {
        return parent_.get();
    }
    const std::wstring& name() const noexcept {
        return name_;
    }
    void markCommitted() noexcept {
        deleteOnDestroy_ = false;
    }

  private:
    WindowsRootedDirectory* owner_ = nullptr;
    Handle file_;
    Handle parent_;
    std::wstring name_;
    bool deleteOnDestroy_ = false;
    bool protectWrites_ = false;
};

Result<void> renameOpenedFile(WindowsRootedFile& source, HANDLE targetParent, const std::wstring& targetName,
                              const RootedEntryExpectation& expectation) {
    auto sourceParentIdentity = fileIdentity(source.parentHandle());
    if (!sourceParentIdentity) {
        return Result<void>::fail(sourceParentIdentity.error());
    }
    auto targetParentIdentity = fileIdentity(targetParent);
    if (!targetParentIdentity) {
        return Result<void>::fail(targetParentIdentity.error());
    }
    if (sourceParentIdentity.value() != targetParentIdentity.value()) {
        return Result<void>::fail(
            {ErrorCode::SecurityPolicyViolation, "Atomic replacement must remain in its opened parent directory"});
    }

    if (expectation.kind == RootedEntryExpectationKind::Missing) {
        const auto status = renameHandleRelative(source.handle(), targetParent, targetName, false);
        if (status < 0) {
            if (status == kStatusObjectNameCollision) {
                return Result<void>::fail(
                    {ErrorCode::SecurityPolicyViolation, "Managed target appeared before commit"});
            }
            return windowsFailure("Failed to install a previously missing managed target", statusToError(status));
        }
        source.markCommitted();
        if (!FlushFileBuffers(targetParent)) {
            return windowsFailure("Failed to persist installed managed target");
        }
        return Result<void>::ok();
    }

    Handle existing;
    const auto opened = createRelative(targetParent, targetName, FILE_READ_ATTRIBUTES | DELETE | SYNCHRONIZE, FILE_OPEN,
                                       FILE_NON_DIRECTORY_FILE, existing, 0);
    if (opened < 0) {
        if (isMissingStatus(opened)) {
            return Result<void>::fail({ErrorCode::SecurityPolicyViolation, "Managed target changed before commit"});
        }
        return windowsFailure("Failed to exclusively open managed target", statusToError(opened));
    }
    auto checked = ensureRegularHandle(existing.get(), "Managed target");
    if (!checked) {
        return checked;
    }
    auto identity = fileIdentity(existing.get());
    if (!identity) {
        return Result<void>::fail(identity.error());
    }
    if (identity.value() != expectation.identity) {
        return Result<void>::fail(
            {ErrorCode::SecurityPolicyViolation, "Managed target identity changed before commit"});
    }
    existing.reset();

    // FileRenameInformationEx needs the destination to share delete access.
    // First taking an exclusive handle above rejects existing competing
    // writers/deleters; reopen immediately before the single atomic rename and
    // revalidate identity so a stale expectation is never knowingly applied.
    const auto guarded = createRelative(targetParent, targetName, FILE_READ_ATTRIBUTES | DELETE | SYNCHRONIZE,
                                        FILE_OPEN, FILE_NON_DIRECTORY_FILE, existing, FILE_SHARE_DELETE);
    if (guarded < 0) {
        return windowsFailure("Failed to guard managed target for atomic replacement", statusToError(guarded));
    }
    checked = ensureRegularHandle(existing.get(), "Managed target");
    if (!checked) {
        return checked;
    }
    identity = fileIdentity(existing.get());
    if (!identity) {
        return Result<void>::fail(identity.error());
    }
    if (identity.value() != expectation.identity) {
        return Result<void>::fail(
            {ErrorCode::SecurityPolicyViolation, "Managed target identity changed before atomic replacement"});
    }

    const auto installed = replaceHandleRelative(source.handle(), targetParent, targetName);
    if (installed < 0) {
        return windowsFailure("Failed to atomically replace managed target", statusToError(installed));
    }

    source.markCommitted();
    FILE_STANDARD_INFO displacedInformation{};
    if (!GetFileInformationByHandleEx(existing.get(), FileStandardInfo, &displacedInformation,
                                      sizeof(displacedInformation))) {
        return windowsFailure("Failed to verify displaced managed target");
    }
    if (!displacedInformation.DeletePending) {
        return Result<void>::fail(
            {ErrorCode::SecurityPolicyViolation, "Managed target changed during atomic replacement"});
    }

    Handle namedInstalled;
    const auto namedStatus = createRelative(targetParent, targetName, FILE_READ_ATTRIBUTES | SYNCHRONIZE, FILE_OPEN,
                                            FILE_NON_DIRECTORY_FILE, namedInstalled);
    if (namedStatus < 0) {
        return windowsFailure("Failed to verify atomically installed target", statusToError(namedStatus));
    }
    auto sourceIdentity = fileIdentity(source.handle());
    if (!sourceIdentity) {
        return Result<void>::fail(sourceIdentity.error());
    }
    auto namedIdentity = fileIdentity(namedInstalled.get());
    if (!namedIdentity) {
        return Result<void>::fail(namedIdentity.error());
    }
    if (sourceIdentity.value() != namedIdentity.value()) {
        return Result<void>::fail(
            {ErrorCode::SecurityPolicyViolation, "Installed target changed during atomic replacement"});
    }
    if (!FlushFileBuffers(targetParent)) {
        return windowsFailure("Failed to persist atomically replaced managed target");
    }
    return Result<void>::ok();
}

class WindowsTemporaryFile final : public IRootedTemporaryFile {
  public:
    WindowsTemporaryFile(WindowsRootedDirectory& root, std::unique_ptr<WindowsRootedFile> file, std::string target)
        : root_(root), file_(std::move(file)), target_(std::move(target)) {}

    IRootedFile& file() noexcept override {
        return *file_;
    }

    Result<void> commit(const RootedEntryExpectation& expectation) noexcept override;

  private:
    WindowsRootedDirectory& root_;
    std::unique_ptr<WindowsRootedFile> file_;
    std::string target_;
};

class WindowsLock final : public IRootedLock {
  public:
    explicit WindowsLock(Handle handle) : handle_(std::move(handle)) {}
    ~WindowsLock() override = default;

  private:
    Handle handle_;
};

class WindowsRootedDirectory final : public IRootedDirectory {
  public:
    WindowsRootedDirectory(Handle root, RootAccess access) : root_(std::move(root)), access_(access) {}

    Result<RootedOpenResult> openRegularFile(const std::string& relativePath, RootedFileOpenMode mode,
                                             RootedDirectoryCreationMode directoryMode) noexcept override {
        try {
            (void)directoryMode;
            auto components = managedComponents(relativePath);
            if (!components) {
                return Result<RootedOpenResult>::fail(components.error());
            }
            const bool writing = mode != RootedFileOpenMode::ReadOnly;
            const bool creating =
                mode == RootedFileOpenMode::OpenOrCreate || mode == RootedFileOpenMode::CreateOrTruncate;
            if (writing && access_ != RootAccess::ReadWrite) {
                return Result<RootedOpenResult>::fail(
                    {ErrorCode::SecurityPolicyViolation, "Rooted directory is read-only"});
            }
            auto parent = traverseParent(root_.get(), writing ? RootAccess::ReadWrite : RootAccess::ReadOnly,
                                         components.value(), creating);
            if (!parent) {
                return Result<RootedOpenResult>::fail(parent.error());
            }
            if (!parent.value()) {
                return Result<RootedOpenResult>::ok({});
            }

            ACCESS_MASK desired = FILE_READ_DATA | FILE_READ_ATTRIBUTES | SYNCHRONIZE;
            if (writing) {
                desired |= FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_WRITE_ATTRIBUTES | DELETE;
            }
            Handle file;
            const auto status = createRelative(parent.value().get(), components.value().back(), desired,
                                               creating ? FILE_OPEN_IF : FILE_OPEN, FILE_NON_DIRECTORY_FILE, file,
                                               writing ? 0 : FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE);
            if (status < 0) {
                if (isMissingStatus(status) && !creating) {
                    return Result<RootedOpenResult>::ok({});
                }
                return windowsFailureValue<RootedOpenResult>("Failed to open rooted regular file",
                                                             statusToError(status));
            }
            auto checked = ensureRegularHandle(file.get(), "Managed file");
            if (!checked) {
                return Result<RootedOpenResult>::fail(checked.error());
            }
            if (writing) {
                auto singleLink = ensureSingleLinkHandle(file.get(), "Writable managed file");
                if (!singleLink) {
                    return Result<RootedOpenResult>::fail(singleLink.error());
                }
            }

            auto result = std::make_unique<WindowsRootedFile>(this, std::move(file), std::move(parent.value()),
                                                              components.value().back(), false, writing);
            if (mode == RootedFileOpenMode::CreateOrTruncate) {
                auto truncated = result->truncate(0);
                if (!truncated) {
                    return Result<RootedOpenResult>::fail(truncated.error());
                }
            }
            RootedOpenResult opened;
            opened.file = std::move(result);
            return Result<RootedOpenResult>::ok(std::move(opened));
        } catch (...) {
            return Result<RootedOpenResult>::fail({ErrorCode::FileSystemError, "Unexpected rooted file open failure"});
        }
    }

    Result<std::unique_ptr<IRootedTemporaryFile>>
    createAtomicReplacement(const std::string& relativePath,
                            RootedDirectoryCreationMode directoryMode) noexcept override {
        try {
            (void)directoryMode;
            if (access_ != RootAccess::ReadWrite) {
                return Result<std::unique_ptr<IRootedTemporaryFile>>::fail(
                    {ErrorCode::SecurityPolicyViolation, "Rooted directory is read-only"});
            }
            auto components = managedComponents(relativePath);
            if (!components) {
                return Result<std::unique_ptr<IRootedTemporaryFile>>::fail(components.error());
            }
            auto parent = traverseParent(root_.get(), RootAccess::ReadWrite, components.value(), true);
            if (!parent) {
                return Result<std::unique_ptr<IRootedTemporaryFile>>::fail(parent.error());
            }

            static std::atomic<std::uint64_t> sequence{0};
            for (int attempt = 0; attempt < 32; ++attempt) {
                const auto seed = std::to_string(GetCurrentProcessId()) + ":" +
                                  std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ":" +
                                  std::to_string(sequence.fetch_add(1, std::memory_order_relaxed));
                const auto temporaryUtf8 = ".autoupdater-tmp-" + util::sha256Bytes(seed).substr(0, 24);
                const auto temporaryName = util::pathFromUtf8(temporaryUtf8).native();
                Handle file;
                const auto status =
                    createRelative(parent.value().get(), temporaryName,
                                   FILE_READ_DATA | FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_READ_ATTRIBUTES |
                                       FILE_WRITE_ATTRIBUTES | DELETE | SYNCHRONIZE,
                                   FILE_CREATE, FILE_NON_DIRECTORY_FILE, file, 0);
                if (status == kStatusObjectNameCollision) {
                    continue;
                }
                if (status < 0) {
                    return windowsFailureValue<std::unique_ptr<IRootedTemporaryFile>>(
                        "Failed to create rooted temporary file", statusToError(status));
                }
                auto rootedFile = std::make_unique<WindowsRootedFile>(this, std::move(file), std::move(parent.value()),
                                                                      temporaryName, true, true);
                auto temporary = std::make_unique<WindowsTemporaryFile>(*this, std::move(rootedFile), relativePath);
                return Result<std::unique_ptr<IRootedTemporaryFile>>::ok(std::move(temporary));
            }
            return Result<std::unique_ptr<IRootedTemporaryFile>>::fail(
                {ErrorCode::FileSystemError, "Failed to allocate a unique rooted temporary file"});
        } catch (...) {
            return Result<std::unique_ptr<IRootedTemporaryFile>>::fail(
                {ErrorCode::FileSystemError, "Unexpected rooted temporary file failure"});
        }
    }

    Result<void> replaceWithOpenedFile(IRootedFile& source, const std::string& relativePath,
                                       const RootedEntryExpectation& expectation) noexcept override {
        try {
            if (access_ != RootAccess::ReadWrite) {
                return Result<void>::fail({ErrorCode::SecurityPolicyViolation, "Rooted directory is read-only"});
            }
            auto* file = dynamic_cast<WindowsRootedFile*>(&source);
            if (!file || file->owner() != this) {
                return Result<void>::fail(
                    {ErrorCode::SecurityPolicyViolation, "Rooted source belongs to a different root"});
            }
            auto singleLink = ensureSingleLinkHandle(file->handle(), "Atomic replacement source");
            if (!singleLink) {
                return singleLink;
            }
            auto components = managedComponents(relativePath);
            if (!components) {
                return Result<void>::fail(components.error());
            }
            auto targetParent = traverseParent(root_.get(), RootAccess::ReadWrite, components.value(), false);
            if (!targetParent) {
                return Result<void>::fail(targetParent.error());
            }
            if (!targetParent.value()) {
                return Result<void>::fail(
                    {ErrorCode::SecurityPolicyViolation, "Managed target parent changed before commit"});
            }
            return renameOpenedFile(*file, targetParent.value().get(), components.value().back(), expectation);
        } catch (...) {
            return Result<void>::fail({ErrorCode::FileSystemError, "Unexpected rooted replacement failure"});
        }
    }

    Result<void> removeRegularFile(const std::string& relativePath,
                                   const RootedEntryExpectation& expectation) noexcept override {
        try {
            if (access_ != RootAccess::ReadWrite) {
                return Result<void>::fail({ErrorCode::SecurityPolicyViolation, "Rooted directory is read-only"});
            }
            auto components = managedComponents(relativePath);
            if (!components) {
                return Result<void>::fail(components.error());
            }
            auto parent = traverseParent(root_.get(), RootAccess::ReadWrite, components.value(), false);
            if (!parent) {
                return Result<void>::fail(parent.error());
            }
            if (!parent.value()) {
                if (expectation.kind == RootedEntryExpectationKind::Missing) {
                    return Result<void>::ok();
                }
                return Result<void>::fail({ErrorCode::SecurityPolicyViolation, "Managed remove target disappeared"});
            }
            Handle file;
            const auto status = createRelative(parent.value().get(), components.value().back(),
                                               FILE_READ_ATTRIBUTES | DELETE | SYNCHRONIZE, FILE_OPEN,
                                               FILE_NON_DIRECTORY_FILE, file, 0);
            if (status < 0) {
                if (isMissingStatus(status) && expectation.kind == RootedEntryExpectationKind::Missing) {
                    return Result<void>::ok();
                }
                return windowsFailure("Failed to open managed file for removal", statusToError(status));
            }
            auto checked = ensureRegularHandle(file.get(), "Managed remove target");
            if (!checked) {
                return checked;
            }
            auto identity = fileIdentity(file.get());
            if (!identity) {
                return Result<void>::fail(identity.error());
            }
            if (expectation.kind != RootedEntryExpectationKind::Identity || identity.value() != expectation.identity) {
                return Result<void>::fail(
                    {ErrorCode::SecurityPolicyViolation, "Managed remove target identity changed"});
            }
            bool quarantined = false;
            for (int attempt = 0; attempt < 32; ++attempt) {
                const auto quarantine = removalQuarantineName();
                const auto renamed = renameHandleRelative(file.get(), parent.value().get(), quarantine, false);
                if (renamed >= 0) {
                    quarantined = true;
                    break;
                }
                if (renamed != kStatusObjectNameCollision) {
                    return windowsFailure("Failed to quarantine managed removal target", statusToError(renamed));
                }
            }
            if (!quarantined) {
                return Result<void>::fail(
                    {ErrorCode::FileSystemError, "Failed to allocate a managed removal quarantine name"});
            }
            if (!FlushFileBuffers(parent.value().get())) {
                return windowsFailure("Failed to persist managed removal namespace change");
            }
            FILE_DISPOSITION_INFO disposition{};
            disposition.DeleteFile = TRUE;
            if (!SetFileInformationByHandle(file.get(), FileDispositionInfo, &disposition, sizeof(disposition))) {
                // The requested target name is already durably absent. Report
                // cleanup failure so the journal can reconcile; the uniquely
                // named private quarantine may safely remain for later GC.
                return windowsFailure("Failed to clean managed removal quarantine");
            }
            return Result<void>::ok();
        } catch (...) {
            return Result<void>::fail({ErrorCode::FileSystemError, "Unexpected rooted removal failure"});
        }
    }

    Result<std::unique_ptr<IRootedLock>> acquireExclusiveLock(const std::string& relativePath) noexcept override {
        try {
            if (access_ != RootAccess::ReadWrite) {
                return Result<std::unique_ptr<IRootedLock>>::fail(
                    {ErrorCode::SecurityPolicyViolation, "Rooted directory is read-only"});
            }
            auto components = managedComponents(relativePath);
            if (!components) {
                return Result<std::unique_ptr<IRootedLock>>::fail(components.error());
            }
            auto parent = traverseParent(root_.get(), RootAccess::ReadWrite, components.value(), true);
            if (!parent) {
                return Result<std::unique_ptr<IRootedLock>>::fail(parent.error());
            }
            Handle lock;
            const auto status =
                // A zero share mask only conflicts with another open when the
                // handle requests a share-checked data/delete access. Attribute
                // access alone allowed multiple processes to believe they held
                // this lock concurrently.
                createRelative(parent.value().get(), components.value().back(),
                               FILE_READ_DATA | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
                               FILE_OPEN_IF, FILE_NON_DIRECTORY_FILE, lock, 0);
            if (status == kStatusSharingViolation || status == kStatusFileIsADirectory) {
                return Result<std::unique_ptr<IRootedLock>>::fail(
                    {ErrorCode::ApplyFailed, "Another update appears to be running"});
            }
            if (status < 0) {
                return windowsFailureValue<std::unique_ptr<IRootedLock>>("Failed to open update lock",
                                                                         statusToError(status));
            }
            auto regular = ensureRegularHandle(lock.get(), "update lock");
            if (!regular) {
                return Result<std::unique_ptr<IRootedLock>>::fail(regular.error());
            }
            auto result = std::make_unique<WindowsLock>(std::move(lock));
            return Result<std::unique_ptr<IRootedLock>>::ok(std::move(result));
        } catch (...) {
            return Result<std::unique_ptr<IRootedLock>>::fail(
                {ErrorCode::FileSystemError, "Unexpected rooted lock failure"});
        }
    }

  private:
    Handle root_;
    RootAccess access_ = RootAccess::ReadOnly;
};

Result<void> WindowsTemporaryFile::commit(const RootedEntryExpectation& expectation) noexcept {
    auto flushed = file_->flush();
    if (!flushed) {
        return flushed;
    }
    return root_.replaceWithOpenedFile(*file_, target_, expectation);
}

Result<Handle> openRootPath(const std::filesystem::path& path, RootAccess access, bool create) {
    if (!path.is_absolute()) {
        return Result<Handle>::fail({ErrorCode::SecurityPolicyViolation, "Rooted filesystem paths must be absolute"});
    }
    const auto normalized = path.lexically_normal();
    const auto rootPath = normalized.root_path();
    if (rootPath.empty()) {
        return Result<Handle>::fail({ErrorCode::SecurityPolicyViolation, "Rooted filesystem path has no root"});
    }

    std::vector<std::wstring> components;
    for (const auto& component : normalized.relative_path()) {
        const auto name = component.native();
        if (name.empty() || name == L".") {
            continue;
        }
        if (name == L"..") {
            return Result<Handle>::fail(
                {ErrorCode::PathTraversalRejected, "Rooted filesystem path contains parent traversal"});
        }
        components.push_back(name);
    }

    const auto rootAccess = components.empty() ? access : RootAccess::ReadOnly;
    HANDLE rootHandle = CreateFileW(rootPath.c_str(), directoryAccess(rootAccess),
                                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                    FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (rootHandle == INVALID_HANDLE_VALUE) {
        return windowsFailureValue<Handle>("Failed to open filesystem root");
    }
    Handle current(rootHandle);
    auto checked = ensureDirectoryHandle(current.get(), "Filesystem root");
    if (!checked) {
        return Result<Handle>::fail(checked.error());
    }

    Handle parent;
    std::wstring currentName;
    bool creating = false;
    for (std::size_t index = 0; index < components.size(); ++index) {
        if (!creating) {
            const auto componentAccess = index + 1 == components.size() ? access : RootAccess::ReadOnly;
            auto existing = openChildDirectory(current.get(), components[index], componentAccess, false);
            if (!existing) {
                return existing;
            }
            if (existing.value()) {
                parent = std::move(current);
                current = std::move(existing.value());
                currentName = components[index];
                continue;
            }
            if (!create) {
                return Result<Handle>::fail({ErrorCode::FileSystemError, "Rooted filesystem path does not exist"});
            }

            auto writableParent = reopenDirectoryForMutation(current.get(), parent.get(), currentName, rootPath);
            if (!writableParent) {
                return writableParent;
            }
            current = std::move(writableParent.value());
            parent.reset();
            creating = true;
        }

        // Once the first missing component is found, keep every newly opened
        // directory writable so each following namespace boundary can be
        // durably created relative to its already-pinned parent handle.
        auto next = openChildDirectory(current.get(), components[index], RootAccess::ReadWrite, true, true);
        if (!next) {
            return next;
        }
        if (!next.value()) {
            return Result<Handle>::fail({ErrorCode::FileSystemError, "Rooted filesystem path does not exist"});
        }
        current = std::move(next.value());
    }
    return Result<Handle>::ok(std::move(current));
}

} // namespace

Result<std::unique_ptr<IRootedDirectory>>
openDefaultRootedDirectory(const std::filesystem::path& path, RootAccess access, bool create,
                           RootedDirectoryCreationMode directoryMode) noexcept {
    try {
        (void)directoryMode;
        if (!nativeFunctions().ntCreateFile || !nativeFunctions().ntSetInformationFile ||
            !nativeFunctions().statusToDosError) {
            return Result<std::unique_ptr<IRootedDirectory>>::fail(
                {ErrorCode::SecurityPolicyViolation, "Secure rooted filesystem support is unavailable"});
        }
        auto root = openRootPath(path, access, create);
        if (!root) {
            return Result<std::unique_ptr<IRootedDirectory>>::fail(root.error());
        }
        auto result = std::make_unique<WindowsRootedDirectory>(std::move(root.value()), access);
        return Result<std::unique_ptr<IRootedDirectory>>::ok(std::move(result));
    } catch (...) {
        return Result<std::unique_ptr<IRootedDirectory>>::fail(
            {ErrorCode::FileSystemError, "Unexpected rooted filesystem failure"});
    }
}

} // namespace autoupdater

#endif
