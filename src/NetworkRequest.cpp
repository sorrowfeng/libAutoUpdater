#include "NetworkRequest.h"

#include "NetworkLimits.h"

#include <algorithm>
#include <optional>
#include <set>
#include <string>
#include <utility>

namespace autoupdater {

namespace {

class BoundedRootedFile final : public IRootedFile {
  public:
    BoundedRootedFile(IRootedFile& delegate, std::uint64_t position, std::uint64_t limit)
        : delegate_(delegate), position_(position), limit_(limit), writeBudget_(limit - position) {}

    Result<std::size_t> read(void* buffer, std::size_t size) noexcept override {
        if (position_ > limit_) {
            return exceeded<std::size_t>();
        }
        const auto remaining = limit_ - position_;
        const auto requested =
            static_cast<std::uint64_t>(size) > remaining ? static_cast<std::size_t>(remaining) : size;
        auto result = delegate_.read(buffer, requested);
        if (result) {
            position_ += static_cast<std::uint64_t>(result.value());
        }
        return result;
    }

    Result<void> write(const void* data, std::size_t size) noexcept override {
        if (position_ > limit_ || static_cast<std::uint64_t>(size) > limit_ - position_ ||
            static_cast<std::uint64_t>(size) > writeBudget_ - bytesWritten_) {
            limitExceeded_ = true;
            return Result<void>::fail(limitError());
        }
        auto result = delegate_.write(data, size);
        if (result) {
            position_ += static_cast<std::uint64_t>(size);
            bytesWritten_ += static_cast<std::uint64_t>(size);
        }
        return result;
    }

    Result<void> seek(std::uint64_t offset) noexcept override {
        if (offset > limit_) {
            limitExceeded_ = true;
            return Result<void>::fail(limitError());
        }
        auto result = delegate_.seek(offset);
        if (result) {
            position_ = offset;
        }
        return result;
    }

    Result<void> truncate(std::uint64_t size) noexcept override {
        if (size > limit_) {
            limitExceeded_ = true;
            return Result<void>::fail(limitError());
        }
        return delegate_.truncate(size);
    }

    Result<void> flush() noexcept override {
        return delegate_.flush();
    }

    Result<RootedFileMetadata> metadata() noexcept override {
        return delegate_.metadata();
    }

    Result<void> setPermissions(std::filesystem::perms permissions) noexcept override {
        return delegate_.setPermissions(permissions);
    }

    bool limitExceeded() const noexcept {
        return limitExceeded_;
    }

    std::uint64_t bytesWritten() const noexcept {
        return bytesWritten_;
    }

  private:
    static Error limitError() {
        return {ErrorCode::ResourceLimitExceeded, "Artifact response exceeds its signed byte limit"};
    }

    template <typename T> Result<T> exceeded() noexcept {
        limitExceeded_ = true;
        return Result<T>::fail(limitError());
    }

