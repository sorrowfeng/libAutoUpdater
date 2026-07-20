#include "libAutoUpdater/interfaces/IFileSystem.h"

#include "default/RootedFileSystemFactory.h"
#include "util/BoundedFile.h"
#include "util/PathUtil.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <optional>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#endif

namespace autoupdater {

namespace {

Error fileSystemError(const std::string& operation, const std::error_code& error) {
    return {ErrorCode::FileSystemError, operation + ": " + error.message()};
}

Error appendSecondary(Error primary, const std::string& context, const Error& secondary) {
    primary.message += "; " + context + " [" + toString(secondary.code) + "]: " + secondary.message;
    return primary;
}

std::filesystem::perms sanitizedFilePermissions(std::filesystem::perms permissions) noexcept {
    constexpr auto permissionMask =
        std::filesystem::perms::owner_all | std::filesystem::perms::group_all | std::filesystem::perms::others_all;
    return permissions & permissionMask;
}

Result<void> atomicRenameOrReplace(const std::filesystem::path& from, const std::filesystem::path& to) {
#ifdef _WIN32
    if (!MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const DWORD code = GetLastError();
        return Result<void>::fail(fileSystemError("Failed to atomically replace file",
                                                  std::error_code(static_cast<int>(code), std::system_category())));
    }
#else
    std::error_code error;
    std::filesystem::rename(from, to, error);
    if (error) {
        return Result<void>::fail(fileSystemError("Failed to atomically replace file", error));
    }
#endif
    return Result<void>::ok();
}

struct ResolvedFilePath {
    std::filesystem::path parent;
    std::string name;
};

Result<ResolvedFilePath> resolveFilePath(const std::filesystem::path& path) {
    if (path.empty()) {
        return Result<ResolvedFilePath>::fail({ErrorCode::FileSystemError, "File path is empty"});
    }
    std::error_code error;
    const auto absolute = std::filesystem::absolute(path, error).lexically_normal();
    if (error) {
        return Result<ResolvedFilePath>::fail(fileSystemError("Failed to resolve file path", error));
    }
    if (absolute.filename().empty()) {
        return Result<ResolvedFilePath>::fail({ErrorCode::FileSystemError, "File path has no filename"});
    }
    auto name = util::pathToUtf8(absolute.filename());
    if (name.empty()) {
        return Result<ResolvedFilePath>::fail({ErrorCode::FileSystemError, "Failed to encode file name as UTF-8"});
    }
    auto validName = util::validateManagedPath(name);
    if (!validName) {
        return Result<ResolvedFilePath>::fail(validName.error());
    }
    return Result<ResolvedFilePath>::ok({absolute.parent_path(), std::move(name)});
}

struct ExistingTarget {
    RootedEntryExpectation expectation = RootedEntryExpectation::missing();
    std::optional<RootedFileMetadata> metadata;
};

Result<ExistingTarget> inspectTarget(IRootedDirectory& root, const std::string& name) {
    auto opened =
        root.openRegularFile(name, RootedFileOpenMode::ReadOnly, RootedDirectoryCreationMode::InstalledContent);
    if (!opened) {
        return Result<ExistingTarget>::fail(opened.error());
    }
    ExistingTarget target;
    if (!opened.value().exists()) {
        return Result<ExistingTarget>::ok(std::move(target));
    }
    auto metadata = opened.value().file->metadata();
    if (!metadata) {
        auto error = metadata.error();
        auto closed = opened.value().file->close();
        if (!closed) {
            error = appendSecondary(std::move(error), "failed to close inspected target", closed.error());
        }
        return Result<ExistingTarget>::fail(std::move(error));
    }
    target.expectation = RootedEntryExpectation::matching(metadata.value());
    target.metadata = std::move(metadata.value());
    auto closed = opened.value().file->close();
    if (!closed) {
        return Result<ExistingTarget>::fail(closed.error());
    }
    return Result<ExistingTarget>::ok(std::move(target));
}

Result<void> copyOpenedFile(IRootedFile& source, IRootedFile& destination) {
    auto rewound = source.seek(0);
    if (!rewound) {
        return rewound;
    }
    std::array<unsigned char, 64 * 1024> buffer{};
    for (;;) {
        auto read = source.read(buffer.data(), buffer.size());
        if (!read) {
            return Result<void>::fail(read.error());
        }
        if (read.value() == 0) {
            return Result<void>::ok();
        }
        auto written = destination.write(buffer.data(), read.value());
        if (!written) {
            return written;
        }
    }
}

Result<void> discardTemporary(std::unique_ptr<IRootedTemporaryFile>& temporary) {
    if (!temporary) {
        return Result<void>::ok();
    }
    auto discarded = temporary->discard();
    temporary.reset();
    return discarded;
}

Result<void> failAndDiscard(std::unique_ptr<IRootedTemporaryFile>& temporary, Error primary,
                            const std::string& context) {
    auto discarded = discardTemporary(temporary);
    if (!discarded) {
        primary = appendSecondary(std::move(primary), context, discarded.error());
    }
    return Result<void>::fail(std::move(primary));
}

Result<void> closeAfter(IRootedFile& file, Result<void> operation, const std::string& context) {
    auto closed = file.close();
    if (operation && closed) {
        return Result<void>::ok();
    }
    if (!operation) {
        auto error = operation.error();
        if (!closed) {
            error = appendSecondary(std::move(error), context, closed.error());
        }
        return Result<void>::fail(std::move(error));
    }
    return Result<void>::fail(closed.error());
}

Result<bool> openedFilesEqual(IRootedFile& left, IRootedFile& right) {
    auto leftStart = left.seek(0);
    if (!leftStart) {
        return Result<bool>::fail(leftStart.error());
    }
    auto rightStart = right.seek(0);
    if (!rightStart) {
        return Result<bool>::fail(rightStart.error());
    }
    std::array<unsigned char, 64 * 1024> leftBuffer{};
    std::array<unsigned char, 64 * 1024> rightBuffer{};
    for (;;) {
        auto leftRead = left.read(leftBuffer.data(), leftBuffer.size());
        if (!leftRead) {
            return Result<bool>::fail(leftRead.error());
        }
        auto rightRead = right.read(rightBuffer.data(), rightBuffer.size());
        if (!rightRead) {
            return Result<bool>::fail(rightRead.error());
        }
        if (leftRead.value() != rightRead.value()) {
            return Result<bool>::ok(false);
        }
        if (leftRead.value() == 0) {
            return Result<bool>::ok(true);
        }
        if (std::memcmp(leftBuffer.data(), rightBuffer.data(), leftRead.value()) != 0) {
            return Result<bool>::ok(false);
        }
    }
}

Result<std::optional<RootedFileMetadata>> inspectMatchingFile(IRootedDirectory& root, const std::string& name,
                                                              IRootedFile& source,
                                                              const RootedFileMetadata& sourceMetadata,
                                                              const std::string& publishedIdentity) {
    auto opened =
        root.openRegularFile(name, RootedFileOpenMode::ReadOnly, RootedDirectoryCreationMode::InstalledContent);
    if (!opened) {
        return Result<std::optional<RootedFileMetadata>>::fail(opened.error());
    }
    if (!opened.value().exists()) {
        return Result<std::optional<RootedFileMetadata>>::ok(std::nullopt);
    }
    auto metadata = opened.value().file->metadata();
    if (!metadata) {
        auto error = metadata.error();
        auto closed = opened.value().file->close();
        if (!closed) {
            error = appendSecondary(std::move(error), "failed to close reconciled target", closed.error());
        }
        return Result<std::optional<RootedFileMetadata>>::fail(std::move(error));
    }
    bool matches = metadata.value().identity == publishedIdentity && metadata.value().size == sourceMetadata.size;
    if (matches) {
        auto compared = openedFilesEqual(source, *opened.value().file);
        if (!compared) {
            auto error = compared.error();
            auto closed = opened.value().file->close();
            if (!closed) {
                error = appendSecondary(std::move(error), "failed to close reconciled target", closed.error());
            }
            return Result<std::optional<RootedFileMetadata>>::fail(std::move(error));
        }
        matches = compared.value();
    }
    auto closed = opened.value().file->close();
    if (!closed) {
        return Result<std::optional<RootedFileMetadata>>::fail(closed.error());
    }
    return Result<std::optional<RootedFileMetadata>>::ok(matches ? std::optional<RootedFileMetadata>(metadata.value())
                                                                 : std::nullopt);
}

Result<std::optional<RootedFileMetadata>> inspectMatchingText(IRootedDirectory& root, const std::string& name,
                                                              const std::string& text,
                                                              const std::string& publishedIdentity) {
    auto opened =
        root.openRegularFile(name, RootedFileOpenMode::ReadOnly, RootedDirectoryCreationMode::InstalledContent);
    if (!opened) {
        return Result<std::optional<RootedFileMetadata>>::fail(opened.error());
    }
    if (!opened.value().exists()) {
        return Result<std::optional<RootedFileMetadata>>::ok(std::nullopt);
    }
    auto metadata = opened.value().file->metadata();
    if (!metadata) {
        auto error = metadata.error();
        auto closed = opened.value().file->close();
        if (!closed) {
            error = appendSecondary(std::move(error), "failed to close reconciled text target", closed.error());
        }
        return Result<std::optional<RootedFileMetadata>>::fail(std::move(error));
    }
    bool matches = metadata.value().identity == publishedIdentity && metadata.value().size == text.size();
    std::size_t offset = 0;
    std::array<char, 64 * 1024> buffer{};
    while (matches && offset < text.size()) {
        auto read = opened.value().file->read(buffer.data(), std::min(buffer.size(), text.size() - offset));
        if (!read) {
            auto error = read.error();
            auto closed = opened.value().file->close();
            if (!closed) {
                error = appendSecondary(std::move(error), "failed to close reconciled text target", closed.error());
            }
            return Result<std::optional<RootedFileMetadata>>::fail(std::move(error));
        }
        if (read.value() == 0 || std::memcmp(buffer.data(), text.data() + offset, read.value()) != 0) {
            matches = false;
            break;
        }
        offset += read.value();
    }
    matches = matches && offset == text.size();
    auto closed = opened.value().file->close();
    if (!closed) {
        return Result<std::optional<RootedFileMetadata>>::fail(closed.error());
    }
    return Result<std::optional<RootedFileMetadata>>::ok(matches ? std::optional<RootedFileMetadata>(metadata.value())
                                                                 : std::nullopt);
}

Result<void> copyFileAtomically(IRootedDirectory& root, const std::string& name, IRootedFile& source,
                                const RootedFileMetadata& sourceMetadata, const ExistingTarget& target) {
    auto temporary = root.createAtomicReplacement(name, RootedDirectoryCreationMode::InstalledContent);
    if (!temporary) {
        return Result<void>::fail(temporary.error());
    }
    auto copied = copyOpenedFile(source, temporary.value()->file());
    if (!copied) {
        return failAndDiscard(temporary.value(), copied.error(), "failed to discard incomplete copied file");
    }
    if (sourceMetadata.permissions != std::filesystem::perms::unknown) {
        auto permissions =
            temporary.value()->file().setPermissions(sanitizedFilePermissions(sourceMetadata.permissions));
        if (!permissions) {
            return failAndDiscard(temporary.value(), permissions.error(),
                                  "failed to discard copied file after permission error");
        }
    }
    auto preparedMetadata = temporary.value()->file().metadata();
    if (!preparedMetadata) {
        return failAndDiscard(temporary.value(), preparedMetadata.error(),
                              "failed to discard copied file after metadata error");
    }
    auto committed = temporary.value()->commit(target.expectation);
    auto discarded = temporary.value()->discard();
    const auto publication = temporary.value()->publishStatus();
    temporary.value().reset();
    if (committed) {
        return discarded;
    }

    auto error = committed.error();
    if (!discarded) {
        return Result<void>::fail(
            appendSecondary(std::move(error), "failed to finish copied-file cleanup", discarded.error()));
    }
    if (publication.publication == RootedPublication::NotPublished || !publication.namespaceDurable ||
        !publication.failureCanBeReconciled) {
        return Result<void>::fail(std::move(error));
    }
    auto observed = inspectMatchingFile(root, name, source, sourceMetadata, preparedMetadata.value().identity);
    if (!observed) {
        return Result<void>::fail(
            appendSecondary(std::move(error), "failed to reconcile copied-file publication", observed.error()));
    }
    if (!observed.value()) {
        return Result<void>::fail(std::move(error));
    }
    return Result<void>::ok();
}

Result<void> writeTextAtomically(IRootedDirectory& root, const std::string& name, const std::string& text,
                                 const ExistingTarget& target) {
    auto temporary = root.createAtomicReplacement(name, RootedDirectoryCreationMode::InstalledContent);
    if (!temporary) {
        return Result<void>::fail(temporary.error());
    }
    auto written = temporary.value()->file().write(text.data(), text.size());
    if (!written) {
        return failAndDiscard(temporary.value(), written.error(), "failed to discard incomplete text file");
    }
    if (target.metadata && target.metadata->permissions != std::filesystem::perms::unknown) {
        auto permissions =
            temporary.value()->file().setPermissions(sanitizedFilePermissions(target.metadata->permissions));
        if (!permissions) {
            return failAndDiscard(temporary.value(), permissions.error(),
                                  "failed to discard text file after permission error");
        }
    }
    auto preparedMetadata = temporary.value()->file().metadata();
    if (!preparedMetadata) {
        return failAndDiscard(temporary.value(), preparedMetadata.error(),
                              "failed to discard text file after metadata error");
    }
    auto committed = temporary.value()->commit(target.expectation);
    auto discarded = temporary.value()->discard();
    const auto publication = temporary.value()->publishStatus();
    temporary.value().reset();
    if (committed) {
        return discarded;
    }

    auto error = committed.error();
    if (!discarded) {
        return Result<void>::fail(
            appendSecondary(std::move(error), "failed to finish text-file cleanup", discarded.error()));
    }
    if (publication.publication == RootedPublication::NotPublished || !publication.namespaceDurable ||
        !publication.failureCanBeReconciled) {
        return Result<void>::fail(std::move(error));
    }
    auto observed = inspectMatchingText(root, name, text, preparedMetadata.value().identity);
    if (!observed) {
        return Result<void>::fail(
            appendSecondary(std::move(error), "failed to reconcile text-file publication", observed.error()));
    }
    if (!observed.value()) {
        return Result<void>::fail(std::move(error));
    }
    return Result<void>::ok();
}

class StdFileSystem final : public IFileSystem {
  public:
    bool exists(const std::filesystem::path& path) noexcept override {
        std::error_code ec;
        return std::filesystem::exists(path, ec);
    }

