#include "default/RootedFileSystemFactory.h"

#ifndef _WIN32

#include "util/PathUtil.h"
#include "util/Sha256.h"

#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#if defined(__linux__)
#include <sys/syscall.h>
#elif defined(__APPLE__)
#include <stdio.h>
#endif
#include <unistd.h>

#ifndef O_NOFOLLOW
#error "Secure rooted filesystem support requires O_NOFOLLOW"
#endif

#include <array>
#include <atomic>
#include <chrono>
#include <iomanip>
#include <limits>
#include <mutex>
#include <random>
#include <sstream>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <vector>

namespace autoupdater {

namespace {

#if defined(__linux__)
#ifndef RENAME_NOREPLACE
#define RENAME_NOREPLACE (1U << 0)
#endif
#ifndef RENAME_EXCHANGE
#define RENAME_EXCHANGE (1U << 1)
#endif
#elif defined(__APPLE__)
#ifndef RENAME_SWAP
#define RENAME_SWAP 0x00000002
#endif
#ifndef RENAME_EXCL
#define RENAME_EXCL 0x00000004
#endif
#endif

class FileDescriptor {
  public:
    FileDescriptor() = default;
    explicit FileDescriptor(int value) : value_(value) {}
    ~FileDescriptor() {
        reset();
    }

    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;

    FileDescriptor(FileDescriptor&& other) noexcept : value_(std::exchange(other.value_, -1)) {}
    FileDescriptor& operator=(FileDescriptor&& other) noexcept {
        if (this != &other) {
            reset();
            value_ = std::exchange(other.value_, -1);
        }
        return *this;
    }

    int get() const noexcept {
        return value_;
    }
    explicit operator bool() const noexcept {
        return value_ >= 0;
    }
    Result<void> close() noexcept {
        if (value_ < 0) {
            return Result<void>::ok();
        }
        const int descriptor = std::exchange(value_, -1);
        if (::close(descriptor) != 0) {
            const int code = errno;
            return Result<void>::fail({ErrorCode::FileSystemError,
                                       "Failed to close rooted descriptor: " + std::generic_category().message(code)});
        }
        return Result<void>::ok();
    }
    void reset(int value = -1) noexcept {
        (void)close();
        value_ = value;
    }

