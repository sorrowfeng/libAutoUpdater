#include "libAutoUpdater/interfaces/INetworkClient.h"

#ifdef LIBAUTOUPDATER_HAS_CFNETWORK

#include "NetworkLimits.h"
#include "default/LocalNetworkFile.h"

#include <CoreFoundation/CoreFoundation.h>
#include <CFNetwork/CFNetwork.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <string>
#include <utility>
#include <vector>

namespace autoupdater {

namespace {

constexpr std::size_t kMaxPreHeaderBodyBytes = 64 * 1024;
constexpr std::size_t kResponseReadBufferBytes = 64 * 1024;

template <class T> class CfRef {
  public:
    CfRef() = default;
    explicit CfRef(T value) : value_(value) {}
    ~CfRef() {
        if (value_) {
            CFRelease(value_);
        }
    }

    CfRef(const CfRef&) = delete;
    CfRef& operator=(const CfRef&) = delete;

    CfRef(CfRef&& other) noexcept : value_(std::exchange(other.value_, nullptr)) {}
    CfRef& operator=(CfRef&& other) noexcept {
        if (this != &other) {
            if (value_) {
                CFRelease(value_);
            }
            value_ = std::exchange(other.value_, nullptr);
        }
        return *this;
    }

    T get() const noexcept {
        return value_;
    }
    explicit operator bool() const noexcept {
        return value_ != nullptr;
    }

  private:
    T value_ = nullptr;
};

std::string cfStringToStd(CFStringRef value) {
    if (!value) {
        return {};
    }
    if (const char* direct = CFStringGetCStringPtr(value, kCFStringEncodingUTF8)) {
        return direct;
    }

    const auto length = CFStringGetLength(value);
    const auto maxSize = CFStringGetMaximumSizeForEncoding(length, kCFStringEncodingUTF8) + 1;
    std::string output(static_cast<std::size_t>(maxSize), '\0');
    if (!CFStringGetCString(value, output.data(), maxSize, kCFStringEncodingUTF8)) {
        return {};
    }
    output.resize(std::char_traits<char>::length(output.c_str()));
    return output;
}

CfRef<CFStringRef> makeCfString(const std::string& value) {
    return CfRef<CFStringRef>(CFStringCreateWithCString(kCFAllocatorDefault, value.c_str(), kCFStringEncodingUTF8));
}

std::string streamError(CFReadStreamRef stream, const char* action) {
    const CFStreamError error = CFReadStreamGetError(stream);
    return std::string(action) + " failed (domain " + std::to_string(error.domain) + ", error " +
           std::to_string(error.error) + ")";
}

bool isHttpUrl(const std::string& url) {
    const auto separator = url.find("://");
    if (separator == std::string::npos) {
        return false;
    }
    auto scheme = url.substr(0, separator);
    std::transform(scheme.begin(), scheme.end(), scheme.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return scheme == "http" || scheme == "https";
}

struct HttpRequest {
    CfRef<CFHTTPMessageRef> message;
    bool secure = false;
};

Result<HttpRequest> makeRequest(const std::string& url) {
    auto cfUrl =
        CfRef<CFURLRef>(CFURLCreateWithBytes(kCFAllocatorDefault, reinterpret_cast<const UInt8*>(url.data()),
                                             static_cast<CFIndex>(url.size()), kCFStringEncodingUTF8, nullptr));
    if (!cfUrl) {
        return Result<HttpRequest>::fail({ErrorCode::NetworkError, "Invalid URL"});
    }

    auto request = CfRef<CFHTTPMessageRef>(
        CFHTTPMessageCreateRequest(kCFAllocatorDefault, CFSTR("GET"), cfUrl.get(), kCFHTTPVersion1_1));
    if (!request) {
        return Result<HttpRequest>::fail({ErrorCode::NetworkError, "Failed to create CFNetwork request"});
    }

    const auto lowerUrl = [&] {
        std::string lower = url;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        return lower;
    }();

    HttpRequest output;
    output.message = std::move(request);
    output.secure = lowerUrl.rfind("https://", 0) == 0;
    return Result<HttpRequest>::ok(std::move(output));
}

void setHeader(CFHTTPMessageRef request, CFStringRef name, const std::string& value) {
    auto text = makeCfString(value);
    if (text) {
        CFHTTPMessageSetHeaderFieldValue(request, name, text.get());
    }
}

Result<CfRef<CFReadStreamRef>> openStream(const HttpRequest& request, const NetworkOptions& options) {
    auto stream = CfRef<CFReadStreamRef>(CFReadStreamCreateForHTTPRequest(kCFAllocatorDefault, request.message.get()));
    if (!stream) {
        return Result<CfRef<CFReadStreamRef>>::fail({ErrorCode::NetworkError, "Failed to create CFNetwork stream"});
    }

    if (!CFReadStreamSetProperty(stream.get(), kCFStreamPropertyHTTPShouldAutoredirect, kCFBooleanFalse)) {
        return Result<CfRef<CFReadStreamRef>>::fail(
            {ErrorCode::NetworkError, "Failed to disable CFNetwork automatic redirects"});
    }

    if (request.secure && !options.verifyTls) {
        const void* keys[] = {kCFStreamSSLValidatesCertificateChain};
        const void* values[] = {kCFBooleanFalse};
        auto sslSettings = CfRef<CFDictionaryRef>(CFDictionaryCreate(
            kCFAllocatorDefault, keys, values, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks));
        if (sslSettings) {
            CFReadStreamSetProperty(stream.get(), kCFStreamPropertySSLSettings, sslSettings.get());
        }
    }

    if (!CFReadStreamOpen(stream.get())) {
        return Result<CfRef<CFReadStreamRef>>::fail(
            {ErrorCode::NetworkError, streamError(stream.get(), "CFReadStreamOpen")});
    }
    return Result<CfRef<CFReadStreamRef>>::ok(std::move(stream));
}

CfRef<CFHTTPMessageRef> copyResponseHeaders(CFReadStreamRef stream) {
    const auto value = CFReadStreamCopyProperty(stream, kCFStreamPropertyHTTPResponseHeader);
    if (!value) {
        return {};
    }
    auto response = static_cast<CFHTTPMessageRef>(const_cast<void*>(value));
    return CfRef<CFHTTPMessageRef>(response);
}

void trimOptionalWhitespace(std::string& value) {
    const auto begin = value.find_first_not_of(" \t");
    if (begin == std::string::npos) {
        value.clear();
        return;
    }
    const auto end = value.find_last_not_of(" \t");
    value = value.substr(begin, end - begin + 1);
}

Result<std::vector<NetworkHeader>> responseHeaders(CFHTTPMessageRef response) {
    std::vector<NetworkHeader> headers;
    auto serialized = CfRef<CFDataRef>(CFHTTPMessageCopySerializedMessage(response));
    if (!serialized) {
        return Result<std::vector<NetworkHeader>>::fail(
            {ErrorCode::NetworkError, "Failed to serialize CFNetwork response headers"});
    }

    const auto length = CFDataGetLength(serialized.get());
    const auto* bytes = CFDataGetBytePtr(serialized.get());
    if (!bytes || length <= 0) {
        return Result<std::vector<NetworkHeader>>::fail(
            {ErrorCode::NetworkError, "CFNetwork response headers are empty"});
    }
    if (static_cast<std::uint64_t>(length) > detail::kMaxNetworkResponseHeaderBytes) {
        return Result<std::vector<NetworkHeader>>::fail(
            {ErrorCode::ResourceLimitExceeded, "HTTP response headers exceed their byte limit"});
    }

    const std::string raw(reinterpret_cast<const char*>(bytes), static_cast<std::size_t>(length));
    std::size_t position = 0;
    bool firstLine = true;
    while (position <= raw.size()) {
        const auto end = raw.find("\r\n", position);
        const auto count = end == std::string::npos ? raw.size() - position : end - position;
        const auto line = raw.substr(position, count);
        if (!firstLine && !line.empty()) {
            const auto colon = line.find(':');
            if (colon != std::string::npos) {
                auto name = line.substr(0, colon);
                auto value = line.substr(colon + 1);
                std::transform(name.begin(), name.end(), name.begin(),
                               [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
                trimOptionalWhitespace(value);
                headers.push_back({std::move(name), std::move(value)});
            }
        }
        firstLine = false;
        if (end == std::string::npos || count == 0) {
            break;
        }
        position = end + 2;
    }
    return Result<std::vector<NetworkHeader>>::ok(std::move(headers));
}

std::string responseHeader(const NetworkResponseInfo& response, const std::string& name) {
    for (const auto& header : response.headers) {
        if (header.name == name) {
            return header.value;
        }
    }
    return {};
}

Result<void> validateIdentityContentEncoding(const NetworkResponseInfo& response, ErrorCode errorCode) {
    for (const auto& header : response.headers) {
        if (header.name != "content-encoding") {
            continue;
        }
        auto value = header.value;
        trimOptionalWhitespace(value);
        std::transform(value.begin(), value.end(), value.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        if (!value.empty() && value != "identity") {
            return Result<void>::fail(
                {errorCode, "Unsupported Content-Encoding; update responses must use identity encoding"});
        }
    }
    return Result<void>::ok();
}

Result<NetworkResponseInfo> responseInfo(CFReadStreamRef stream, CFHTTPMessageRef headers,
                                         const std::string& requestedUrl) {
    NetworkResponseInfo response;
    response.statusCode = static_cast<int>(CFHTTPMessageGetResponseStatusCode(headers));
    auto parsedHeaders = responseHeaders(headers);
    if (!parsedHeaders) {
        return Result<NetworkResponseInfo>::fail(parsedHeaders.error());
    }
    response.headers = std::move(parsedHeaders.value());
    response.effectiveUrl = requestedUrl;

    auto finalUrl = CfRef<CFTypeRef>(CFReadStreamCopyProperty(stream, kCFStreamPropertyHTTPFinalURL));
    if (finalUrl && CFGetTypeID(finalUrl.get()) == CFURLGetTypeID()) {
        const auto url = reinterpret_cast<CFURLRef>(finalUrl.get());
        const auto serialized = cfStringToStd(CFURLGetString(url));
        if (!serialized.empty()) {
            response.effectiveUrl = serialized;
        }
    }
    return Result<NetworkResponseInfo>::ok(std::move(response));
}

struct ResponseStart {
    CfRef<CFHTTPMessageRef> headers;
    std::vector<char> bufferedBody;
};

Result<ResponseStart> waitForResponse(CFReadStreamRef stream, std::uint64_t maxBufferedBodyBytes,
                                      ErrorCode readErrorCode, CancellationToken& cancel) {
    ResponseStart start;
    std::array<UInt8, kResponseReadBufferBytes> buffer{};

    for (;;) {
        if (cancel.isCancelled()) {
            return Result<ResponseStart>::fail({ErrorCode::Cancelled, "Operation cancelled"});
        }

        if (auto headers = copyResponseHeaders(stream)) {
            start.headers = std::move(headers);
            return Result<ResponseStart>::ok(std::move(start));
        }

        const auto buffered = static_cast<std::uint64_t>(start.bufferedBody.size());
        const auto remaining = maxBufferedBodyBytes - buffered;
        std::size_t requested = buffer.size();
        if (remaining < requested) {
            requested = static_cast<std::size_t>(remaining + 1);
        }
        const CFIndex count = CFReadStreamRead(stream, buffer.data(), static_cast<CFIndex>(requested));
        if (count < 0) {
            return Result<ResponseStart>::fail({readErrorCode, streamError(stream, "CFReadStreamRead")});
        }
        if (count == 0) {
            return Result<ResponseStart>::fail({readErrorCode, "No HTTP response headers received"});
        }

        if (static_cast<std::uint64_t>(count) > remaining) {
            return Result<ResponseStart>::fail(
                {ErrorCode::ResourceLimitExceeded, "Response body exceeds its pre-header buffer limit"});
        }
        const auto* begin = reinterpret_cast<const char*>(buffer.data());
        start.bufferedBody.insert(start.bufferedBody.end(), begin, begin + count);
    }
}

Result<void> validateCompletedBody(const NetworkResponseInfo& response, std::uint64_t actualBytes,
                                   std::uint64_t maxResponseBytes, ErrorCode invalidHeaderCode) {
    auto budget = detail::validateResponseBodyBudget(response, actualBytes, maxResponseBytes, invalidHeaderCode);
    if (!budget) {
        return budget;
    }
    auto declared = detail::declaredContentLength(response, invalidHeaderCode);
    if (!declared) {
        return Result<void>::fail(declared.error());
    }
    if (declared.value() && *declared.value() != actualBytes) {
        return Result<void>::fail({invalidHeaderCode, "Content-Length does not match the received response body"});
    }
    return Result<void>::ok();
}

struct ReadResponseResult {
    std::string body;
    std::uint64_t actualBytes = 0;
};

Result<ReadResponseResult> readResponse(CFReadStreamRef stream, const NetworkResponseInfo& response,
                                        std::uint64_t maxResponseBytes, std::uint64_t initialBytes,
                                        std::uint64_t totalBytes, ErrorCode readErrorCode, CancellationToken& cancel,
                                        ProgressCallback progress, const std::string& currentFile, IRootedFile* output,
                                        const std::vector<char>& initialBody = {}) {
    auto encoding = validateIdentityContentEncoding(response, readErrorCode);
    if (!encoding) {
        return Result<ReadResponseResult>::fail(encoding.error());
    }
    auto declaredBudget = detail::validateResponseBodyBudget(response, 0, maxResponseBytes, readErrorCode);
    if (!declaredBudget) {
        return Result<ReadResponseResult>::fail(declaredBudget.error());
    }
    auto declared = detail::declaredContentLength(response, readErrorCode);
    if (!declared) {
        return Result<ReadResponseResult>::fail(declared.error());
    }
    const auto bodyLimit = declared.value().value_or(maxResponseBytes);

    ReadResponseResult result;
    std::array<UInt8, kResponseReadBufferBytes> buffer{};

    const auto consume = [&](const char* data, std::size_t count) -> Result<void> {
        const auto bytes = static_cast<std::uint64_t>(count);
        if (bytes > bodyLimit - result.actualBytes) {
            return Result<void>::fail({declared.value() ? readErrorCode : ErrorCode::ResourceLimitExceeded,
                                       declared.value() ? "Response body exceeds its declared Content-Length"
                                                        : "Response body exceeds its byte limit"});
        }
        if (output) {
            auto written = output->write(data, count);
            if (!written) {
                return written;
            }
        } else {
            result.body.append(data, count);
        }

        result.actualBytes += bytes;
        if (progress) {
            std::uint64_t downloaded = 0;
            if (!detail::checkedAdd(initialBytes, result.actualBytes, downloaded)) {
                return Result<void>::fail({ErrorCode::ResourceLimitExceeded, "Download byte counter overflow"});
            }
            progress({downloaded, totalBytes, currentFile});
        }
        return Result<void>::ok();
    };

    if (!initialBody.empty()) {
        auto consumed = consume(initialBody.data(), initialBody.size());
        if (!consumed) {
            return Result<ReadResponseResult>::fail(consumed.error());
        }
    }

    for (;;) {
        if (cancel.isCancelled()) {
            return Result<ReadResponseResult>::fail({ErrorCode::Cancelled, "Operation cancelled"});
        }

        const auto remaining = bodyLimit - result.actualBytes;
        std::size_t requested = buffer.size();
        if (remaining < requested) {
            requested = static_cast<std::size_t>(remaining + 1);
        }
        const CFIndex count = CFReadStreamRead(stream, buffer.data(), static_cast<CFIndex>(requested));
        if (count < 0) {
            return Result<ReadResponseResult>::fail({readErrorCode, streamError(stream, "CFReadStreamRead")});
        }
        if (count == 0) {
            break;
        }

        auto consumed = consume(reinterpret_cast<const char*>(buffer.data()), static_cast<std::size_t>(count));
        if (!consumed) {
            return Result<ReadResponseResult>::fail(consumed.error());
        }
    }

    auto completed = validateCompletedBody(response, result.actualBytes, maxResponseBytes, readErrorCode);
    if (!completed) {
        return Result<ReadResponseResult>::fail(completed.error());
    }
    return Result<ReadResponseResult>::ok(std::move(result));
}

void setResumeHeaders(CFHTTPMessageRef request, const NetworkOptions& options,
                      const std::optional<DownloadResumeInfo>& resume) {
    if (!options.enableResume || !resume || resume->offset == 0) {
        return;
    }

    setHeader(request, CFSTR("Range"), "bytes=" + std::to_string(resume->offset) + "-");
    if (!resume->etag.empty()) {
        setHeader(request, CFSTR("If-Range"), resume->etag);
    } else if (!resume->lastModified.empty()) {
        setHeader(request, CFSTR("If-Range"), resume->lastModified);
    }
}

class CfNetworkClient final : public INetworkClient {
  public:
    Result<TextResponse> getText(const std::string& url, const NetworkOptions& options, std::uint64_t maxResponseBytes,
                                 CancellationToken& cancel) noexcept override {
        if (!isHttpUrl(url)) {
            return detail::readLocalText(url, maxResponseBytes, cancel);
        }

        try {
            auto request = makeRequest(url);
            if (!request) {
                return Result<TextResponse>::fail(request.error());
            }
            setHeader(request.value().message.get(), CFSTR("Accept-Encoding"), "identity");
            auto stream = openStream(request.value(), options);
            if (!stream) {
                return Result<TextResponse>::fail(stream.error());
            }
            const auto preHeaderLimit = std::min<std::uint64_t>(maxResponseBytes, kMaxPreHeaderBodyBytes);
            auto response = waitForResponse(stream.value().get(), preHeaderLimit, ErrorCode::NetworkError, cancel);
            if (!response) {
                return Result<TextResponse>::fail(response.error());
            }
            auto info = responseInfo(stream.value().get(), response.value().headers.get(), url);
            if (!info) {
                return Result<TextResponse>::fail(info.error());
            }
            TextResponse result;
            result.response = std::move(info.value());
            if (result.response.statusCode == 200) {
                auto bytes =
                    readResponse(stream.value().get(), result.response, maxResponseBytes, 0, maxResponseBytes,
                                 ErrorCode::NetworkError, cancel, {}, {}, nullptr, response.value().bufferedBody);
                if (!bytes) {
                    return Result<TextResponse>::fail(bytes.error());
                }
                result.body = std::move(bytes.value().body);
            }
            return Result<TextResponse>::ok(std::move(result));
        } catch (...) {
            return Result<TextResponse>::fail({ErrorCode::NetworkError, "Unexpected CFNetwork request failure"});
        }
    }

    Result<DownloadResult> downloadToFile(const std::string& url, IRootedFile& target, const NetworkOptions& options,
                                          std::uint64_t maxTotalBytes, const std::optional<DownloadResumeInfo>& resume,
                                          ProgressCallback progress, CancellationToken& cancel) noexcept override {
        if (!isHttpUrl(url)) {
            const auto effectiveResume = options.enableResume ? resume : std::nullopt;
            return detail::copyLocalToFile(url, target, maxTotalBytes, effectiveResume, std::move(progress), cancel);
        }

        try {
            const bool appending = options.enableResume && resume && resume->offset > 0;
            const auto initialBytes = appending ? resume->offset : 0;
            auto remaining = detail::remainingTransferBudget(initialBytes, maxTotalBytes);
            if (!remaining) {
                return Result<DownloadResult>::fail(remaining.error());
            }
            auto request = makeRequest(url);
            if (!request) {
                return Result<DownloadResult>::fail(request.error());
            }
            setHeader(request.value().message.get(), CFSTR("Accept-Encoding"), "identity");
            setResumeHeaders(request.value().message.get(), options, resume);

            auto stream = openStream(request.value(), options);
            if (!stream) {
                return Result<DownloadResult>::fail(stream.error());
            }
            const auto preHeaderLimit = std::min<std::uint64_t>(remaining.value(), kMaxPreHeaderBodyBytes);
            auto response = waitForResponse(stream.value().get(), preHeaderLimit, ErrorCode::DownloadFailed, cancel);
            if (!response) {
                return Result<DownloadResult>::fail(response.error());
            }

            auto info = responseInfo(stream.value().get(), response.value().headers.get(), url);
            if (!info) {
                return Result<DownloadResult>::fail(info.error());
            }
            const int writableStatus = appending ? 206 : 200;
            std::uint64_t responseBytes = 0;
            if (info.value().statusCode == writableStatus) {
                auto bytes = readResponse(stream.value().get(), info.value(), remaining.value(), initialBytes,
                                          maxTotalBytes, ErrorCode::DownloadFailed, cancel, std::move(progress), {},
                                          &target, response.value().bufferedBody);
                if (!bytes) {
                    return Result<DownloadResult>::fail(bytes.error());
                }
                responseBytes = bytes.value().actualBytes;
            } else if (cancel.isCancelled()) {
                return Result<DownloadResult>::fail({ErrorCode::Cancelled, "Operation cancelled"});
            }

            DownloadResult result;
            result.response = std::move(info.value());
            if (!detail::checkedAdd(initialBytes, responseBytes, result.bytesWritten)) {
                return Result<DownloadResult>::fail(
                    {ErrorCode::ResourceLimitExceeded, "Download byte counter overflow"});
            }
            result.etag = responseHeader(result.response, "etag");
            result.lastModified = responseHeader(result.response, "last-modified");
            return Result<DownloadResult>::ok(std::move(result));
        } catch (...) {
            return Result<DownloadResult>::fail({ErrorCode::DownloadFailed, "Unexpected CFNetwork download failure"});
        }
    }
};

} // namespace

std::shared_ptr<INetworkClient> createDefaultNetworkClient() {
    return std::make_shared<CfNetworkClient>();
}

} // namespace autoupdater

#endif
