#include "libAutoUpdater/interfaces/IFileSystem.h"

#include <fstream>
#include <sstream>

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
            std::filesystem::remove(to, ec);
            ec.clear();
            std::filesystem::rename(from, to, ec);
            if (ec) {
                std::filesystem::copy_file(from, to, std::filesystem::copy_options::overwrite_existing, ec);
                if (ec) {
                    return Result<void>::fail({ErrorCode::FileSystemError, ec.message()});
                }
                std::filesystem::remove(from, ec);
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
            std::ofstream output(path, std::ios::binary | std::ios::trunc);
            if (!output) {
                return Result<void>::fail({ErrorCode::FileSystemError, "Failed to open file for writing"});
            }
            output.write(text.data(), static_cast<std::streamsize>(text.size()));
            return Result<void>::ok();
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