  private:
    int value_ = -1;
};

Result<void> posixFailure(const std::string& action, int code = errno) {
    return Result<void>::fail({ErrorCode::FileSystemError, action + ": " + std::generic_category().message(code)});
}

template <class T> Result<T> posixFailureValue(const std::string& action, int code = errno) {
    return Result<T>::fail({ErrorCode::FileSystemError, action + ": " + std::generic_category().message(code)});
}

Result<void> syncDescriptor(int descriptor, const std::string& action) {
    for (;;) {
        if (::fsync(descriptor) == 0) {
            return Result<void>::ok();
        }
        if (errno != EINTR) {
            return posixFailure(action);
        }
    }
}

Result<void> syncFileDescriptor(int descriptor, const std::string& action) {
#if defined(__APPLE__) && defined(F_FULLFSYNC)
    for (;;) {
        if (::fcntl(descriptor, F_FULLFSYNC) == 0) {
            return Result<void>::ok();
        }
        if (errno == EINTR) {
            continue;
        }
        // Some Apple-backed filesystems do not implement F_FULLFSYNC. Retain
        // the strongest barrier the filesystem exposes instead of making
        // updates unusable on those volumes.
        if (errno != EINVAL && errno != ENOTSUP) {
            return posixFailure(action);
        }
        break;
    }
#endif
    return syncDescriptor(descriptor, action);
}

Result<void> syncDirectory(int descriptor, const std::string& context) {
    return syncDescriptor(descriptor, "Failed to persist " + context + " directory");
}

Result<std::string> directoryDurabilityKey(int parent, int child) {
    struct stat parentStatus {};
    struct stat childStatus {};
    if (::fstat(parent, &parentStatus) != 0 || ::fstat(child, &childStatus) != 0) {
        return posixFailureValue<std::string>("Failed to identify rooted directory durability boundary");
    }
    std::ostringstream stream;
    stream << static_cast<std::uintmax_t>(parentStatus.st_dev) << ':'
           << static_cast<std::uintmax_t>(parentStatus.st_ino) << ':' << static_cast<std::uintmax_t>(childStatus.st_dev)
           << ':' << static_cast<std::uintmax_t>(childStatus.st_ino) << ':';
#if defined(__APPLE__)
    stream << static_cast<std::intmax_t>(childStatus.st_ctimespec.tv_sec) << ':'
           << static_cast<std::intmax_t>(childStatus.st_ctimespec.tv_nsec);
#else
    stream << static_cast<std::intmax_t>(childStatus.st_ctim.tv_sec) << ':'
           << static_cast<std::intmax_t>(childStatus.st_ctim.tv_nsec);
#endif
    return Result<std::string>::ok(stream.str());
}

std::mutex& durabilityCacheMutex() {
    static std::mutex mutex;
    return mutex;
}

std::unordered_set<std::string>& durabilityCache() {
    static std::unordered_set<std::string> cache;
    return cache;
}

bool directoryBoundaryWasSynced(const std::string& key) {
    std::lock_guard<std::mutex> lock(durabilityCacheMutex());
    return durabilityCache().find(key) != durabilityCache().end();
}

void rememberSyncedDirectoryBoundary(std::string key) {
    std::lock_guard<std::mutex> lock(durabilityCacheMutex());
    durabilityCache().insert(std::move(key));
}

int closeOnExecFlag() {
#ifdef O_CLOEXEC
    return O_CLOEXEC;
#else
    return 0;
#endif
}

int noFollowFlag() {
    return O_NOFOLLOW;
}

mode_t directoryPermissions(RootedDirectoryCreationMode mode) {
    return mode == RootedDirectoryCreationMode::InstalledContent
               ? static_cast<mode_t>(S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH)
               : static_cast<mode_t>(S_IRWXU);
}

Result<FileDescriptor> duplicateDescriptor(int source) {
#ifdef F_DUPFD_CLOEXEC
    const int duplicate = ::fcntl(source, F_DUPFD_CLOEXEC, 0);
#else
    const int duplicate = ::dup(source);
#endif
    if (duplicate < 0) {
        return posixFailureValue<FileDescriptor>("Failed to duplicate rooted directory descriptor");
    }
    return Result<FileDescriptor>::ok(FileDescriptor(duplicate));
}

Result<void> ensureDirectoryDescriptor(int descriptor, const std::string& context) {
    struct stat status {};
    if (::fstat(descriptor, &status) != 0) {
        return posixFailure("Failed to inspect " + context);
    }
    if (!S_ISDIR(status.st_mode)) {
        return Result<void>::fail({ErrorCode::FileSystemError, context + " is not a directory"});
    }
    return Result<void>::ok();
}

Result<void> ensureRegularDescriptor(int descriptor, const std::string& context) {
    struct stat status {};
    if (::fstat(descriptor, &status) != 0) {
        return posixFailure("Failed to inspect " + context);
    }
    if (!S_ISREG(status.st_mode)) {
        return Result<void>::fail({ErrorCode::PathTraversalRejected, context + " is not a regular file"});
    }
    return Result<void>::ok();
}

Result<void> ensureSingleLinkDescriptor(int descriptor, const std::string& context) {
    struct stat status {};
    if (::fstat(descriptor, &status) != 0) {
        return posixFailure("Failed to inspect " + context);
    }
    if (status.st_nlink != 1) {
        return Result<void>::fail({ErrorCode::SecurityPolicyViolation, context + " has multiple hard links"});
    }
    return Result<void>::ok();
}

Result<std::vector<std::string>> managedComponents(const std::string& relativePath) {
    auto valid = util::validateManagedPath(relativePath);
    if (!valid) {
        return Result<std::vector<std::string>>::fail(valid.error());
    }
    std::vector<std::string> components;
    std::size_t begin = 0;
    while (begin < relativePath.size()) {
        const auto separator = relativePath.find('/', begin);
        const auto end = separator == std::string::npos ? relativePath.size() : separator;
        components.push_back(relativePath.substr(begin, end - begin));
        if (separator == std::string::npos) {
            break;
        }
        begin = separator + 1;
    }
    if (components.empty()) {
        return Result<std::vector<std::string>>::fail({ErrorCode::PathTraversalRejected, "Managed path is empty"});
    }
    return Result<std::vector<std::string>>::ok(std::move(components));
}

Result<FileDescriptor> openChildDirectory(int parent, const std::string& name, bool create,
                                          RootedDirectoryCreationMode directoryMode) {
    bool created = false;
    int descriptor =
        ::openat(parent, name.c_str(), O_RDONLY | O_DIRECTORY | O_NONBLOCK | closeOnExecFlag() | noFollowFlag());
    if (descriptor < 0 && errno == ENOENT && create) {
        if (::mkdirat(parent, name.c_str(), directoryPermissions(directoryMode)) != 0) {
            if (errno != EEXIST) {
                return posixFailureValue<FileDescriptor>("Failed to create rooted directory component");
            }
        } else {
            created = true;
        }
        descriptor =
            ::openat(parent, name.c_str(), O_RDONLY | O_DIRECTORY | O_NONBLOCK | closeOnExecFlag() | noFollowFlag());
    }
    if (descriptor < 0) {
        if (errno == ENOENT && !create) {
            return Result<FileDescriptor>::ok(FileDescriptor());
        }
        if (errno == ELOOP) {
            return Result<FileDescriptor>::fail(
                {ErrorCode::PathTraversalRejected, "Managed directory contains a symbolic link"});
        }
        return posixFailureValue<FileDescriptor>("Failed to open rooted directory component");
    }
    FileDescriptor result(descriptor);
    auto checked = ensureDirectoryDescriptor(result.get(), "Managed directory");
    if (!checked) {
        return Result<FileDescriptor>::fail(checked.error());
    }
    if (created && ::fchmod(result.get(), directoryPermissions(directoryMode)) != 0) {
        return posixFailureValue<FileDescriptor>("Failed to secure rooted directory component");
    }
    auto durabilityKey = directoryDurabilityKey(parent, result.get());
    if (!durabilityKey) {
        return Result<FileDescriptor>::fail(durabilityKey.error());
    }
    if (create && (created || !directoryBoundaryWasSynced(durabilityKey.value()))) {
        auto childSynced = syncDirectory(result.get(), "writable rooted component");
        if (!childSynced) {
            return Result<FileDescriptor>::fail(childSynced.error());
        }
        auto parentSynced = syncDirectory(parent, "rooted component parent");
        if (!parentSynced) {
            return Result<FileDescriptor>::fail(parentSynced.error());
        }
        rememberSyncedDirectoryBoundary(std::move(durabilityKey.value()));
    }
    return Result<FileDescriptor>::ok(std::move(result));
}

Result<FileDescriptor> traverseParent(int root, const std::vector<std::string>& components, bool create,
                                      RootedDirectoryCreationMode directoryMode) {
    auto currentResult = duplicateDescriptor(root);
    if (!currentResult) {
        return currentResult;
    }
    auto current = std::move(currentResult.value());
    for (std::size_t index = 0; index + 1 < components.size(); ++index) {
        auto next = openChildDirectory(current.get(), components[index], create, directoryMode);
        if (!next) {
            return next;
        }
        if (!next.value()) {
            return Result<FileDescriptor>::ok(FileDescriptor());
        }
        current = std::move(next.value());
    }
    return Result<FileDescriptor>::ok(std::move(current));
}

std::string identityFromStat(const struct stat& status) {
    std::ostringstream stream;
    stream << std::hex << static_cast<std::uintmax_t>(status.st_dev) << ':'
           << static_cast<std::uintmax_t>(status.st_ino);
    return stream.str();
}

Result<bool> sameDirectory(int left, int right) {
    struct stat leftStatus {};
    struct stat rightStatus {};
    if (::fstat(left, &leftStatus) != 0 || ::fstat(right, &rightStatus) != 0) {
        return posixFailureValue<bool>("Failed to compare rooted directory identity");
    }
    return Result<bool>::ok(leftStatus.st_dev == rightStatus.st_dev && leftStatus.st_ino == rightStatus.st_ino);
}

Result<void> syncNamespaceMutation(int primaryParent, int secondaryParent, const std::string& context) {
    auto primarySynced = syncDirectory(primaryParent, context + " primary parent");
    if (!primarySynced) {
        return primarySynced;
    }
    auto sameParent = sameDirectory(primaryParent, secondaryParent);
    if (!sameParent) {
        return Result<void>::fail(sameParent.error());
    }
    if (sameParent.value()) {
        return Result<void>::ok();
    }
    return syncDirectory(secondaryParent, context + " secondary parent");
}

bool sameIdentity(const struct stat& left, const struct stat& right) noexcept {
    return left.st_dev == right.st_dev && left.st_ino == right.st_ino;
}

Result<struct stat> descriptorStatus(int descriptor, const std::string& context) {
    struct stat status {};
    if (::fstat(descriptor, &status) != 0) {
        return posixFailureValue<struct stat>("Failed to inspect " + context);
    }
    return Result<struct stat>::ok(status);
}

Result<struct stat> entryStatus(int parent, const std::string& name, const std::string& context) {
    struct stat status {};
    if (::fstatat(parent, name.c_str(), &status, AT_SYMLINK_NOFOLLOW) != 0) {
        return posixFailureValue<struct stat>("Failed to inspect " + context);
    }
    return Result<struct stat>::ok(status);
}

Result<void> verifyEntryIdentity(int parent, const std::string& name, int descriptor, const std::string& context) {
    auto opened = descriptorStatus(descriptor, context + " descriptor");
    if (!opened) {
        return Result<void>::fail(opened.error());
    }
    auto named = entryStatus(parent, name, context + " name");
    if (!named) {
        return Result<void>::fail(named.error());
    }
    if (!S_ISREG(named.value().st_mode) || !sameIdentity(opened.value(), named.value())) {
        return Result<void>::fail(
            {ErrorCode::SecurityPolicyViolation, context + " name no longer refers to the opened file"});
    }
    return Result<void>::ok();
}

Result<void> verifyEntryIdentity(int parent, const std::string& name, const std::string& identity,
                                 const std::string& context) {
    auto named = entryStatus(parent, name, context);
    if (!named) {
        return Result<void>::fail(named.error());
    }
    if (!S_ISREG(named.value().st_mode) || identityFromStat(named.value()) != identity) {
        return Result<void>::fail({ErrorCode::SecurityPolicyViolation, context + " identity changed"});
    }
    return Result<void>::ok();
}

Result<void> removeEntryWithIdentity(int parent, const std::string& name, const std::string& identity,
                                     const std::string& context, bool* removed = nullptr) {
    if (removed) {
        *removed = false;
    }
    struct stat status {};
    if (::fstatat(parent, name.c_str(), &status, AT_SYMLINK_NOFOLLOW) != 0) {
        if (errno == ENOENT) {
            return syncDirectory(parent, context + " parent");
        }
        return posixFailure("Failed to inspect " + context);
    }
    if (!S_ISREG(status.st_mode) || identityFromStat(status) != identity) {
        return Result<void>::fail({ErrorCode::SecurityPolicyViolation, context + " identity changed before cleanup"});
    }
    if (::unlinkat(parent, name.c_str(), 0) != 0) {
        return posixFailure("Failed to remove " + context);
    }
    if (removed) {
        *removed = true;
    }
    return syncDirectory(parent, context + " parent");
}

Result<void> renameNoReplace(int sourceParent, const std::string& sourceName, int targetParent,
                             const std::string& targetName, const std::string& context, bool* renamed = nullptr) {
    if (renamed) {
        *renamed = false;
    }
#if defined(__linux__)
#if defined(SYS_renameat2)
    const long result = ::syscall(SYS_renameat2, sourceParent, sourceName.c_str(), targetParent, targetName.c_str(),
                                  static_cast<unsigned int>(RENAME_NOREPLACE));
#elif defined(__NR_renameat2)
    const long result = ::syscall(__NR_renameat2, sourceParent, sourceName.c_str(), targetParent, targetName.c_str(),
                                  static_cast<unsigned int>(RENAME_NOREPLACE));
#else
    const long result = -1;
    errno = ENOTSUP;
#endif
    if (result == 0) {
        if (renamed) {
            *renamed = true;
        }
        return syncNamespaceMutation(targetParent, sourceParent, context);
    }
#elif defined(__APPLE__)
    if (::renameatx_np(sourceParent, sourceName.c_str(), targetParent, targetName.c_str(), RENAME_EXCL) == 0) {
        if (renamed) {
            *renamed = true;
        }
        return syncNamespaceMutation(targetParent, sourceParent, context);
    }
#else
    (void)sourceParent;
    (void)sourceName;
    (void)targetParent;
    (void)targetName;
    (void)context;
    return Result<void>::fail(
        {ErrorCode::SecurityPolicyViolation, "Atomic no-replace rename is not supported on this POSIX platform"});
#endif
    const int code = errno;
    if (code == ENOSYS || code == ENOTSUP || code == EINVAL) {
        return Result<void>::fail(
            {ErrorCode::SecurityPolicyViolation, context + " requires atomic no-replace rename support"});
    }
    if (code == EEXIST || code == ENOTEMPTY) {
        return Result<void>::fail({ErrorCode::SecurityPolicyViolation, context + " destination already exists"});
    }
    return posixFailure(context, code);
}

Result<void> exchangeEntries(int leftParent, const std::string& leftName, int rightParent, const std::string& rightName,
                             const std::string& context, bool* exchanged = nullptr) {
    if (exchanged) {
        *exchanged = false;
    }
#if defined(__linux__)
#if defined(SYS_renameat2)
    const long result = ::syscall(SYS_renameat2, leftParent, leftName.c_str(), rightParent, rightName.c_str(),
                                  static_cast<unsigned int>(RENAME_EXCHANGE));
#elif defined(__NR_renameat2)
    const long result = ::syscall(__NR_renameat2, leftParent, leftName.c_str(), rightParent, rightName.c_str(),
                                  static_cast<unsigned int>(RENAME_EXCHANGE));
#else
    const long result = -1;
    errno = ENOTSUP;
#endif
    if (result == 0) {
        if (exchanged) {
            *exchanged = true;
        }
        return syncNamespaceMutation(rightParent, leftParent, context);
    }
#elif defined(__APPLE__)
    if (::renameatx_np(leftParent, leftName.c_str(), rightParent, rightName.c_str(), RENAME_SWAP) == 0) {
        if (exchanged) {
            *exchanged = true;
        }
        return syncNamespaceMutation(rightParent, leftParent, context);
    }
#else
    (void)leftParent;
    (void)leftName;
    (void)rightParent;
    (void)rightName;
    (void)context;
    return Result<void>::fail(
        {ErrorCode::SecurityPolicyViolation, "Atomic exchange rename is not supported on this POSIX platform"});
#endif
    const int code = errno;
    if (code == ENOSYS || code == ENOTSUP || code == EINVAL) {
        return Result<void>::fail(
            {ErrorCode::SecurityPolicyViolation, context + " requires atomic exchange rename support"});
    }
    return posixFailure(context, code);
}

std::string privateNamespaceName() {
    static std::atomic<std::uint64_t> sequence{0};
    std::random_device random;
    std::string seed = std::to_string(::getpid()) + ":" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ":" +
                       std::to_string(sequence.fetch_add(1, std::memory_order_relaxed)) + ":" +
                       std::to_string(random()) + ":" + std::to_string(random());
    return ".autoupdater-private-" + util::sha256Bytes(seed).substr(0, 32);
}

constexpr const char* privatePayloadName = "payload";

class PrivateNamespace {
  public:
    PrivateNamespace() = default;
    PrivateNamespace(FileDescriptor parent, FileDescriptor directory, std::string name, std::string identity)
        : parent_(std::move(parent)), directory_(std::move(directory)), name_(std::move(name)),
          identity_(std::move(identity)) {}
    ~PrivateNamespace() {
        cleanupDirectoryNoThrow();
    }