    bool isRegularFile(const std::filesystem::path& path) noexcept override {
        std::error_code ec;
        return std::filesystem::is_regular_file(path, ec);
    }

    Result<std::uint64_t> fileSize(const std::filesystem::path& path) noexcept override {
        std::error_code ec;
        const auto size = std::filesystem::file_size(path, ec);
        if (ec) {
            return Result<std::uint64_t>::fail({ErrorCode::FileSystemError, ec.message()});
        }
        return Result<std::uint64_t>::ok(static_cast<std::uint64_t>(size));
    }

    Result<void> createDirectories(const std::filesystem::path& path) noexcept override {
        try {
            std::error_code ec;
            std::filesystem::create_directories(path, ec);
            if (ec) {
                return Result<void>::fail({ErrorCode::FileSystemError, ec.message()});
            }
            return Result<void>::ok();
        } catch (...) {
            return Result<void>::fail({ErrorCode::FileSystemError, "Failed to create directories"});
        }
    }

    Result<void> copyFile(const std::filesystem::path& from, const std::filesystem::path& to,
                          bool overwrite) noexcept override {
        try {
            auto sourcePath = resolveFilePath(from);
            if (!sourcePath) {
                return Result<void>::fail(sourcePath.error());
            }
            auto targetPath = resolveFilePath(to);
            if (!targetPath) {
                return Result<void>::fail(targetPath.error());
            }
            auto sourceRoot = openDefaultRootedDirectory(sourcePath.value().parent, RootAccess::ReadOnly, false,
                                                         RootedDirectoryCreationMode::InstalledContent);
            if (!sourceRoot) {
                return Result<void>::fail(sourceRoot.error());
            }
            auto source = sourceRoot.value()->openRegularFile(sourcePath.value().name, RootedFileOpenMode::ReadOnly,
                                                              RootedDirectoryCreationMode::InstalledContent);
            if (!source) {
                return Result<void>::fail(source.error());
            }
            if (!source.value().exists()) {
                return Result<void>::fail({ErrorCode::FileSystemError, "Copy source does not exist"});
            }
            auto sourceMetadata = source.value().file->metadata();
            if (!sourceMetadata) {
                return closeAfter(*source.value().file, Result<void>::fail(sourceMetadata.error()),
                                  "failed to close copy source");
            }
            auto targetRoot = openDefaultRootedDirectory(targetPath.value().parent, RootAccess::ReadWrite, true,
                                                         RootedDirectoryCreationMode::InstalledContent);
            if (!targetRoot) {
                return closeAfter(*source.value().file, Result<void>::fail(targetRoot.error()),
                                  "failed to close copy source");
            }
            auto target = inspectTarget(*targetRoot.value(), targetPath.value().name);
            if (!target) {
                return closeAfter(*source.value().file, Result<void>::fail(target.error()),
                                  "failed to close copy source");
            }
            if (target.value().metadata && !overwrite) {
                return closeAfter(*source.value().file,
                                  Result<void>::fail({ErrorCode::FileSystemError, "Copy target already exists"}),
                                  "failed to close copy source");
            }
            auto copied = copyFileAtomically(*targetRoot.value(), targetPath.value().name, *source.value().file,
                                             sourceMetadata.value(), target.value());
            return closeAfter(*source.value().file, std::move(copied), "failed to close copy source");
        } catch (...) {
            return Result<void>::fail({ErrorCode::FileSystemError, "Failed to copy file"});
        }
    }