    IRootedFile& delegate_;
    std::uint64_t position_ = 0;
    std::uint64_t limit_ = 0;
    std::uint64_t writeBudget_ = 0;
    std::uint64_t bytesWritten_ = 0;
    bool limitExceeded_ = false;
};

bool isRedirectStatus(int statusCode) {
    return statusCode == 301 || statusCode == 302 || statusCode == 303 || statusCode == 307 || statusCode == 308;
}

bool isSuccessfulStatus(int statusCode) {
    return statusCode >= 200 && statusCode < 300;
}

bool equalsHeaderName(const std::string& left, const char* right) {
    const std::string expected(right);
    if (left.size() != expected.size()) {
        return false;
    }
    for (std::size_t i = 0; i < left.size(); ++i) {
        const auto leftChar = static_cast<unsigned char>(left[i]);
        const auto rightChar = static_cast<unsigned char>(expected[i]);
        const auto lowerLeft = leftChar >= 'A' && leftChar <= 'Z' ? leftChar + ('a' - 'A') : leftChar;
        const auto lowerRight = rightChar >= 'A' && rightChar <= 'Z' ? rightChar + ('a' - 'A') : rightChar;
        if (lowerLeft != lowerRight) {
            return false;
        }
    }
    return true;
}

std::string trimOptionalWhitespace(std::string value) {
    const auto isWhitespace = [](unsigned char ch) { return ch == ' ' || ch == '\t'; };
    while (!value.empty() && isWhitespace(static_cast<unsigned char>(value.front()))) {
        value.erase(value.begin());
    }
    while (!value.empty() && isWhitespace(static_cast<unsigned char>(value.back()))) {
        value.pop_back();
    }
    return value;
}

Result<std::string> uniqueLocation(const NetworkResponseInfo& response) {
    std::optional<std::string> location;
    for (const auto& header : response.headers) {
        if (!equalsHeaderName(header.name, "location")) {
            continue;
        }
        if (location) {
            return Result<std::string>::fail(
                {ErrorCode::SecurityPolicyViolation, "Redirect response contains multiple Location headers"});
        }
        location = trimOptionalWhitespace(header.value);
    }
    if (!location || location->empty()) {
        return Result<std::string>::fail(
            {ErrorCode::SecurityPolicyViolation, "Redirect response is missing a Location header"});
    }
    return Result<std::string>::ok(std::move(*location));
}

Result<void> validateSingleHopEffectiveUrl(const util::ParsedUrl& requested, const NetworkResponseInfo& response,
                                           const UrlPolicy& policy) {
    if (response.effectiveUrl.empty()) {
        return Result<void>::fail(
            {ErrorCode::SecurityPolicyViolation, "Network adapter did not report an effective URL"});
    }
    auto effective = policy.authorize(response.effectiveUrl);
    if (!effective) {
        return Result<void>::fail(effective.error());
    }
    if (!util::urlsEquivalent(requested, effective.value())) {
        return Result<void>::fail(
            {ErrorCode::SecurityPolicyViolation, "Network adapter followed or rewrote a URL unexpectedly"});
    }
    return Result<void>::ok();
}

Result<util::ParsedUrl> nextRedirect(const util::ParsedUrl& current, const NetworkResponseInfo& response,
                                     const UrlPolicy& policy, std::set<std::string>& visited, std::size_t redirectCount,
                                     std::size_t maxRedirects) {
    if (redirectCount >= maxRedirects) {
        return Result<util::ParsedUrl>::fail({ErrorCode::SecurityPolicyViolation, "Redirect limit exceeded"});
    }
    auto location = uniqueLocation(response);
    if (!location) {
        return Result<util::ParsedUrl>::fail(location.error());
    }
    auto next = policy.resolveAndAuthorize(current, location.value());
    if (!next) {
        return next;
    }
    if (!visited.insert(next.value().canonical).second) {
        return Result<util::ParsedUrl>::fail({ErrorCode::SecurityPolicyViolation, "Redirect loop detected"});
    }
    return next;
}

Result<std::uint64_t> fileSize(IRootedFile& target) {
    auto metadata = target.metadata();
    if (!metadata) {
        return Result<std::uint64_t>::fail(metadata.error());
    }
    return Result<std::uint64_t>::ok(metadata.value().size);
}

Result<void> restoreFileSize(IRootedFile& target, std::uint64_t size) {
    auto current = fileSize(target);
    if (!current) {
        return Result<void>::fail(current.error());
    }
    if (current.value() != size) {
        auto truncated = target.truncate(size);
        if (!truncated) {
            return truncated;
        }
    }
    return target.seek(size);
}

Error preferResourceLimitError(Error primary, const Result<void>& cleanup) {
    if (cleanup || primary.code != ErrorCode::ResourceLimitExceeded) {
        return cleanup ? std::move(primary) : cleanup.error();
    }
    primary.message += "; failed to restore the partial file: " + cleanup.error().message;
    return primary;
}

Error httpError(ErrorCode code, int statusCode) {
    return {code, "HTTP error " + std::to_string(statusCode)};
}

} // namespace

Result<RedirectedTextResult> fetchTextWithRedirects(const std::string& initialUrl, const NetworkOptions& options,
                                                    std::uint64_t maxResponseBytes, const UrlPolicy& policy,
                                                    INetworkClient& network, CancellationToken& cancel) noexcept {
    try {
        auto current = policy.authorize(initialUrl);
        if (!current) {
            return Result<RedirectedTextResult>::fail(current.error());
        }
        std::set<std::string> visited{current.value().canonical};
        std::size_t redirectCount = 0;

        for (;;) {
            auto response = network.getText(current.value().canonical, options, maxResponseBytes, cancel);
            if (!response) {
                return Result<RedirectedTextResult>::fail(response.error());
            }
            auto headerBudget = detail::validateResponseHeadersBudget(response.value().response);
            if (!headerBudget) {
                return Result<RedirectedTextResult>::fail(headerBudget.error());
            }
            const auto actualBytes = static_cast<std::uint64_t>(response.value().body.size());
            if (actualBytes > maxResponseBytes) {
                return Result<RedirectedTextResult>::fail(
                    {ErrorCode::ResourceLimitExceeded, "Response body exceeds its byte limit"});
            }
            if (response.value().response.statusCode == 200) {
                auto budget = detail::validateResponseBodyBudget(response.value().response, actualBytes,
                                                                 maxResponseBytes, ErrorCode::NetworkError);
                if (!budget) {
                    return Result<RedirectedTextResult>::fail(budget.error());
                }
            }
            auto effective = validateSingleHopEffectiveUrl(current.value(), response.value().response, policy);
            if (!effective) {
                return Result<RedirectedTextResult>::fail(effective.error());
            }
            if (isRedirectStatus(response.value().response.statusCode)) {
                auto next = nextRedirect(current.value(), response.value().response, policy, visited, redirectCount,
                                         options.maxRedirects);
                if (!next) {
                    return Result<RedirectedTextResult>::fail(next.error());
                }
                current = std::move(next);
                ++redirectCount;
                continue;
            }
            if (response.value().response.statusCode != 200) {
                return Result<RedirectedTextResult>::fail(
                    httpError(ErrorCode::NetworkError, response.value().response.statusCode));
            }

            RedirectedTextResult result;
            result.body = std::move(response.value().body);
            result.effectiveUrl = current.value().canonical;
            result.response = std::move(response.value().response);
            return Result<RedirectedTextResult>::ok(std::move(result));
        }
    } catch (...) {
        return Result<RedirectedTextResult>::fail(
            {ErrorCode::NetworkError, "Unexpected redirected text request failure"});
    }
}

Result<RedirectedDownloadResult> downloadWithRedirects(const std::string& initialUrl, IRootedFile& target,
                                                       const NetworkOptions& options, std::uint64_t maxTotalBytes,
                                                       const UrlPolicy& policy, INetworkClient& network,
                                                       const std::optional<DownloadResumeInfo>& resume,
                                                       ProgressCallback progress, CancellationToken& cancel) noexcept {
    try {
        auto current = policy.authorize(initialUrl);
        if (!current) {
            return Result<RedirectedDownloadResult>::fail(current.error());
        }
        std::set<std::string> visited{current.value().canonical};
        std::size_t redirectCount = 0;
        auto activeResume = resume;

        if (activeResume && activeResume->offset > maxTotalBytes) {
            return Result<RedirectedDownloadResult>::fail(
                {ErrorCode::ResourceLimitExceeded, "Resume offset exceeds the signed artifact size"});
        }

        for (;;) {
            auto sizeBeforeRequest = fileSize(target);
            if (!sizeBeforeRequest) {
                return Result<RedirectedDownloadResult>::fail(sizeBeforeRequest.error());
            }
            if (sizeBeforeRequest.value() > maxTotalBytes) {
                return Result<RedirectedDownloadResult>::fail(
                    {ErrorCode::ResourceLimitExceeded, "Partial artifact exceeds the signed artifact size"});
            }
            const bool requestIsResume = activeResume && activeResume->offset > 0;
            if (requestIsResume && activeResume->offset != sizeBeforeRequest.value()) {
                return Result<RedirectedDownloadResult>::fail(
                    {ErrorCode::DownloadFailed, "Resume offset does not match the partial artifact size"});
            }
            const auto initialBytes = requestIsResume ? activeResume->offset : sizeBeforeRequest.value();
            auto remaining = detail::remainingTransferBudget(initialBytes, maxTotalBytes);
            if (!remaining) {
                return Result<RedirectedDownloadResult>::fail(remaining.error());
            }
            BoundedRootedFile boundedTarget(target, initialBytes, maxTotalBytes);
            auto response = network.downloadToFile(current.value().canonical, boundedTarget, options, maxTotalBytes,
                                                   activeResume, progress, cancel);
            if (!response) {
                if (boundedTarget.limitExceeded() || response.error().code == ErrorCode::ResourceLimitExceeded) {
                    auto restored = restoreFileSize(target, sizeBeforeRequest.value());
                    const auto error =
                        boundedTarget.limitExceeded()
                            ? Error{ErrorCode::ResourceLimitExceeded, "Artifact response exceeds its signed byte limit"}
                            : response.error();
                    return Result<RedirectedDownloadResult>::fail(preferResourceLimitError(error, restored));
                }
                // Transport failures may leave a resumable partial body. The
                // caller owns retry and persistence policy for that data.
                return Result<RedirectedDownloadResult>::fail(response.error());
            }

            auto headerBudget = detail::validateResponseHeadersBudget(response.value().response);
            if (!headerBudget) {
                auto restored = restoreFileSize(target, sizeBeforeRequest.value());
                return Result<RedirectedDownloadResult>::fail(preferResourceLimitError(headerBudget.error(), restored));
            }

            if (boundedTarget.limitExceeded()) {
                auto restored = restoreFileSize(target, sizeBeforeRequest.value());
                return Result<RedirectedDownloadResult>::fail(preferResourceLimitError(
                    {ErrorCode::ResourceLimitExceeded, "Artifact response exceeds its signed byte limit"}, restored));
            }

            const int writableStatus =
                current.value().scheme == util::UrlScheme::File ? 200 : (requestIsResume ? 206 : 200);
            const bool responseCanWrite = response.value().response.statusCode == writableStatus;
            if (responseCanWrite) {
                auto budget =
                    detail::validateResponseBodyBudget(response.value().response, boundedTarget.bytesWritten(),
                                                       remaining.value(), ErrorCode::DownloadFailed);
                if (!budget) {
                    auto restored = restoreFileSize(target, sizeBeforeRequest.value());
                    return Result<RedirectedDownloadResult>::fail(preferResourceLimitError(budget.error(), restored));
                }
            }

            auto effective = validateSingleHopEffectiveUrl(current.value(), response.value().response, policy);
            if (!effective) {
                auto restored = restoreFileSize(target, sizeBeforeRequest.value());
                return Result<RedirectedDownloadResult>::fail(restored ? effective.error() : restored.error());
            }
            if (isRedirectStatus(response.value().response.statusCode)) {
                // Validators are scoped to the resource that produced them.
                // Never forward Range/If-Range across a redirect. Restart the
                // target from zero before issuing the next request.
                auto restored = restoreFileSize(target, activeResume ? 0 : sizeBeforeRequest.value());
                if (!restored) {
                    return Result<RedirectedDownloadResult>::fail(restored.error());
                }
                activeResume.reset();
                auto next = nextRedirect(current.value(), response.value().response, policy, visited, redirectCount,
                                         options.maxRedirects);
                if (!next) {
                    return Result<RedirectedDownloadResult>::fail(next.error());
                }
                current = std::move(next);
                ++redirectCount;
                continue;
            }
            if (!isSuccessfulStatus(response.value().response.statusCode)) {
                auto restored = restoreFileSize(target, sizeBeforeRequest.value());
                if (!restored) {
                    return Result<RedirectedDownloadResult>::fail(restored.error());
                }
                return Result<RedirectedDownloadResult>::fail(
                    httpError(ErrorCode::DownloadFailed, response.value().response.statusCode));
            }
            const bool resuming = activeResume && activeResume->offset > 0;
            if (resuming && current.value().scheme != util::UrlScheme::File &&
                response.value().response.statusCode == 200) {
                // A 200 response means the server ignored Range or invalidated
                // If-Range. Bundled adapters deliberately do not write that
                // response while configured for append mode. Discard the stale
                // partial body and validator, then retry this URL as a full
                // download within the same coordinated request.
                auto restarted = restoreFileSize(target, 0);
                if (!restarted) {
                    return Result<RedirectedDownloadResult>::fail(restarted.error());
                }
                activeResume.reset();
                continue;
            }
            const int expectedSuccessStatus =
                current.value().scheme == util::UrlScheme::File ? 200 : (resuming ? 206 : 200);
            if (response.value().response.statusCode != expectedSuccessStatus) {
                auto restored = restoreFileSize(target, sizeBeforeRequest.value());
                if (!restored) {
                    return Result<RedirectedDownloadResult>::fail(restored.error());
                }
                const auto message = resuming && current.value().scheme != util::UrlScheme::File
                                         ? "Server ignored or rejected Range request"
                                         : "Unexpected successful response status for a full download";
                return Result<RedirectedDownloadResult>::fail({ErrorCode::DownloadFailed, message});
            }

            RedirectedDownloadResult result;
            result.effectiveUrl = current.value().canonical;
            result.download = std::move(response.value());
            return Result<RedirectedDownloadResult>::ok(std::move(result));
        }
    } catch (...) {
        return Result<RedirectedDownloadResult>::fail(
            {ErrorCode::DownloadFailed, "Unexpected redirected download failure"});
    }
}

} // namespace autoupdater