    PrivateNamespace(const PrivateNamespace&) = delete;
    PrivateNamespace& operator=(const PrivateNamespace&) = delete;
    PrivateNamespace(PrivateNamespace&& other) noexcept
        : parent_(std::move(other.parent_)), directory_(std::move(other.directory_)), name_(std::move(other.name_)),
          identity_(std::move(other.identity_)), cleaned_(other.cleaned_) {
        other.cleaned_ = true;
    }
    PrivateNamespace& operator=(PrivateNamespace&& other) noexcept {
        if (this != &other) {
            cleanupDirectoryNoThrow();
            parent_ = std::move(other.parent_);
            directory_ = std::move(other.directory_);
            name_ = std::move(other.name_);
            identity_ = std::move(other.identity_);
            cleaned_ = other.cleaned_;
            other.cleaned_ = true;
        }
        return *this;
    }

    int parentDescriptor() const noexcept {
        return parent_.get();
    }
    int descriptor() const noexcept {
        return directory_.get();
    }
    const std::string& name() const noexcept {
        return name_;
    }

    Result<void> cleanupDirectory() noexcept {
        if (cleaned_ || !parent_ || !directory_) {
            return Result<void>::ok();
        }
        struct stat opened {};
        struct stat named {};
        if (::fstat(directory_.get(), &opened) != 0) {
            return posixFailure("Failed to inspect private temporary namespace");
        }
        if (::fstatat(parent_.get(), name_.c_str(), &named, AT_SYMLINK_NOFOLLOW) != 0) {
            if (errno == ENOENT) {
                auto synced = syncDirectory(parent_.get(), "private temporary namespace parent");
                if (synced) {
                    cleaned_ = true;
                }
                return synced;
            }
            return posixFailure("Failed to inspect private temporary namespace name");
        }
        if (!S_ISDIR(named.st_mode) || !sameIdentity(opened, named) || identityFromStat(named) != identity_) {
            return Result<void>::fail(
                {ErrorCode::SecurityPolicyViolation, "Private temporary namespace name was replaced"});
        }
        if (::unlinkat(parent_.get(), name_.c_str(), AT_REMOVEDIR) != 0) {
            return posixFailure("Failed to remove private temporary namespace");
        }
        auto synced = syncDirectory(parent_.get(), "private temporary namespace parent");
        if (synced) {
            cleaned_ = true;
        }
        return synced;
    }

    Result<void> close() noexcept {
        auto directoryClosed = directory_.close();
        auto parentClosed = parent_.close();
        if (!directoryClosed) {
            auto error = directoryClosed.error();
            if (!parentClosed) {
                error.message += "; failed to close temporary namespace parent: " + parentClosed.error().message;
            }
            return Result<void>::fail(std::move(error));
        }
        return parentClosed;
    }

  private:
    void cleanupDirectoryNoThrow() noexcept {
        if (!cleaned_) {
            (void)cleanupDirectory();
        }
    }