    Result<void> renameOrReplace(const std::filesystem::path& from, const std::filesystem::path& to) noexcept override {
        try {
            if (from.lexically_normal() == to.lexically_normal()) {
                std::error_code existsError;
                const bool sourceExists = std::filesystem::exists(from, existsError);
                if (existsError) {
                    return Result<void>::fail(fileSystemError("Failed to inspect rename source", existsError));
                }
                return sourceExists ? Result<void>::ok()
                                    : Result<void>::fail({ErrorCode::FileSystemError, "Rename source does not exist"});
            }
            std::error_code ec;
            if (!to.parent_path().empty()) {
                std::filesystem::create_directories(to.parent_path(), ec);
                if (ec) {
                    return Result<void>::fail({ErrorCode::FileSystemError, ec.message()});
                }
            }
            return atomicRenameOrReplace(from, to);
        } catch (...) {
            return Result<void>::fail({ErrorCode::FileSystemError, "Failed to replace file"});
        }
    }

    Result<void> remove(const std::filesystem::path& path) noexcept override {
        try {
            std::error_code ec;
            std::filesystem::remove(path, ec);
            if (ec) {
                return Result<void>::fail({ErrorCode::FileSystemError, ec.message()});
            }
            return Result<void>::ok();
        } catch (...) {
            return Result<void>::fail({ErrorCode::FileSystemError, "Failed to remove file"});
        }
    }

