#include "util/BoundedFile.h"

#include <array>
#include <fstream>
#include <optional>

namespace autoupdater::util {

Result<std::string> readRegularFileWithLimit(const std::filesystem::path& path, std::uint64_t maxBytes,
                                             ErrorCode readErrorCode, const std::string& description) noexcept {
    std::filebuf file;
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

        if (!file.open(path, std::ios::in | std::ios::binary)) {
            return Result<std::string>::fail({readErrorCode, "Failed to open " + description});
        }
        std::istream input(&file);

        std::string contents;
        std::array<char, 64 * 1024> buffer{};
        std::uint64_t consumed = 0;
        std::optional<Error> failure;
        for (;;) {
            input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            const auto count = input.gcount();
            if (count > 0) {
                const auto bytes = static_cast<std::uint64_t>(count);
                if (bytes > maxBytes - consumed) {
                    failure = Error{ErrorCode::ResourceLimitExceeded, description + " exceeds its byte limit"};
                    break;
                }
                contents.append(buffer.data(), static_cast<std::size_t>(count));
                consumed += bytes;
            }
            if (input.bad() || (input.fail() && !input.eof())) {
                failure = Error{readErrorCode, "Failed to read " + description};
                break;
            }
            if (input.eof()) {
                break;
            }
        }
        const bool closed = file.close() != nullptr;
        if (failure) {
            if (!closed) {
                failure->message += "; failed to close " + description;
            }
            return Result<std::string>::fail(std::move(*failure));
        }
        if (!closed) {
            return Result<std::string>::fail({readErrorCode, "Failed to close " + description});
        }
        return Result<std::string>::ok(std::move(contents));
    } catch (...) {
        Error error{readErrorCode, "Failed to read " + description};
        if (file.is_open() && file.close() == nullptr) {
            error.message += "; failed to close " + description;
        }
        return Result<std::string>::fail(std::move(error));
    }
}

} // namespace autoupdater::util