    FileDescriptor parent_;
    FileDescriptor directory_;
    std::string name_;
    std::string identity_;
    bool cleaned_ = false;
};

Result<PrivateNamespace> createPrivateNamespace(FileDescriptor parent) {
    for (int attempt = 0; attempt < 32; ++attempt) {
        const auto name = privateNamespaceName();
        if (::mkdirat(parent.get(), name.c_str(), S_IRWXU) != 0) {
            if (errno == EEXIST) {
                continue;
            }
            return posixFailureValue<PrivateNamespace>("Failed to create private temporary namespace");
        }

        const int descriptor = ::openat(parent.get(), name.c_str(),
                                        O_RDONLY | O_DIRECTORY | O_NONBLOCK | closeOnExecFlag() | noFollowFlag());
        if (descriptor < 0) {
            return posixFailureValue<PrivateNamespace>("Failed to open private temporary namespace");
        }
        FileDescriptor directory(descriptor);
        if (::fchmod(directory.get(), S_IRWXU) != 0) {
            return posixFailureValue<PrivateNamespace>("Failed to secure private temporary namespace");
        }
        struct stat opened {};
        struct stat named {};
        if (::fstat(directory.get(), &opened) != 0 ||
            ::fstatat(parent.get(), name.c_str(), &named, AT_SYMLINK_NOFOLLOW) != 0) {
            return posixFailureValue<PrivateNamespace>("Failed to verify private temporary namespace");
        }
        if (!S_ISDIR(opened.st_mode) || !S_ISDIR(named.st_mode) || !sameIdentity(opened, named) ||
            opened.st_uid != ::geteuid()) {
            return Result<PrivateNamespace>::fail(
                {ErrorCode::SecurityPolicyViolation, "Private temporary namespace was replaced while opening"});
        }
        PrivateNamespace temporaryNamespace(std::move(parent), std::move(directory), name, identityFromStat(opened));
        auto directorySynced = syncDirectory(temporaryNamespace.descriptor(), "new private temporary namespace");
        if (!directorySynced) {
            return Result<PrivateNamespace>::fail(directorySynced.error());
        }
        auto parentSynced = syncDirectory(temporaryNamespace.parentDescriptor(), "private temporary namespace parent");
        if (!parentSynced) {
            return Result<PrivateNamespace>::fail(parentSynced.error());
        }
        return Result<PrivateNamespace>::ok(std::move(temporaryNamespace));
    }
    return Result<PrivateNamespace>::fail(
        {ErrorCode::FileSystemError, "Failed to allocate a unique private temporary namespace"});
}

class PosixRootedDirectory;

class PosixRootedFile final : public IRootedFile {
  public:
    PosixRootedFile(PosixRootedDirectory* owner, FileDescriptor file, FileDescriptor parent, std::string name,
                    bool deleteOnDestroy = false, bool protectWrites = false)
        : owner_(owner), file_(std::move(file)), parent_(std::move(parent)), name_(std::move(name)),
          deleteOnDestroy_(deleteOnDestroy), protectWrites_(protectWrites) {}

    ~PosixRootedFile() override {
        if (deleteOnDestroy_ && parent_) {
            auto verified = verifyEntryIdentity(parent_.get(), name_, file_.get(), "Temporary managed file");
            if (verified) {
                (void)::unlinkat(parent_.get(), name_.c_str(), 0);
            }
        }
    }

    Result<std::size_t> read(void* buffer, std::size_t size) noexcept override {
        for (;;) {
            const auto count = ::read(file_.get(), buffer, size);
            if (count < 0 && errno == EINTR) {
                continue;
            }
            if (count < 0) {
                return posixFailureValue<std::size_t>("Failed to read rooted file");
            }
            return Result<std::size_t>::ok(static_cast<std::size_t>(count));
        }
    }

    Result<void> write(const void* data, std::size_t size) noexcept override {
        if (protectWrites_) {
            auto singleLink = ensureSingleLinkDescriptor(file_.get(), "Writable managed file");
            if (!singleLink) {
                return singleLink;
            }
        }
        const auto* bytes = static_cast<const unsigned char*>(data);
        std::size_t offset = 0;
        while (offset < size) {
            const auto count = ::write(file_.get(), bytes + offset, size - offset);
            if (count < 0 && errno == EINTR) {
                continue;
            }
            if (count <= 0) {
                return posixFailure("Failed to write rooted file");
            }
            offset += static_cast<std::size_t>(count);
        }
        return Result<void>::ok();
    }

    Result<void> seek(std::uint64_t offset) noexcept override {
        if (offset > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()) ||
            ::lseek(file_.get(), static_cast<off_t>(offset), SEEK_SET) < 0) {
            return posixFailure("Failed to seek rooted file");
        }
        return Result<void>::ok();
    }

    Result<void> truncate(std::uint64_t size) noexcept override {
        if (protectWrites_) {
            auto singleLink = ensureSingleLinkDescriptor(file_.get(), "Writable managed file");
            if (!singleLink) {
                return singleLink;
            }
        }
        if (size > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()) ||
            ::ftruncate(file_.get(), static_cast<off_t>(size)) != 0) {
            return posixFailure("Failed to truncate rooted file");
        }
        return Result<void>::ok();
    }

    Result<void> flush() noexcept override {
        if (protectWrites_) {
            auto singleLink = ensureSingleLinkDescriptor(file_.get(), "Writable managed file");
            if (!singleLink) {
                return singleLink;
            }
        }
        return syncFileDescriptor(file_.get(), "Failed to flush rooted file");
    }

    Result<RootedFileMetadata> metadata() noexcept override {
        struct stat status {};
        if (::fstat(file_.get(), &status) != 0 || status.st_size < 0) {
            return posixFailureValue<RootedFileMetadata>("Failed to read rooted file metadata");
        }
        RootedFileMetadata result;
        result.size = static_cast<std::uint64_t>(status.st_size);
        result.permissions = static_cast<std::filesystem::perms>(status.st_mode & 07777);
        result.identity = identityFromStat(status);
        return Result<RootedFileMetadata>::ok(std::move(result));
    }

    Result<void> setPermissions(std::filesystem::perms permissions) noexcept override {
        if (protectWrites_) {
            auto singleLink = ensureSingleLinkDescriptor(file_.get(), "Writable managed file");
            if (!singleLink) {
                return singleLink;
            }
        }
        const auto mode = static_cast<mode_t>(permissions) & 07777;
        if (::fchmod(file_.get(), mode) != 0) {
            return posixFailure("Failed to set rooted file permissions");
        }
        return Result<void>::ok();
    }

    Result<void> close() noexcept override {
        auto fileClosed = file_.close();
        auto parentClosed = parent_.close();
        if (!fileClosed) {
            auto error = fileClosed.error();
            if (!parentClosed) {
                error.message += "; failed to close rooted file parent: " + parentClosed.error().message;
            }
            return Result<void>::fail(std::move(error));
        }
        return parentClosed;
    }

    PosixRootedDirectory* owner() const noexcept {
        return owner_;
    }
    int descriptor() const noexcept {
        return file_.get();
    }
    int parentDescriptor() const noexcept {
        return parent_.get();
    }
    const std::string& name() const noexcept {
        return name_;
    }
    void markCommitted() noexcept {
        deleteOnDestroy_ = false;
    }
    void markTemporary() noexcept {
        deleteOnDestroy_ = true;
    }

  private:
    PosixRootedDirectory* owner_ = nullptr;
    FileDescriptor file_;
    FileDescriptor parent_;
    std::string name_;
    bool deleteOnDestroy_ = false;
    bool protectWrites_ = false;
};

