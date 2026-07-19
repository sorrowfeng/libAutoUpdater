#include "libAutoUpdater/interfaces/INetworkClient.h"

#ifdef LIBAUTOUPDATER_HAS_CFNETWORK

#include "util/UrlUtil.h"
#include "util/PathUtil.h"

#include <CoreFoundation/CoreFoundation.h>
#include <CFNetwork/CFNetwork.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace autoupdater {

namespace {

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

Result<std::filesystem::path> localPathFromUrl(const std::string& url) {
    if (util::isFileUrl(url)) {
        return Result<std::filesystem::path>::ok(util::fileUrlToPath(url));
    }
    return Result<std::filesystem::path>::fail(
        {ErrorCode::NetworkError, "CFNetwork accepts only HTTP, HTTPS, and explicit file: URLs"});
}

Result<TextResponse> readLocalText(const std::string& url, CancellationToken& cancel) {
    if (cancel.isCancelled()) {
        return Result<TextResponse>::fail({ErrorCode::Cancelled, "Operation cancelled"});
    }

    auto path = localPathFromUrl(url);
    if (!path) {
        return Result<TextResponse>::fail(path.error());
    }

    try {
        std::ifstream input(path.value(), std::ios::binary);
        if (!input) {
            return Result<TextResponse>::fail({ErrorCode::ManifestDownloadFailed, "Failed to open local source"});
        }
        std::ostringstream stream;
        stream << input.rdbuf();
        if (input.bad()) {
            return Result<TextResponse>::fail({ErrorCode::NetworkError, "Failed to read local source"});
        }
        TextResponse response;
        response.response.statusCode = 200;
        response.response.effectiveUrl = url;
        response.body = stream.str();
        return Result<TextResponse>::ok(std::move(response));
    } catch (...) {
        return Result<TextResponse>::fail({ErrorCode::NetworkError, "Failed to read local source"});
    }
}

Result<DownloadResult> copyLocalToFile(const std::string& url, IRootedFile& target,
                                       const std::optional<DownloadResumeInfo>& resume, ProgressCallback progress,
                                       CancellationToken& cancel) {
    auto source = localPathFromUrl(url);
    if (!source) {
        return Result<DownloadResult>::fail(source.error());
    }

    try {
        std::error_code ec;
        const auto total = std::filesystem::file_size(source.value(), ec);
        if (ec) {
            return Result<DownloadResult>::fail({ErrorCode::DownloadFailed, ec.message()});
        }
        std::ifstream input(source.value(), std::ios::binary);
        if (resume && resume->offset > 0) {
            input.seekg(static_cast<std::streamoff>(resume->offset), std::ios::beg);
        }
        if (!input) {
            return Result<DownloadResult>::fail({ErrorCode::DownloadFailed, "Failed to open local source"});
        }

        std::array<char, 64 * 1024> buffer{};
        std::uint64_t written = resume ? resume->offset : 0;
        while (input) {
            if (cancel.isCancelled()) {
                return Result<DownloadResult>::fail({ErrorCode::Cancelled, "Operation cancelled"});
            }
            input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            const auto count = input.gcount();
            if (count > 0) {
                auto write = target.write(buffer.data(), static_cast<std::size_t>(count));
                if (!write) {
                    return Result<DownloadResult>::fail(write.error());
                }
                written += static_cast<std::uint64_t>(count);
                if (progress) {
                    progress({written, static_cast<std::uint64_t>(total), {}});
                }
            }
        }
        if (input.bad()) {
            return Result<DownloadResult>::fail({ErrorCode::DownloadFailed, "Failed to read local source"});
        }

        DownloadResult result;
        result.response.statusCode = 200;
        result.response.effectiveUrl = url;
        result.bytesWritten = written;
        return Result<DownloadResult>::ok(result);
    } catch (...) {
        return Result<DownloadResult>::fail({ErrorCode::DownloadFailed, "Failed to copy local source"});
    }
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

std::string queryHeader(CFHTTPMessageRef response, CFStringRef name) {
    auto value = CfRef<CFStringRef>(CFHTTPMessageCopyHeaderFieldValue(response, name));
    return value ? cfStringToStd(value.get()) : std::string();
}

std::uint64_t contentLength(CFHTTPMessageRef response) {
    const auto value = queryHeader(response, CFSTR("Content-Length"));
    if (value.empty()) {
        return 0;
    }
    char* end = nullptr;
    const auto parsed = std::strtoull(value.c_str(), &end, 10);
    if (end == value.c_str()) {
        return 0;
    }
    return parsed;
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

std::vector<NetworkHeader> responseHeaders(CFHTTPMessageRef response) {
    std::vector<NetworkHeader> headers;
    auto serialized = CfRef<CFDataRef>(CFHTTPMessageCopySerializedMessage(response));
    if (!serialized) {
        return headers;
    }

    const auto length = CFDataGetLength(serialized.get());
    const auto* bytes = CFDataGetBytePtr(serialized.get());
    if (!bytes || length <= 0) {
        return headers;
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
    return headers;
}

std::string responseHeader(const NetworkResponseInfo& response, const std::string& name) {
    for (const auto& header : response.headers) {
        if (header.name == name) {
            return header.value;
        }
    }
    return {};
}

NetworkResponseInfo responseInfo(CFReadStreamRef stream, CFHTTPMessageRef headers, const std::string& requestedUrl) {
    NetworkResponseInfo response;
    response.statusCode = static_cast<int>(CFHTTPMessageGetResponseStatusCode(headers));
    response.headers = responseHeaders(headers);
    response.effectiveUrl = requestedUrl;

    auto finalUrl = CfRef<CFTypeRef>(CFReadStreamCopyProperty(stream, kCFStreamPropertyHTTPFinalURL));
    if (finalUrl && CFGetTypeID(finalUrl.get()) == CFURLGetTypeID()) {
        const auto url = reinterpret_cast<CFURLRef>(finalUrl.get());
        const auto serialized = cfStringToStd(CFURLGetString(url));
        if (!serialized.empty()) {
            response.effectiveUrl = serialized;
        }
    }
    return response;
}

struct ResponseStart {
    CfRef<CFHTTPMessageRef> headers;
    std::vector<char> bufferedBody;
};

Result<ResponseStart> waitForResponse(CFReadStreamRef stream, CancellationToken& cancel) {
    ResponseStart start;
    std::array<UInt8, 64 * 1024> buffer{};

    for (;;) {
        if (cancel.isCancelled()) {
            return Result<ResponseStart>::fail({ErrorCode::Cancelled, "Operation cancelled"});
        }

        if (auto headers = copyResponseHeaders(stream)) {
            start.headers = std::move(headers);
            return Result<ResponseStart>::ok(std::move(start));
        }

        const CFIndex count = CFReadStreamRead(stream, buffer.data(), static_cast<CFIndex>(buffer.size()));
        if (count < 0) {
            return Result<ResponseStart>::fail({ErrorCode::NetworkError, streamError(stream, "CFReadStreamRead")});
        }
        if (count == 0) {
            return Result<ResponseStart>::fail({ErrorCode::NetworkError, "No HTTP response headers received"});
        }

        const auto* begin = reinterpret_cast<const char*>(buffer.data());
        start.bufferedBody.insert(start.bufferedBody.end(), begin, begin + count);
    }
}

Result<std::vector<char>> readResponse(CFReadStreamRef stream, CancellationToken& cancel, ProgressCallback progress,
                                       const std::string& currentFile, std::uint64_t initialBytes,
                                       std::uint64_t expectedBytes, IRootedFile* output,
                                       const std::vector<char>& initialBody = {}) {
    std::vector<char> bytes;
    std::array<UInt8, 64 * 1024> buffer{};
    std::uint64_t downloaded = initialBytes;
    const std::uint64_t total = expectedBytes > 0 ? initialBytes + expectedBytes : 0;

    const auto consume = [&](const char* data, std::size_t count) -> Result<void> {
        if (output) {
            auto written = output->write(data, count);
            if (!written) {
                return written;
            }
        } else {
            bytes.insert(bytes.end(), data, data + count);
        }

        downloaded += static_cast<std::uint64_t>(count);
        if (progress) {
            progress({downloaded, total, currentFile});
        }
        return Result<void>::ok();
    };

    if (!initialBody.empty()) {
        auto consumed = consume(initialBody.data(), initialBody.size());
        if (!consumed) {
            return Result<std::vector<char>>::fail(consumed.error());
        }
    }

    for (;;) {
        if (cancel.isCancelled()) {
            return Result<std::vector<char>>::fail({ErrorCode::Cancelled, "Operation cancelled"});
        }

        const CFIndex count = CFReadStreamRead(stream, buffer.data(), static_cast<CFIndex>(buffer.size()));
        if (count < 0) {
            return Result<std::vector<char>>::fail({ErrorCode::NetworkError, streamError(stream, "CFReadStreamRead")});
        }
        if (count == 0) {
            break;
        }

        auto consumed = consume(reinterpret_cast<const char*>(buffer.data()), static_cast<std::size_t>(count));
        if (!consumed) {
            return Result<std::vector<char>>::fail(consumed.error());
        }
    }

    return Result<std::vector<char>>::ok(std::move(bytes));
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
    Result<TextResponse> getText(const std::string& url, const NetworkOptions& options,
                                 CancellationToken& cancel) noexcept override {
        if (!isHttpUrl(url)) {
            return readLocalText(url, cancel);
        }

        try {
            auto request = makeRequest(url);
            if (!request) {
                return Result<TextResponse>::fail(request.error());
            }
            auto stream = openStream(request.value(), options);
            if (!stream) {
                return Result<TextResponse>::fail(stream.error());
            }
            auto response = waitForResponse(stream.value().get(), cancel);
            if (!response) {
                return Result<TextResponse>::fail(response.error());
            }
            auto info = responseInfo(stream.value().get(), response.value().headers.get(), url);

            auto bytes =
                readResponse(stream.value().get(), cancel, {}, {}, 0, contentLength(response.value().headers.get()),
                             nullptr, response.value().bufferedBody);
            if (!bytes) {
                return Result<TextResponse>::fail(bytes.error());
            }
            TextResponse result;
            result.response = std::move(info);
            result.body.assign(bytes.value().begin(), bytes.value().end());
            return Result<TextResponse>::ok(std::move(result));
        } catch (...) {
            return Result<TextResponse>::fail({ErrorCode::NetworkError, "Unexpected CFNetwork request failure"});
        }
    }

    Result<DownloadResult> downloadToFile(const std::string& url, IRootedFile& target, const NetworkOptions& options,
                                          const std::optional<DownloadResumeInfo>& resume, ProgressCallback progress,
                                          CancellationToken& cancel) noexcept override {
        if (!isHttpUrl(url)) {
            const auto effectiveResume = options.enableResume ? resume : std::nullopt;
            return copyLocalToFile(url, target, effectiveResume, std::move(progress), cancel);
        }

        try {
            auto request = makeRequest(url);
            if (!request) {
                return Result<DownloadResult>::fail(request.error());
            }
            setResumeHeaders(request.value().message.get(), options, resume);

            auto stream = openStream(request.value(), options);
            if (!stream) {
                return Result<DownloadResult>::fail(stream.error());
            }
            auto response = waitForResponse(stream.value().get(), cancel);
            if (!response) {
                return Result<DownloadResult>::fail(response.error());
            }

            auto info = responseInfo(stream.value().get(), response.value().headers.get(), url);

            const bool appending = options.enableResume && resume && resume->offset > 0;
            const auto initialBytes = appending ? resume->offset : 0;
            const int writableStatus = appending ? 206 : 200;
            if (info.statusCode == writableStatus) {
                auto bytes =
                    readResponse(stream.value().get(), cancel, std::move(progress), {}, initialBytes,
                                 contentLength(response.value().headers.get()), &target, response.value().bufferedBody);
                if (!bytes) {
                    const auto code =
                        bytes.error().code == ErrorCode::NetworkError ? ErrorCode::DownloadFailed : bytes.error().code;
                    return Result<DownloadResult>::fail({code, bytes.error().message});
                }
            } else if (cancel.isCancelled()) {
                return Result<DownloadResult>::fail({ErrorCode::Cancelled, "Operation cancelled"});
            }

            DownloadResult result;
            result.response = std::move(info);
            result.bytesWritten = initialBytes;
            if (result.response.statusCode == writableStatus) {
                auto metadata = target.metadata();
                if (metadata) {
                    result.bytesWritten = metadata.value().size;
                }
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
