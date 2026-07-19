#include "NetworkLimits.h"

#include <cctype>
#include <limits>
#include <string>

namespace autoupdater::detail {

namespace {

bool equalsHeaderName(const std::string& left, const char* right) {
    const std::string expected(right);
    if (left.size() != expected.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.size(); ++index) {
        const auto leftChar = static_cast<unsigned char>(left[index]);
        const auto rightChar = static_cast<unsigned char>(expected[index]);
        if (std::tolower(leftChar) != std::tolower(rightChar)) {
            return false;
        }
    }
    return true;
}

std::string trimOptionalWhitespace(std::string value) {
    const auto first = value.find_first_not_of(" \t");
    if (first == std::string::npos) {
        return {};
    }
    const auto last = value.find_last_not_of(" \t");
    return value.substr(first, last - first + 1);
}

Result<std::uint64_t> parseContentLength(const std::string& raw, ErrorCode invalidHeaderCode) {
    const auto value = trimOptionalWhitespace(raw);
    if (value.empty()) {
        return Result<std::uint64_t>::fail({invalidHeaderCode, "Content-Length is empty"});
    }

    std::uint64_t parsed = 0;
    for (const unsigned char character : value) {
        if (character < '0' || character > '9') {
            return Result<std::uint64_t>::fail({invalidHeaderCode, "Content-Length is invalid"});
        }
        const auto digit = static_cast<std::uint64_t>(character - '0');
        if (parsed > (std::numeric_limits<std::uint64_t>::max() - digit) / 10) {
            return Result<std::uint64_t>::fail(
                {ErrorCode::ResourceLimitExceeded, "Content-Length exceeds the supported byte range"});
        }
        parsed = parsed * 10 + digit;
    }
    return Result<std::uint64_t>::ok(parsed);
}

} // namespace

bool checkedAdd(std::uint64_t left, std::uint64_t right, std::uint64_t& result) noexcept {
    if (right > std::numeric_limits<std::uint64_t>::max() - left) {
        return false;
    }
    result = left + right;
    return true;
}

Result<void> validateResponseHeadersBudget(const NetworkResponseInfo& response) noexcept {
    try {
        static_assert(std::numeric_limits<std::size_t>::digits <= std::numeric_limits<std::uint64_t>::digits,
                      "Header sizes must fit in uint64_t");
        std::uint64_t total = 0;
        for (const auto& header : response.headers) {
            std::uint64_t fieldBytes = 0;
            if (!checkedAdd(static_cast<std::uint64_t>(header.name.size()),
                            static_cast<std::uint64_t>(header.value.size()), fieldBytes) ||
                !checkedAdd(fieldBytes, 4, fieldBytes) || !checkedAdd(total, fieldBytes, total) ||
                total > kMaxNetworkResponseHeaderBytes) {
                return Result<void>::fail(
                    {ErrorCode::ResourceLimitExceeded, "HTTP response headers exceed their byte limit"});
            }
        }
        return Result<void>::ok();
    } catch (...) {
        return Result<void>::fail({ErrorCode::ResourceLimitExceeded, "Failed to validate HTTP response header size"});
    }
}

Result<std::optional<std::uint64_t>> declaredContentLength(const NetworkResponseInfo& response,
                                                           ErrorCode invalidHeaderCode) noexcept {
    try {
        std::optional<std::uint64_t> length;
        for (const auto& header : response.headers) {
            if (!equalsHeaderName(header.name, "content-length")) {
                continue;
            }
            if (length) {
                return Result<std::optional<std::uint64_t>>::fail(
                    {invalidHeaderCode, "Response contains multiple Content-Length headers"});
            }
            auto parsed = parseContentLength(header.value, invalidHeaderCode);
            if (!parsed) {
                return Result<std::optional<std::uint64_t>>::fail(parsed.error());
            }
            length = parsed.value();
        }
        return Result<std::optional<std::uint64_t>>::ok(length);
    } catch (...) {
        return Result<std::optional<std::uint64_t>>::fail({invalidHeaderCode, "Failed to validate Content-Length"});
    }
}

Result<void> validateResponseBodyBudget(const NetworkResponseInfo& response, std::uint64_t actualBytes,
                                        std::uint64_t maxBytes, ErrorCode invalidHeaderCode) noexcept {
    auto headers = validateResponseHeadersBudget(response);
    if (!headers) {
        return headers;
    }
    auto declared = declaredContentLength(response, invalidHeaderCode);
    if (!declared) {
        return Result<void>::fail(declared.error());
    }
    if ((declared.value() && *declared.value() > maxBytes) || actualBytes > maxBytes) {
        return Result<void>::fail({ErrorCode::ResourceLimitExceeded, "Response body exceeds its byte limit"});
    }
    return Result<void>::ok();
}

Result<std::uint64_t> remainingTransferBudget(std::uint64_t initialBytes, std::uint64_t maxTotalBytes) noexcept {
    if (initialBytes > maxTotalBytes) {
        return Result<std::uint64_t>::fail(
            {ErrorCode::ResourceLimitExceeded, "Resume offset exceeds the signed artifact size"});
    }
    return Result<std::uint64_t>::ok(maxTotalBytes - initialBytes);
}

} // namespace autoupdater::detail