class PosixTemporaryFile final : public IRootedTemporaryFile {
  public:
    PosixTemporaryFile(PosixRootedDirectory& root, std::unique_ptr<PosixRootedFile> file, std::string target,
                       PrivateNamespace temporaryNamespace, std::string payloadIdentity)
        : root_(root), file_(std::move(file)), target_(std::move(target)),
          temporaryNamespace_(std::move(temporaryNamespace)), payloadIdentity_(std::move(payloadIdentity)) {}
    ~PosixTemporaryFile() override {
        (void)discard();
    }

    IRootedFile& file() noexcept override {
        return *file_;
    }
    Result<void> commit(const RootedEntryExpectation& expectation) noexcept override;
    RootedPublishStatus publishStatus() const noexcept override {
        return publishStatus_;
    }
    Result<void> discard() noexcept override {
        Error primary;
        bool failed = false;
        const auto recordFailure = [&](const std::string& context, const Error& error) {
            if (!failed) {
                primary = error;
                failed = true;
            } else {
                primary.message += "; " + context + ": " + error.message;
            }
        };

        if (payloadPresent_) {
            bool removed = false;
            auto payloadRemoved = removeEntryWithIdentity(temporaryNamespace_.descriptor(), privatePayloadName,
                                                          payloadIdentity_, "private temporary payload", &removed);
            if (removed || payloadRemoved) {
                payloadPresent_ = false;
                file_->markCommitted();
            }
            if (!payloadRemoved) {
                recordFailure("failed to discard private temporary payload", payloadRemoved.error());
            }
        }

        auto namespaceCleaned = temporaryNamespace_.cleanupDirectory();
        if (!namespaceCleaned) {
            recordFailure("failed to discard private temporary namespace", namespaceCleaned.error());
        } else if (publishStatus_.publication != RootedPublication::Unknown) {
            publishStatus_.namespaceDurable = true;
        }
        auto fileClosed = file_->close();
        if (!fileClosed) {
            recordFailure("failed to close private temporary payload", fileClosed.error());
        }
        if (namespaceCleaned) {
            auto namespaceClosed = temporaryNamespace_.close();
            if (!namespaceClosed) {
                recordFailure("failed to close private temporary namespace", namespaceClosed.error());
            }
        }
        return failed ? Result<void>::fail(std::move(primary)) : Result<void>::ok();
    }

    PosixRootedFile& rootedFile() noexcept {
        return *file_;
    }
    const std::string& target() const noexcept {
        return target_;
    }
    int namespaceDescriptor() const noexcept {
        return temporaryNamespace_.descriptor();
    }
    int namespaceParentDescriptor() const noexcept {
        return temporaryNamespace_.parentDescriptor();
    }
    void swapPayloadIdentity(std::string& identity) noexcept {
        payloadIdentity_.swap(identity);
        payloadPresent_ = true;
    }
    void clearPayload() noexcept {
        payloadPresent_ = false;
    }
    void restorePayload() noexcept {
        payloadPresent_ = true;
    }
    void markPublished(bool durable) noexcept {
        publishStatus_ = {RootedPublication::Published, durable, !durable};
        file_->markCommitted();
    }
    void markRestored(bool durable) noexcept {
        publishStatus_ = {RootedPublication::NotPublished, durable, false};
        file_->markTemporary();
    }
    void markUnknown() noexcept {
        publishStatus_ = {RootedPublication::Unknown, false, false};
        file_->markCommitted();
    }
    void markFailureReconcilable() noexcept {
        if (publishStatus_.publication != RootedPublication::NotPublished) {
            publishStatus_.failureCanBeReconciled = true;
        }
    }
    Result<void> cleanupNamespace() noexcept {
        return temporaryNamespace_.cleanupDirectory();
    }

  private:
    PosixRootedDirectory& root_;
    std::unique_ptr<PosixRootedFile> file_;
    std::string target_;
    PrivateNamespace temporaryNamespace_;
    std::string payloadIdentity_;
    bool payloadPresent_ = true;
    RootedPublishStatus publishStatus_;
};

Result<void> copyOpenedFile(IRootedFile& source, IRootedFile& target) {
    auto sourceStart = source.seek(0);
    if (!sourceStart) {
        return sourceStart;
    }
    auto targetStart = target.seek(0);
    if (!targetStart) {
        return targetStart;
    }
    auto cleared = target.truncate(0);
    if (!cleared) {
        return cleared;
    }
    std::array<unsigned char, 64 * 1024> buffer{};
    for (;;) {
        auto read = source.read(buffer.data(), buffer.size());
        if (!read) {
            return Result<void>::fail(read.error());
        }
        if (read.value() == 0) {
            break;
        }
        auto written = target.write(buffer.data(), read.value());
        if (!written) {
            return written;
        }
    }
    auto sourceReset = source.seek(0);
    if (!sourceReset) {
        return sourceReset;
    }
    auto targetReset = target.seek(0);
    if (!targetReset) {
        return targetReset;
    }
    return Result<void>::ok();
}

class PosixLock final : public IRootedLock {
  public:
    explicit PosixLock(FileDescriptor file) : file_(std::move(file)) {}
    ~PosixLock() override = default;

  private:
    FileDescriptor file_;
};

class PosixRootedDirectory final : public IRootedDirectory {
  public:
    PosixRootedDirectory(FileDescriptor root, RootAccess access) : root_(std::move(root)), access_(access) {}

