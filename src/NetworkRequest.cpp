#include "NetworkRequest.h"

#include <algorithm>
#include <optional>
#include <set>
#include <string>
#include <utility>

namespace autoupdater {

namespace {

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

Error httpError(ErrorCode code, int statusCode) {
    return {code, "HTTP error " + std::to_string(statusCode)};
}

} // namespace

Result<RedirectedTextResult> fetchTextWithRedirects(const std::string& initialUrl, const NetworkOptions& options,
                                                    const UrlPolicy& policy, INetworkClient& network,
                                                    CancellationToken& cancel) noexcept {
    try {
        auto current = policy.authorize(initialUrl);
        if (!current) {
            return Result<RedirectedTextResult>::fail(current.error());
        }
        std::set<std::string> visited{current.value().canonical};
        std::size_t redirectCount = 0;

        for (;;) {
            auto response = network.getText(current.value().canonical, options, cancel);
            if (!response) {
                return Result<RedirectedTextResult>::fail(response.error());
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
                                                       const NetworkOptions& options, const UrlPolicy& policy,
                                                       INetworkClient& network,
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

        for (;;) {
            auto sizeBeforeRequest = fileSize(target);
            if (!sizeBeforeRequest) {
                return Result<RedirectedDownloadResult>::fail(sizeBeforeRequest.error());
            }
            auto response =
                network.downloadToFile(current.value().canonical, target, options, activeResume, progress, cancel);
            if (!response) {
                // Transport failures may leave a resumable partial body. The
                // caller owns retry and persistence policy for that data.
                return Result<RedirectedDownloadResult>::fail(response.error());
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
