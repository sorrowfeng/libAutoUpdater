#include "libAutoUpdater/interfaces/IFileSystem.h"

#include <fstream>
#include <sstream>

#ifdef _WIN32
#include <windows.h>
#endif

namespace autoupdater {

namespace {

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
            std::error_code ec;
            if (!to.parent_path().empty()) {
                std::filesystem::create_directories(to.parent_path(), ec);
                if (ec) {
                    return Result<void>::fail({ErrorCode::FileSystemError, ec.message()});
                }
            }
            const auto options =
                overwrite ? std::filesystem::copy_options::overwrite_existing : std::filesystem::copy_options::none;
            std::filesystem::copy_file(from, to, options, ec);
            if (ec) {
                return Result<void>::fail({ErrorCode::FileSystemError, ec.message()});
            }
            return Result<void>::ok();
        } catch (...) {
            return Result<void>::fail({ErrorCode::FileSystemError, "Failed to copy file"});
        }
    }

    Result<void> renameOrReplace(const std::filesystem::path& from, const std::filesystem::path& to) noexcept override {
        try {
            std::error_code ec;
            if (!to.parent_path().empty()) {
                std::filesystem::create_directories(to.parent_path(), ec);
                if (ec) {
                    return Result<void>::fail({ErrorCode::FileSystemError, ec.message()});
                }
            }
#ifdef _WIN32
            // MoveFileExW atomically replaces an existing destination; a
            // remove+rename sequence would leave a window where the target
            // does not exist at all.
            if (MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING)) {
                return Result<void>::ok();
            }
            // Fall through to the copy path for cross-volume moves.
#else
            // POSIX rename(2) atomically replaces the destination.
            std::filesystem::rename(from, to, ec);
            if (!ec) {
                return Result<void>::ok();
            }
            ec.clear();
            // Fall through to the copy path for cross-device moves (EXDEV).
#endif
            std::filesystem::copy_file(from, to, std::filesystem::copy_options::overwrite_existing, ec);
            if (ec) {
                return Result<void>::fail({ErrorCode::FileSystemError, ec.message()});
            }
            std::filesystem::remove(from, ec);
            if (ec) {
                return Result<void>::fail({ErrorCode::FileSystemError, ec.message()});
            }
            return Result<void>::ok();
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

    Result<std::string> readText(const std::filesystem::path& path) noexcept override {
        try {
            std::ifstream input(path, std::ios::binary);
            if (!input) {
                return Result<std::string>::fail({ErrorCode::FileSystemError, "Failed to open file"});
            }
            std::ostringstream stream;
            stream << input.rdbuf();
            return Result<std::string>::ok(stream.str());
        } catch (...) {
            return Result<std::string>::fail({ErrorCode::FileSystemError, "Failed to read file"});
        }
    }

    Result<void> writeText(const std::filesystem::path& path, const std::string& text) noexcept override {
        try {
            std::error_code ec;
            if (!path.parent_path().empty()) {
                std::filesystem::create_directories(path.parent_path(), ec);
                if (ec) {
                    return Result<void>::fail({ErrorCode::FileSystemError, ec.message()});
                }
            }
            // Write to a sibling temp file and atomically move it over the
            // destination so a crash cannot leave a truncated file behind.
            auto tempPath = path;
            tempPath += ".tmp";
            {
                std::ofstream output(tempPath, std::ios::binary | std::ios::trunc);
                if (!output) {
                    return Result<void>::fail({ErrorCode::FileSystemError, "Failed to open file for writing"});
                }
                output.write(text.data(), static_cast<std::streamsize>(text.size()));
                output.flush();
                if (!output.good()) {
                    output.close();
                    std::filesystem::remove(tempPath, ec);
                    return Result<void>::fail({ErrorCode::FileSystemError, "Failed to write file contents"});
                }
            }
            return renameOrReplace(tempPath, path);
        } catch (...) {
            return Result<void>::fail({ErrorCode::FileSystemError, "Failed to write file"});
        }
    }
};

} // namespace

std::shared_ptr<IFileSystem> createDefaultFileSystem() {
    return std::make_shared<StdFileSystem>();
}

} // namespace autoupdater