    Result<RootedOpenResult> openRegularFile(const std::string& relativePath, RootedFileOpenMode mode,
                                             RootedDirectoryCreationMode directoryMode) noexcept override {
        try {
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
            auto parent = traverseParent(root_.get(), components.value(), creating, directoryMode);
            if (!parent) {
                return Result<RootedOpenResult>::fail(parent.error());
            }
            if (!parent.value()) {
                return Result<RootedOpenResult>::ok({});
            }

            int flags = (writing ? O_RDWR : O_RDONLY) | O_NONBLOCK | closeOnExecFlag() | noFollowFlag();
            if (creating) {
                flags |= O_CREAT;
            }
            const int descriptor =
                ::openat(parent.value().get(), components.value().back().c_str(), flags, S_IRUSR | S_IWUSR);
            if (descriptor < 0) {
                if (errno == ENOENT && !creating) {
                    return Result<RootedOpenResult>::ok({});
                }
                if (errno == ELOOP) {
                    return Result<RootedOpenResult>::fail(
                        {ErrorCode::PathTraversalRejected, "Managed file is a symbolic link"});
                }
                return posixFailureValue<RootedOpenResult>("Failed to open rooted regular file");
            }
            FileDescriptor file(descriptor);
            auto checked = ensureRegularDescriptor(file.get(), "Managed file");
            if (!checked) {
                return Result<RootedOpenResult>::fail(checked.error());
            }
            if (writing) {
                auto singleLink = ensureSingleLinkDescriptor(file.get(), "Writable managed file");
                if (!singleLink) {
                    return Result<RootedOpenResult>::fail(singleLink.error());
                }
            }
            auto result = std::make_unique<PosixRootedFile>(this, std::move(file), std::move(parent.value()),
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
            if (access_ != RootAccess::ReadWrite) {
                return Result<std::unique_ptr<IRootedTemporaryFile>>::fail(
                    {ErrorCode::SecurityPolicyViolation, "Rooted directory is read-only"});
            }
            auto components = managedComponents(relativePath);
            if (!components) {
                return Result<std::unique_ptr<IRootedTemporaryFile>>::fail(components.error());
            }
            auto parent = traverseParent(root_.get(), components.value(), true, directoryMode);
            if (!parent) {
                return Result<std::unique_ptr<IRootedTemporaryFile>>::fail(parent.error());
            }
            auto temporaryNamespace = createPrivateNamespace(std::move(parent.value()));
            if (!temporaryNamespace) {
                return Result<std::unique_ptr<IRootedTemporaryFile>>::fail(temporaryNamespace.error());
            }
            const int descriptor = ::openat(temporaryNamespace.value().descriptor(), privatePayloadName,
                                            O_RDWR | O_CREAT | O_EXCL | O_NONBLOCK | closeOnExecFlag() | noFollowFlag(),
                                            S_IRUSR | S_IWUSR);
            if (descriptor < 0) {
                return posixFailureValue<std::unique_ptr<IRootedTemporaryFile>>(
                    "Failed to create private rooted temporary payload");
            }
            FileDescriptor payload(descriptor);
            auto regular = ensureRegularDescriptor(payload.get(), "Private temporary payload");
            if (!regular) {
                return Result<std::unique_ptr<IRootedTemporaryFile>>::fail(regular.error());
            }
            auto singleLink = ensureSingleLinkDescriptor(payload.get(), "Private temporary payload");
            if (!singleLink) {
                return Result<std::unique_ptr<IRootedTemporaryFile>>::fail(singleLink.error());
            }
            auto payloadStatus = descriptorStatus(payload.get(), "private temporary payload");
            if (!payloadStatus) {
                return Result<std::unique_ptr<IRootedTemporaryFile>>::fail(payloadStatus.error());
            }
            auto payloadParent = duplicateDescriptor(temporaryNamespace.value().descriptor());
            if (!payloadParent) {
                return Result<std::unique_ptr<IRootedTemporaryFile>>::fail(payloadParent.error());
            }
            const auto payloadIdentity = identityFromStat(payloadStatus.value());
            auto rootedFile = std::make_unique<PosixRootedFile>(
                this, std::move(payload), std::move(payloadParent.value()), privatePayloadName, true, true);
            auto temporary = std::make_unique<PosixTemporaryFile>(
                *this, std::move(rootedFile), relativePath, std::move(temporaryNamespace.value()), payloadIdentity);
            return Result<std::unique_ptr<IRootedTemporaryFile>>::ok(std::move(temporary));
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
            auto* file = dynamic_cast<PosixRootedFile*>(&source);
            if (!file || file->owner() != this) {
                return Result<void>::fail(
                    {ErrorCode::SecurityPolicyViolation, "Rooted source belongs to a different root"});
            }
            auto singleLink = ensureSingleLinkDescriptor(file->descriptor(), "Atomic replacement source");
            if (!singleLink) {
                return singleLink;
            }
            auto components = managedComponents(relativePath);
            if (!components) {
                return Result<void>::fail(components.error());
            }
            auto targetParent =
                traverseParent(root_.get(), components.value(), false, RootedDirectoryCreationMode::Private);
            if (!targetParent) {
                return Result<void>::fail(targetParent.error());
            }
            if (!targetParent.value()) {
                return Result<void>::fail(
                    {ErrorCode::SecurityPolicyViolation, "Managed target parent changed before commit"});
            }
            auto sameParent = sameDirectory(file->parentDescriptor(), targetParent.value().get());
            if (!sameParent) {
                return Result<void>::fail(sameParent.error());
            }
            if (sameParent.value() && file->name() == components.value().back()) {
                return Result<void>::fail(
                    {ErrorCode::SecurityPolicyViolation, "Atomic replacement source and target are the same entry"});
            }

            auto sourceMetadata = file->metadata();
            if (!sourceMetadata) {
                return Result<void>::fail(sourceMetadata.error());
            }
            auto temporary = createAtomicReplacement(relativePath, RootedDirectoryCreationMode::Private);
            if (!temporary) {
                return Result<void>::fail(temporary.error());
            }
            const auto failTemporary = [&](Error error, const std::string& context) {
                auto discarded = temporary.value()->discard();
                if (!discarded) {
                    error.message += "; " + context + ": " + discarded.error().message;
                }
                return Result<void>::fail(std::move(error));
            };
            auto copied = copyOpenedFile(source, temporary.value()->file());
            if (!copied) {
                return failTemporary(copied.error(), "failed to discard incomplete rooted replacement");
            }
            constexpr auto permissionMask = std::filesystem::perms::owner_all | std::filesystem::perms::group_all |
                                            std::filesystem::perms::others_all;
            auto permissions =
                temporary.value()->file().setPermissions(sourceMetadata.value().permissions & permissionMask);
            if (!permissions) {
                return failTemporary(permissions.error(),
                                     "failed to discard rooted replacement after permission error");
            }
            auto committed = temporary.value()->commit(expectation);
            if (!committed) {
                return failTemporary(committed.error(), "failed to finish rooted replacement cleanup");
            }
            auto discarded = temporary.value()->discard();
            if (!discarded) {
                return discarded;
            }
            return quarantineAndRemoveOpenedFile(*file, RootedEntryExpectation::matching(sourceMetadata.value()));
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
            auto opened =
                openRegularFile(relativePath, RootedFileOpenMode::ReadOnly, RootedDirectoryCreationMode::Private);
            if (!opened) {
                return Result<void>::fail(opened.error());
            }
            if (!opened.value().exists()) {
                if (expectation.kind == RootedEntryExpectationKind::Missing) {
                    auto parent =
                        traverseParent(root_.get(), components.value(), false, RootedDirectoryCreationMode::Private);
                    if (!parent) {
                        return Result<void>::fail(parent.error());
                    }
                    if (!parent.value()) {
                        return Result<void>::ok();
                    }
                    return syncDirectory(parent.value().get(), "managed remove target parent");
                }
                return Result<void>::fail({ErrorCode::SecurityPolicyViolation, "Managed remove target disappeared"});
            }
            auto* file = dynamic_cast<PosixRootedFile*>(opened.value().file.get());
            const auto finishOpened = [&](Result<void> operation) {
                auto closed = opened.value().file->close();
                if (!operation) {
                    auto error = operation.error();
                    if (!closed) {
                        error.message += "; failed to close managed remove target: " + closed.error().message;
                    }
                    return Result<void>::fail(std::move(error));
                }
                return closed;
            };
            if (!file) {
                return finishOpened(Result<void>::fail(
                    {ErrorCode::InternalError, "Rooted remove target has an unexpected implementation"}));
            }
            auto metadata = file->metadata();
            if (!metadata) {
                return finishOpened(Result<void>::fail(metadata.error()));
            }
            if (expectation.kind != RootedEntryExpectationKind::Identity ||
                metadata.value().identity != expectation.identity) {
                return finishOpened(
                    Result<void>::fail({ErrorCode::SecurityPolicyViolation, "Managed remove target identity changed"}));
            }
            return finishOpened(quarantineAndRemoveOpenedFile(*file, expectation));
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
            auto parent = traverseParent(root_.get(), components.value(), true, RootedDirectoryCreationMode::Private);
            if (!parent) {
                return Result<std::unique_ptr<IRootedLock>>::fail(parent.error());
            }
            const auto& lockName = components.value().back();
            const int descriptor =
                ::openat(parent.value().get(), lockName.c_str(),
                         O_RDWR | O_CREAT | O_NONBLOCK | closeOnExecFlag() | noFollowFlag(), S_IRUSR | S_IWUSR);
            if (descriptor < 0) {
                if (errno == EISDIR) {
                    return Result<std::unique_ptr<IRootedLock>>::fail(
                        {ErrorCode::ApplyFailed, "Another update appears to be running"});
                }
                if (errno == ELOOP) {
                    return Result<std::unique_ptr<IRootedLock>>::fail(
                        {ErrorCode::PathTraversalRejected, "Update lock is a symbolic link"});
                }
                return posixFailureValue<std::unique_ptr<IRootedLock>>("Failed to open update lock file");
            }
            FileDescriptor lockFile(descriptor);
            auto regular = ensureRegularDescriptor(lockFile.get(), "Update lock");
            if (!regular) {
                return Result<std::unique_ptr<IRootedLock>>::fail(regular.error());
            }
            auto singleLink = ensureSingleLinkDescriptor(lockFile.get(), "Update lock");
            if (!singleLink) {
                return Result<std::unique_ptr<IRootedLock>>::fail(singleLink.error());
            }
            auto lockStatus = descriptorStatus(lockFile.get(), "update lock");
            if (!lockStatus) {
                return Result<std::unique_ptr<IRootedLock>>::fail(lockStatus.error());
            }
            if (lockStatus.value().st_uid != ::geteuid()) {
                return Result<std::unique_ptr<IRootedLock>>::fail(
                    {ErrorCode::SecurityPolicyViolation, "Update lock is not owned by the current user"});
            }
            for (;;) {
                if (::flock(lockFile.get(), LOCK_EX | LOCK_NB) == 0) {
                    break;
                }
                const int code = errno;
                if (code == EINTR) {
                    continue;
                }
                if (code == EWOULDBLOCK || code == EAGAIN) {
                    return Result<std::unique_ptr<IRootedLock>>::fail(
                        {ErrorCode::ApplyFailed, "Another update appears to be running"});
                }
                return posixFailureValue<std::unique_ptr<IRootedLock>>("Failed to acquire update lock", code);
            }
            auto namedLock = verifyEntryIdentity(parent.value().get(), lockName, lockFile.get(), "Update lock");
            if (!namedLock) {
                return Result<std::unique_ptr<IRootedLock>>::fail(namedLock.error());
            }
            if (::fchmod(lockFile.get(), S_IRUSR | S_IWUSR) != 0) {
                return posixFailureValue<std::unique_ptr<IRootedLock>>("Failed to secure update lock file");
            }
            auto lockSynced = syncFileDescriptor(lockFile.get(), "Failed to persist update lock file");
            if (!lockSynced) {
                return Result<std::unique_ptr<IRootedLock>>::fail(lockSynced.error());
            }
            auto parentSynced = syncDirectory(parent.value().get(), "update lock parent");
            if (!parentSynced) {
                return Result<std::unique_ptr<IRootedLock>>::fail(parentSynced.error());
            }
            auto result = std::make_unique<PosixLock>(std::move(lockFile));
            return Result<std::unique_ptr<IRootedLock>>::ok(std::move(result));
        } catch (...) {
            return Result<std::unique_ptr<IRootedLock>>::fail(
                {ErrorCode::FileSystemError, "Unexpected rooted lock failure"});
        }
    }

    Result<void> commitPrivateTemporary(PosixTemporaryFile& temporary,
                                        const RootedEntryExpectation& expectation) noexcept;
    Result<void> quarantineAndRemoveOpenedFile(PosixRootedFile& file,
                                               const RootedEntryExpectation& expectation) noexcept;

  private:
    FileDescriptor root_;
    RootAccess access_ = RootAccess::ReadOnly;
};

Result<void> PosixRootedDirectory::commitPrivateTemporary(PosixTemporaryFile& temporary,
                                                          const RootedEntryExpectation& expectation) noexcept {
    try {
        auto components = managedComponents(temporary.target());
        if (!components) {
            return Result<void>::fail(components.error());
        }
        auto currentParent =
            traverseParent(root_.get(), components.value(), false, RootedDirectoryCreationMode::Private);
        if (!currentParent) {
            return Result<void>::fail(currentParent.error());
        }
        if (!currentParent.value()) {
            return Result<void>::fail(
                {ErrorCode::SecurityPolicyViolation, "Managed target parent changed before commit"});
        }
        auto sameParent = sameDirectory(temporary.namespaceParentDescriptor(), currentParent.value().get());
        if (!sameParent) {
            return Result<void>::fail(sameParent.error());
        }
        if (!sameParent.value()) {
            return Result<void>::fail(
                {ErrorCode::SecurityPolicyViolation, "Atomic replacement parent changed before commit"});
        }

        auto singleLink = ensureSingleLinkDescriptor(temporary.rootedFile().descriptor(), "Private temporary payload");
        if (!singleLink) {
            return singleLink;
        }
        auto namedPayload = verifyEntryIdentity(temporary.namespaceDescriptor(), privatePayloadName,
                                                temporary.rootedFile().descriptor(), "Private temporary payload");
        if (!namedPayload) {
            return namedPayload;
        }

        const auto& targetName = components.value().back();
        if (expectation.kind == RootedEntryExpectationKind::Missing) {
            bool installedMutation = false;
            auto renamed = renameNoReplace(temporary.namespaceDescriptor(), privatePayloadName,
                                           temporary.namespaceParentDescriptor(), targetName,
                                           "Failed to install a previously missing managed target", &installedMutation);
            if (installedMutation) {
                temporary.clearPayload();
                temporary.markPublished(static_cast<bool>(renamed));
            }
            if (!renamed) {
                return renamed;
            }
            auto installed = verifyEntryIdentity(temporary.namespaceParentDescriptor(), targetName,
                                                 temporary.rootedFile().descriptor(), "Installed managed target");
            if (!installed) {
                bool restoredMutation = false;
                auto restored = renameNoReplace(
                    temporary.namespaceParentDescriptor(), targetName, temporary.namespaceDescriptor(),
                    privatePayloadName, "Failed to restore rejected private temporary payload", &restoredMutation);
                if (restoredMutation) {
                    temporary.restorePayload();
                    temporary.markRestored(static_cast<bool>(restored));
                } else {
                    temporary.markUnknown();
                }
                if (!restored) {
                    return Result<void>::fail(
                        {ErrorCode::ApplyFailed,
                         installed.error().message + "; source restoration failed: " + restored.error().message});
                }
                return installed;
            }
            auto cleaned = temporary.cleanupNamespace();
            if (!cleaned) {
                // The managed target has already been installed and verified
                // above. A namespace cleanup failure here (for example a
                // namespace-name substitution, which cleanupDirectory refuses
                // to remove) does not invalidate the published target, so the
                // commit must still report success rather than failing an
                // install that is already complete and correct.
                return Result<void>::ok();
            }
            return Result<void>::ok();
        }

        std::string displacedIdentity = expectation.identity;
        bool exchangeMutation = false;
        auto exchanged =
            exchangeEntries(temporary.namespaceDescriptor(), privatePayloadName, temporary.namespaceParentDescriptor(),
                            targetName, "Failed to atomically exchange managed target", &exchangeMutation);
        if (exchangeMutation) {
            temporary.swapPayloadIdentity(displacedIdentity);
            temporary.markPublished(static_cast<bool>(exchanged));
        }
        if (!exchanged) {
            return exchanged;
        }
        auto installed = verifyEntryIdentity(temporary.namespaceParentDescriptor(), targetName,
                                             temporary.rootedFile().descriptor(), "Installed managed target");
        auto displaced = verifyEntryIdentity(temporary.namespaceDescriptor(), privatePayloadName, expectation.identity,
                                             "Displaced managed target");
        if (!installed || !displaced) {
            bool restoredMutation = false;
            auto restored = exchangeEntries(temporary.namespaceDescriptor(), privatePayloadName,
                                            temporary.namespaceParentDescriptor(), targetName,
                                            "Failed to reverse rejected managed target exchange", &restoredMutation);
            if (restoredMutation) {
                temporary.swapPayloadIdentity(displacedIdentity);
                temporary.markRestored(static_cast<bool>(restored));
            } else {
                temporary.markUnknown();
            }
            if (!restored) {
                const auto original = !installed ? installed.error() : displaced.error();
                return Result<void>::fail({ErrorCode::ApplyFailed, original.message + "; target restoration failed: " +
                                                                       restored.error().message});
            }
            return !installed ? installed : displaced;
        }

        bool oldTargetRemoved = false;
        auto removedOldTarget =
            removeEntryWithIdentity(temporary.namespaceDescriptor(), privatePayloadName, expectation.identity,
                                    "displaced managed target", &oldTargetRemoved);
        if (!removedOldTarget) {
            if (oldTargetRemoved) {
                temporary.clearPayload();
                temporary.markFailureReconcilable();
                return removedOldTarget;
            }
            bool restoredMutation = false;
            auto restored = exchangeEntries(
                temporary.namespaceDescriptor(), privatePayloadName, temporary.namespaceParentDescriptor(), targetName,
                "Failed to restore target after displaced-file cleanup failure", &restoredMutation);
            if (restoredMutation) {
                temporary.swapPayloadIdentity(displacedIdentity);
                temporary.markRestored(static_cast<bool>(restored));
            } else {
                temporary.markUnknown();
            }
            if (!restored) {
                return Result<void>::fail(
                    {ErrorCode::ApplyFailed,
                     removedOldTarget.error().message + "; target restoration failed: " + restored.error().message});
            }
            return removedOldTarget;
        }
        temporary.clearPayload();
        auto cleaned = temporary.cleanupNamespace();
        if (!cleaned) {
            return cleaned;
        }
        return Result<void>::ok();
    } catch (...) {
        return Result<void>::fail({ErrorCode::FileSystemError, "Unexpected private temporary commit failure"});
    }
}

Result<void> PosixRootedDirectory::quarantineAndRemoveOpenedFile(PosixRootedFile& file,
                                                                 const RootedEntryExpectation& expectation) noexcept {
    try {
        if (file.owner() != this || expectation.kind != RootedEntryExpectationKind::Identity) {
            return Result<void>::fail({ErrorCode::SecurityPolicyViolation, "Invalid rooted removal expectation"});
        }
        auto opened = file.metadata();
        if (!opened) {
            return Result<void>::fail(opened.error());
        }
        if (opened.value().identity != expectation.identity) {
            return Result<void>::fail({ErrorCode::SecurityPolicyViolation, "Managed remove target identity changed"});
        }
        auto parent = duplicateDescriptor(file.parentDescriptor());
        if (!parent) {
            return Result<void>::fail(parent.error());
        }
        auto quarantine = createPrivateNamespace(std::move(parent.value()));
        if (!quarantine) {
            return Result<void>::fail(quarantine.error());
        }
        const auto finishQuarantine = [&](Result<void> operation) {
            auto cleaned = quarantine.value().cleanupDirectory();
            auto closed = quarantine.value().close();
            if (!operation) {
                auto error = operation.error();
                if (!cleaned) {
                    error.message += "; failed to clean removal quarantine: " + cleaned.error().message;
                }
                if (!closed) {
                    error.message += "; failed to close removal quarantine: " + closed.error().message;
                }
                return Result<void>::fail(std::move(error));
            }
            if (!cleaned) {
                auto error = cleaned.error();
                if (!closed) {
                    error.message += "; failed to close removal quarantine: " + closed.error().message;
                }
                return Result<void>::fail(std::move(error));
            }
            return closed;
        };
        auto moved = renameNoReplace(file.parentDescriptor(), file.name(), quarantine.value().descriptor(),
                                     privatePayloadName, "Failed to quarantine managed removal target");
        if (!moved) {
            return finishQuarantine(moved);
        }
        auto quarantined = verifyEntryIdentity(quarantine.value().descriptor(), privatePayloadName, file.descriptor(),
                                               "Quarantined managed removal target");
        if (!quarantined) {
            auto restored =
                renameNoReplace(quarantine.value().descriptor(), privatePayloadName, file.parentDescriptor(),
                                file.name(), "Failed to restore rejected managed removal target");
            if (!restored) {
                return finishQuarantine(Result<void>::fail(
                    {ErrorCode::ApplyFailed,
                     quarantined.error().message + "; target restoration failed: " + restored.error().message}));
            }
            return finishQuarantine(quarantined);
        }
        bool quarantinedTargetRemoved = false;
        auto removed =
            removeEntryWithIdentity(quarantine.value().descriptor(), privatePayloadName, expectation.identity,
                                    "quarantined managed removal target", &quarantinedTargetRemoved);
        if (!removed) {
            if (quarantinedTargetRemoved) {
                return finishQuarantine(removed);
            }
            auto restored =
                renameNoReplace(quarantine.value().descriptor(), privatePayloadName, file.parentDescriptor(),
                                file.name(), "Failed to restore managed removal target after cleanup failure");
            if (!restored) {
                return finishQuarantine(Result<void>::fail(
                    {ErrorCode::ApplyFailed,
                     removed.error().message + "; target restoration failed: " + restored.error().message}));
            }
            return finishQuarantine(removed);
        }
        return finishQuarantine(Result<void>::ok());
    } catch (...) {
        return Result<void>::fail({ErrorCode::FileSystemError, "Unexpected rooted removal quarantine failure"});
    }
}

Result<void> PosixTemporaryFile::commit(const RootedEntryExpectation& expectation) noexcept {
    auto flushed = file_->flush();
    if (!flushed) {
        return flushed;
    }
    return root_.commitPrivateTemporary(*this, expectation);
}

Result<FileDescriptor> openRootPath(const std::filesystem::path& path, bool create,
                                    RootedDirectoryCreationMode directoryMode) {
    if (!path.is_absolute()) {
        return Result<FileDescriptor>::fail(
            {ErrorCode::SecurityPolicyViolation, "Rooted filesystem paths must be absolute"});
    }
    // Resolve the parent prefix so system-level symbolic links (for example
    // macOS /var -> /private/var and /tmp -> /private/tmp) do not trip the
    // O_NOFOLLOW traversal below. The final component is deliberately left
    // unresolved: a rooted directory that is itself a symbolic link must still
    // be rejected by the O_NOFOLLOW open.
    std::filesystem::path resolved;
    {
        const auto parent = path.parent_path();
        std::error_code resolveError;
        resolved = std::filesystem::weakly_canonical(parent, resolveError);
        if (resolveError) {
            return posixFailureValue<FileDescriptor>("Failed to resolve rooted filesystem parent path");
        }
        resolved /= path.filename();
    }
    auto current =
        FileDescriptor(::open("/", O_RDONLY | O_DIRECTORY | O_NONBLOCK | closeOnExecFlag() | noFollowFlag()));
    if (!current) {
        return posixFailureValue<FileDescriptor>("Failed to open filesystem root");
    }
    for (const auto& component : resolved.lexically_normal().relative_path()) {
        const auto name = component.native();
        if (name.empty() || name == ".") {
            continue;
        }
        if (name == "..") {
            return Result<FileDescriptor>::fail(
                {ErrorCode::PathTraversalRejected, "Rooted filesystem path contains parent traversal"});
        }
        auto next = openChildDirectory(current.get(), name, create, directoryMode);
        if (!next) {
            return next;
        }
        if (!next.value()) {
            return Result<FileDescriptor>::fail({ErrorCode::FileSystemError, "Rooted filesystem path does not exist"});
        }
        current = std::move(next.value());
    }
    return Result<FileDescriptor>::ok(std::move(current));
}

} // namespace

Result<std::unique_ptr<IRootedDirectory>>
openDefaultRootedDirectory(const std::filesystem::path& path, RootAccess access, bool create,
                           RootedDirectoryCreationMode directoryMode) noexcept {
    try {
        auto root = openRootPath(path, create, directoryMode);
        if (!root) {
            return Result<std::unique_ptr<IRootedDirectory>>::fail(root.error());
        }
        auto result = std::make_unique<PosixRootedDirectory>(std::move(root.value()), access);
        return Result<std::unique_ptr<IRootedDirectory>>::ok(std::move(result));
    } catch (...) {
        return Result<std::unique_ptr<IRootedDirectory>>::fail(
            {ErrorCode::FileSystemError, "Unexpected rooted filesystem failure"});
    }
}

} // namespace autoupdater

#endif