    Result<void> removeAll(const std::filesystem::path& path) noexcept override {
        try {
            std::error_code ec;
            std::filesystem::remove_all(path, ec);
            if (ec) {
                return Result<void>::fail({ErrorCode::FileSystemError, ec.message()});
            }
            return Result<void>::ok();
        } catch (...) {
            return Result<void>::fail({ErrorCode::FileSystemError, "Failed to remove tree"});
        }
    }

    Result<std::string> readText(const std::filesystem::path& path, std::uint64_t maxBytes) noexcept override {
        return util::readRegularFileWithLimit(path, maxBytes, ErrorCode::FileSystemError, "file");
    }

    Result<void> writeText(const std::filesystem::path& path, const std::string& text) noexcept override {
        try {
            auto resolved = resolveFilePath(path);
            if (!resolved) {
                return Result<void>::fail(resolved.error());
            }
            auto root = openDefaultRootedDirectory(resolved.value().parent, RootAccess::ReadWrite, true,
                                                   RootedDirectoryCreationMode::InstalledContent);
            if (!root) {
                return Result<void>::fail(root.error());
            }
            auto target = inspectTarget(*root.value(), resolved.value().name);
            if (!target) {
                return Result<void>::fail(target.error());
            }
            return writeTextAtomically(*root.value(), resolved.value().name, text, target.value());
        } catch (...) {
            return Result<void>::fail({ErrorCode::FileSystemError, "Failed to write file"});
        }
    }

    Result<std::unique_ptr<IRootedDirectory>> openRoot(const std::filesystem::path& path, RootAccess access,
                                                       bool create,
                                                       RootedDirectoryCreationMode directoryMode) noexcept override {
        return openDefaultRootedDirectory(path, access, create, directoryMode);
    }
};

} // namespace

std::shared_ptr<IFileSystem> createDefaultFileSystem() {
    return std::make_shared<StdFileSystem>();
}

} // namespace autoupdater
