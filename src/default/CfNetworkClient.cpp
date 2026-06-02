#include "libAutoUpdater/interfaces/INetworkClient.h"

#ifdef LIBAUTOUPDATER_HAS_CFNETWORK

#include "util/UrlUtil.h"

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

template <class T>
class CfRef {
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

    T get() const noexcept { return value_; }
    explicit operator bool() const noexcept { return value_ != nullptr; }

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
    return std::string(action) + " failed (domain " + std::to_string(error.domain) +
        ", error " + std::to_string(error.error) + ")";
}

bool isHttpUrl(const std::string& url) {
    const auto separator = url.find("://");
    if (separator == std::string::npos) {
        return false;
    }
    auto scheme = url.substr(0, separator);
    std::transform(scheme.begin(), scheme.end(), scheme.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return scheme == "http" || scheme == "https";
}

Result<std::filesystem::path> localPathFromUrl(const std::string& url) {
    if (util::isFileUrl(url)) {
        return Result<std::filesystem::path>::ok(util::fileUrlToPath(url));
    }
    if (url.find("://") == std::string::npos) {
        return Result<std::filesystem::path>::ok(url);
    }
    return Result<std::filesystem::path>::fail({ErrorCode::NetworkError, "CFNetwork supports only HTTP and HTTPS URLs"});
}

Result<std::string> readLocalText(const std::string& url, CancellationToken& cancel) {
    if (cancel.isCancelled()) {
        return Result<std::string>::fail({ErrorCode::Cancelled, "Operation cancelled"});
    }

    auto path = localPathFromUrl(url);
    if (!path) {
        return Result<std::string>::fail(path.error());
    }

    try {
        std::ifstream input(path.value(), std::ios::binary);
        if (!input) {
            return Result<std::string>::fail({ErrorCode::ManifestDownloadFailed, "Failed to open local source"});
        }
        std::ostringstream stream;
        stream << input.rdbuf();
        return Result<std::string>::ok(stream.str());
    } catch (...) {
        return Result<std::string>::fail({ErrorCode::NetworkError, "Failed to read local source"});
    }
}

Result<DownloadResult> copyLocalToFile(const std::string& url,
                                       const std::filesystem::path& target,
                                       const std::optional<DownloadResumeInfo>& resume,
                                       ProgressCallback progress,
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
        if (!target.parent_path().empty()) {
            std::filesystem::create_directories(target.parent_path(), ec);
            if (ec) {
                return Result<DownloadResult>::fail({ErrorCode::FileSystemError, ec.message()});
            }
        }

        std::ifstream input(source.value(), std::ios::binary);
        if (resume && resume->offset > 0) {
            input.seekg(static_cast<std::streamoff>(resume->offset), std::ios::beg);
        }
        const auto outputMode = (resume && resume->offset > 0)
            ? (std::ios::binary | std::ios::app)
            : (std::ios::binary | std::ios::trunc);
        std::ofstream output(target, outputMode);
        if (!input || !output) {
            return Result<DownloadResult>::fail({ErrorCode::DownloadFailed, "Failed to open local download streams"});
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
                output.write(buffer.data(), count);
                written += static_cast<std::uint64_t>(count);
                if (progress) {
                    progress({written, static_cast<std::uint64_t>(total), target.generic_string()});
                }
            }
        }

        DownloadResult result;
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
    auto cfUrl = CfRef<CFURLRef>(CFURLCreateWithBytes(
        kCFAllocatorDefault,
        reinterpret_cast<const UInt8*>(url.data()),
        static_cast<CFIndex>(url.size()),
        kCFStringEncodingUTF8,
        nullptr));
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
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
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
    auto stream = CfRef<CFReadStreamRef>(
        CFReadStreamCreateForHTTPRequest(kCFAllocatorDefault, request.message.get()));
    if (!stream) {
        return Result<CfRef<CFReadStreamRef>>::fail({ErrorCode::NetworkError, "Failed to create CFNetwork stream"});
    }

    CFReadStreamSetProperty(stream.get(), kCFStreamPropertyHTTPShouldAutoredirect, kCFBooleanTrue);

    if (request.secure && !options.verifyTls) {
        const void* keys[] = {kCFStreamSSLValidatesCertificateChain};
        const void* values[] = {kCFBooleanFalse};
        auto sslSettings = CfRef<CFDictionaryRef>(CFDictionaryCreate(
            kCFAllocatorDefault,
            keys,
            values,
            1,
            &kCFTypeDictionaryKeyCallBacks,
            &kCFTypeDictionaryValueCallBacks));
        if (sslSettings) {
            CFReadStreamSetProperty(stream.get(), kCFStreamPropertySSLSettings, sslSettings.get());
        }
    }

    if (!CFReadStreamOpen(stream.get())) {
        return Result<CfRef<CFReadStreamRef>>::fail({ErrorCode::NetworkError, streamError(stream.get(), "CFReadStreamOpen")});
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

Result<std::vector<char>> readResponse(CFReadStreamRef stream,
                                       CancellationToken& cancel,
                                       ProgressCallback progress,
                                       const std::string& currentFile,
                                       std::uint64_t initialBytes,
                                       std::uint64_t expectedBytes,
                                       std::ofstream* output,
                                       const std::vector<char>& initialBody = {}) {
    std::vector<char> bytes;
    std::array<UInt8, 64 * 1024> buffer{};
    std::uint64_t downloaded = initialBytes;
    const std::uint64_t total = expectedBytes > 0 ? initialBytes + expectedBytes : 0;

    const auto consume = [&](const char* data, std::size_t count) -> Result<void> {
        if (output) {
            output->write(data, static_cast<std::streamsize>(count));
            if (!*output) {
                return Result<void>::fail({ErrorCode::DownloadFailed, "Failed to write target file"});
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

void setResumeHeaders(CFHTTPMessageRef request,
                      const NetworkOptions& options,
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
    Result<std::string> getText(const std::string& url,
                                const NetworkOptions& options,
                                CancellationToken& cancel) noexcept override {
        if (!isHttpUrl(url)) {
            return readLocalText(url, cancel);
        }

        try {
            auto request = makeRequest(url);
            if (!request) {
                return Result<std::string>::fail(request.error());
            }
            auto stream = openStream(request.value(), options);
            if (!stream) {
                return Result<std::string>::fail(stream.error());
            }
            auto response = waitForResponse(stream.value().get(), cancel);
            if (!response) {
                return Result<std::string>::fail(response.error());
            }
            const auto status = CFHTTPMessageGetResponseStatusCode(response.value().headers.get());
            if (status >= 400) {
                return Result<std::string>::fail({ErrorCode::NetworkError, "HTTP error " + std::to_string(status)});
            }

            auto bytes = readResponse(
                stream.value().get(),
                cancel,
                {},
                {},
                0,
                contentLength(response.value().headers.get()),
                nullptr,
                response.value().bufferedBody);
            if (!bytes) {
                return Result<std::string>::fail(bytes.error());
            }
            return Result<std::string>::ok(std::string(bytes.value().begin(), bytes.value().end()));
        } catch (...) {
            return Result<std::string>::fail({ErrorCode::NetworkError, "Unexpected CFNetwork request failure"});
        }
    }

    Result<DownloadResult> downloadToFile(const std::string& url,
                                          const std::filesystem::path& target,
                                          const NetworkOptions& options,
                                          const std::optional<DownloadResumeInfo>& resume,
                                          ProgressCallback progress,
                                          CancellationToken& cancel) noexcept override {
        if (!isHttpUrl(url)) {
            return copyLocalToFile(url, target, resume, std::move(progress), cancel);
        }

        try {
            std::error_code ec;
            if (!target.parent_path().empty()) {
                std::filesystem::create_directories(target.parent_path(), ec);
                if (ec) {
                    return Result<DownloadResult>::fail({ErrorCode::FileSystemError, ec.message()});
                }
            }

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

            const auto status = CFHTTPMessageGetResponseStatusCode(response.value().headers.get());
            if (options.enableResume && resume && resume->offset > 0 && status == 200) {
                std::filesystem::remove(target, ec);
                return Result<DownloadResult>::fail({ErrorCode::DownloadFailed, "Server ignored Range request"});
            }
            if (status >= 400) {
                return Result<DownloadResult>::fail({ErrorCode::DownloadFailed, "HTTP error " + std::to_string(status)});
            }

            const auto outputMode = (options.enableResume && resume && resume->offset > 0)
                ? (std::ios::binary | std::ios::app)
                : (std::ios::binary | std::ios::trunc);
            std::ofstream output(target, outputMode);
            if (!output) {
                return Result<DownloadResult>::fail({ErrorCode::DownloadFailed, "Failed to open target file"});
            }

            const auto initialBytes = (options.enableResume && resume) ? resume->offset : 0;
            auto bytes = readResponse(
                stream.value().get(),
                cancel,
                std::move(progress),
                target.generic_string(),
                initialBytes,
                contentLength(response.value().headers.get()),
                &output,
                response.value().bufferedBody);
            if (!bytes) {
                const auto code = bytes.error().code == ErrorCode::NetworkError
                    ? ErrorCode::DownloadFailed
                    : bytes.error().code;
                return Result<DownloadResult>::fail({code, bytes.error().message});
            }

            DownloadResult result;
            result.bytesWritten = initialBytes;
            std::error_code sizeError;
            const auto finalSize = std::filesystem::file_size(target, sizeError);
            if (!sizeError) {
                result.bytesWritten = finalSize;
            }
            result.etag = queryHeader(response.value().headers.get(), CFSTR("ETag"));
            result.lastModified = queryHeader(response.value().headers.get(), CFSTR("Last-Modified"));
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
