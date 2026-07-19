#include "util/BoundedFile.h"

#include <array>
#include <fstream>

namespace autoupdater::util {

Result<std::string> readRegularFileWithLimit(const std::filesystem::path& path, std::uint64_t maxBytes,
                                             ErrorCode readErrorCode, const std::string& description) noexcept {
    try {
        std::error_code error;
        if (!std::filesystem::is_regular_file(path, error) || error) {
            return Result<std::string>::fail({readErrorCode, description + " is not a readable regular file"});
        }
        const auto declaredSize = std::filesystem::file_size(path, error);
        if (error) {
            return Result<std::string>::fail({readErrorCode, "Failed to inspect " + description});
        }
        if (declaredSize > maxBytes) {
            return Result<std::string>::fail(
                {ErrorCode::ResourceLimitExceeded, description + " exceeds its byte limit"});
        }

        std::ifstream input(path, std::ios::binary);
        if (!input) {
            return Result<std::string>::fail({readErrorCode, "Failed to open " + description});
        }

        std::string contents;
        std::array<char, 64 * 1024> buffer{};
        std::uint64_t consumed = 0;
        for (;;) {
            input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            const auto count = input.gcount();
            if (count <= 0) {
                break;
            }
            const auto bytes = static_cast<std::uint64_t>(count);
            if (bytes > maxBytes - consumed) {
                return Result<std::string>::fail(
                    {ErrorCode::ResourceLimitExceeded, description + " exceeds its byte limit"});
            }
            contents.append(buffer.data(), static_cast<std::size_t>(count));
            consumed += bytes;
        }
        if (input.bad()) {
            return Result<std::string>::fail({readErrorCode, "Failed to read " + description});
        }
        return Result<std::string>::ok(std::move(contents));
    } catch (...) {
        return Result<std::string>::fail({readErrorCode, "Failed to read " + description});
    }
}

} // namespace autoupdater::util
