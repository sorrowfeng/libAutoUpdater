#include "libAutoUpdater/interfaces/INetworkClient.h"

#ifdef LIBAUTOUPDATER_HAS_CURL

#include "NetworkLimits.h"
#include "default/LocalNetworkFile.h"

#include <curl/curl.h>

#include <algorithm>
#include <cctype>
#include <limits>
#include <mutex>
#include <utility>

namespace autoupdater {

namespace {

std::once_flag curlInitFlag;

void ensureCurlGlobalInit() {
    std::call_once(curlInitFlag, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

class CurlEasyHandle {
  public:
    CurlEasyHandle() : handle_(curl_easy_init()) {}
    ~CurlEasyHandle() {
        if (handle_) {
            curl_easy_cleanup(handle_);
        }
    }

    CurlEasyHandle(const CurlEasyHandle&) = delete;
    CurlEasyHandle& operator=(const CurlEasyHandle&) = delete;

    CURL* get() const noexcept {
        return handle_;
    }

    explicit operator bool() const noexcept {
        return handle_ != nullptr;
    }

  private:
    CURL* handle_ = nullptr;
};

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

struct TextContext {
    std::string data;
    NetworkResponseInfo response;
    Error callbackError;
    std::uint64_t downloaded = 0;
    std::uint64_t maxResponseBytes = 0;
    bool discardedResponseBody = false;
    CancellationToken* cancel = nullptr;
};

struct FileContext {
    IRootedFile* output = nullptr;
    ProgressCallback progress;
    std::string currentFile;
    std::uint64_t downloaded = 0;
    std::uint64_t total = 0;
    std::uint64_t maxTotalBytes = 0;
    int writableStatus = 200;
    NetworkResponseInfo response;
    Error callbackError;
    Error writeError;
    bool discardedResponseBody = false;
    CancellationToken* cancel = nullptr;
};

struct HeaderContext {
    NetworkResponseInfo* response = nullptr;
    Error* callbackError = nullptr;
    std::uint64_t maxBodyBytes = 0;
    int writableStatus = 200;
    ErrorCode invalidHeaderCode = ErrorCode::NetworkError;
    std::uint64_t headerBytes = 0;
};

bool byteCount(std::size_t size, std::size_t nmemb, std::size_t& bytes) noexcept {
    if (size != 0 && nmemb > std::numeric_limits<std::size_t>::max() / size) {
        return false;
    }
    bytes = size * nmemb;
    return true;
}

static_assert(std::numeric_limits<std::size_t>::digits <= std::numeric_limits<std::uint64_t>::digits,
              "curl callback byte counts must fit in uint64_t");

bool parseStatusLine(const std::string& line, int& status) {
    if (line.rfind("HTTP/", 0) != 0) {
        return false;
    }
    const auto separator = line.find(' ');
    if (separator == std::string::npos) {
        return false;
    }
    auto begin = line.find_first_not_of(' ', separator);
    if (begin == std::string::npos || begin + 3 > line.size()) {
        return false;
    }
    if (!std::isdigit(static_cast<unsigned char>(line[begin])) ||
        !std::isdigit(static_cast<unsigned char>(line[begin + 1])) ||
        !std::isdigit(static_cast<unsigned char>(line[begin + 2]))) {
        return false;
    }
    status = (line[begin] - '0') * 100 + (line[begin + 1] - '0') * 10 + (line[begin + 2] - '0');
    return true;
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

std::string responseHeader(const NetworkResponseInfo& response, const std::string& name) {
    for (const auto& header : response.headers) {
        if (header.name == name) {
            return header.value;
        }
    }
    return {};
}

bool equalsAsciiCaseInsensitive(const std::string& value, const char* expected) {
    const std::string expectedValue(expected);
    if (value.size() != expectedValue.size()) {
        return false;
    }
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (std::tolower(static_cast<unsigned char>(value[index])) !=
            std::tolower(static_cast<unsigned char>(expectedValue[index]))) {
            return false;
        }
    }
    return true;
}

Result<void> validateIdentityContentEncoding(const NetworkResponseInfo& response, ErrorCode invalidHeaderCode) {
    for (const auto& header : response.headers) {
        if (header.name == "content-encoding" && !equalsAsciiCaseInsensitive(header.value, "identity")) {
            return Result<void>::fail(
                {invalidHeaderCode, "Encoded HTTP response bodies are not accepted for byte-exact transfers"});
        }
    }
    return Result<void>::ok();
}

Result<void> validateCompletedBody(const NetworkResponseInfo& response, std::uint64_t actualBytes,
                                   std::uint64_t maxBytes, ErrorCode invalidHeaderCode) {
    auto identityEncoding = validateIdentityContentEncoding(response, invalidHeaderCode);
    if (!identityEncoding) {
        return identityEncoding;
    }
    auto budget = detail::validateResponseBodyBudget(response, actualBytes, maxBytes, invalidHeaderCode);
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

std::size_t writeText(char* ptr, std::size_t size, std::size_t nmemb, void* userdata) {
    auto* context = static_cast<TextContext*>(userdata);
    if (context->cancel && context->cancel->isCancelled()) {
        return 0;
    }
    std::size_t bytes = 0;
    if (!byteCount(size, nmemb, bytes)) {
        context->callbackError = {ErrorCode::ResourceLimitExceeded, "HTTP response chunk is too large"};
        return 0;
    }
    if (context->response.statusCode != 200) {
        context->discardedResponseBody = true;
        return 0;
    }

    const auto chunkBytes = static_cast<std::uint64_t>(bytes);
    if (context->downloaded > context->maxResponseBytes ||
        chunkBytes > context->maxResponseBytes - context->downloaded) {
        context->callbackError = {ErrorCode::ResourceLimitExceeded, "HTTP response body exceeds its byte limit"};
        return 0;
    }
    std::uint64_t nextDownloaded = 0;
    if (!detail::checkedAdd(context->downloaded, chunkBytes, nextDownloaded)) {
        context->callbackError = {ErrorCode::ResourceLimitExceeded, "HTTP response byte counter overflow"};
        return 0;
    }
    try {
        if (bytes > context->data.max_size() - context->data.size()) {
            context->callbackError = {ErrorCode::ResourceLimitExceeded,
                                      "HTTP response body exceeds the supported string size"};
            return 0;
        }
        context->data.append(ptr, bytes);
        context->downloaded = nextDownloaded;
    } catch (...) {
        context->callbackError = {ErrorCode::NetworkError, "Failed to buffer HTTP response body"};
        return 0;
    }
    return bytes;
}

std::size_t writeFile(char* ptr, std::size_t size, std::size_t nmemb, void* userdata) {
    auto* context = static_cast<FileContext*>(userdata);
    if (context->cancel && context->cancel->isCancelled()) {
        return 0;
    }
    std::size_t bytes = 0;
    if (!byteCount(size, nmemb, bytes)) {
        context->callbackError = {ErrorCode::ResourceLimitExceeded, "HTTP response chunk is too large"};
        return 0;
    }

    if (context->response.statusCode != context->writableStatus) {
        context->discardedResponseBody = true;
        return 0;
    }

    const auto chunkBytes = static_cast<std::uint64_t>(bytes);
    if (context->downloaded > context->maxTotalBytes || chunkBytes > context->maxTotalBytes - context->downloaded) {
        context->callbackError = {ErrorCode::ResourceLimitExceeded, "Artifact response exceeds its signed byte limit"};
        return 0;
    }
    std::uint64_t nextDownloaded = 0;
    if (!detail::checkedAdd(context->downloaded, chunkBytes, nextDownloaded)) {
        context->callbackError = {ErrorCode::ResourceLimitExceeded, "Artifact byte counter overflow"};
        return 0;
    }

    try {
        auto written = context->output->write(ptr, bytes);
        if (!written) {
            context->writeError = written.error();
            return 0;
        }
        context->downloaded = nextDownloaded;
        if (context->progress) {
            context->progress({context->downloaded, context->total, context->currentFile});
        }
    } catch (...) {
        context->callbackError = {ErrorCode::DownloadFailed, "Download callback failed"};
        return 0;
    }
    return bytes;
}

std::size_t writeHeader(char* ptr, std::size_t size, std::size_t nmemb, void* userdata) {
    auto* context = static_cast<HeaderContext*>(userdata);
    std::size_t bytes = 0;
    if (!byteCount(size, nmemb, bytes)) {
        *context->callbackError = {ErrorCode::ResourceLimitExceeded, "HTTP response header is too large"};
        return 0;
    }
    const auto lineBytes = static_cast<std::uint64_t>(bytes);
    if (context->headerBytes > detail::kMaxNetworkResponseHeaderBytes ||
        lineBytes > detail::kMaxNetworkResponseHeaderBytes - context->headerBytes) {
        *context->callbackError = {ErrorCode::ResourceLimitExceeded, "HTTP response headers exceed their byte limit"};
        return 0;
    }
    context->headerBytes += lineBytes;
    try {
        std::string line(ptr, bytes);
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
            line.pop_back();
        }

        int status = 0;
        if (parseStatusLine(line, status)) {
            context->response->statusCode = status;
            context->response->headers.clear();
            return bytes;
        }

        if (line.empty()) {
            if (context->response->statusCode == context->writableStatus) {
                auto identityEncoding = validateIdentityContentEncoding(*context->response, context->invalidHeaderCode);
                if (!identityEncoding) {
                    *context->callbackError = identityEncoding.error();
                    return 0;
                }
                auto valid = detail::validateResponseBodyBudget(*context->response, 0, context->maxBodyBytes,
                                                                context->invalidHeaderCode);
                if (!valid) {
                    *context->callbackError = valid.error();
                    return 0;
                }
            }
            return bytes;
        }

        const auto colon = line.find(':');
        if (colon != std::string::npos) {
            auto key = line.substr(0, colon);
            auto value = line.substr(colon + 1);
            std::transform(key.begin(), key.end(), key.begin(),
                           [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
            trimOptionalWhitespace(value);
            context->response->headers.push_back({std::move(key), std::move(value)});
        }
    } catch (...) {
        *context->callbackError = {context->invalidHeaderCode, "Failed to process HTTP response headers"};
        return 0;
    }
    return bytes;
}

void applyCommonOptions(CURL* curl, const NetworkOptions& options) {
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
#if LIBCURL_VERSION_NUM >= 0x075500
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
#else
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS, static_cast<long>(CURLPROTO_HTTP | CURLPROTO_HTTPS));
#endif
#if LIBCURL_VERSION_NUM >= 0x071506
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "identity");
#else
    curl_easy_setopt(curl, CURLOPT_ENCODING, "identity");
#endif
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(options.connectTimeout.count()));
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(options.transferTimeout.count()));
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, options.verifyTls ? 1L : 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, options.verifyTls ? 2L : 0L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
}

void populateResponseInfo(CURL* curl, NetworkResponseInfo& response) {
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    response.statusCode = static_cast<int>(status);

    char* effectiveUrl = nullptr;
    if (curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &effectiveUrl) == CURLE_OK && effectiveUrl) {
        response.effectiveUrl = effectiveUrl;
    }
}

class CurlNetworkClient final : public INetworkClient {
  public:
    Result<TextResponse> getText(const std::string& url, const NetworkOptions& options, std::uint64_t maxResponseBytes,
                                 CancellationToken& cancel) noexcept override {
        if (!isHttpUrl(url)) {
            return detail::readLocalText(url, maxResponseBytes, cancel);
        }
        try {
            ensureCurlGlobalInit();
            TextContext context;
            context.maxResponseBytes = maxResponseBytes;
            CurlEasyHandle curl;
            if (!curl) {
                return Result<TextResponse>::fail({ErrorCode::NetworkError, "curl_easy_init failed"});
            }
            context.cancel = &cancel;
            HeaderContext headerContext{&context.response, &context.callbackError, maxResponseBytes, 200,
                                        ErrorCode::NetworkError};
            curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
            curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, writeText);
            curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &context);
            curl_easy_setopt(curl.get(), CURLOPT_HEADERFUNCTION, writeHeader);
            curl_easy_setopt(curl.get(), CURLOPT_HEADERDATA, &headerContext);
            applyCommonOptions(curl.get(), options);

            const CURLcode code = curl_easy_perform(curl.get());
            populateResponseInfo(curl.get(), context.response);

            if (cancel.isCancelled()) {
                return Result<TextResponse>::fail({ErrorCode::Cancelled, "Operation cancelled"});
            }
            if (!context.callbackError.ok()) {
                return Result<TextResponse>::fail(context.callbackError);
            }
            const bool stoppedAfterHeaders =
                context.discardedResponseBody && code == CURLE_WRITE_ERROR && context.response.statusCode != 0;
            if (context.response.statusCode == 200) {
                auto valid = validateCompletedBody(context.response, context.downloaded, maxResponseBytes,
                                                   ErrorCode::NetworkError);
                if (!valid) {
                    return Result<TextResponse>::fail(valid.error());
                }
            }
            if (code != CURLE_OK && !stoppedAfterHeaders) {
                return Result<TextResponse>::fail({ErrorCode::NetworkError, curl_easy_strerror(code)});
            }
            TextResponse response;
            response.response = std::move(context.response);
            response.body = std::move(context.data);
            return Result<TextResponse>::ok(std::move(response));
        } catch (...) {
            return Result<TextResponse>::fail({ErrorCode::NetworkError, "Unexpected curl request failure"});
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
            ensureCurlGlobalInit();
            FileContext context;
            CurlEasyHandle curl;
            if (!curl) {
                return Result<DownloadResult>::fail({ErrorCode::NetworkError, "curl_easy_init failed"});
            }

            context.output = &target;
            context.progress = std::move(progress);
            const bool appending = options.enableResume && resume && resume->offset > 0;
            const auto initialBytes = appending ? resume->offset : 0;
            auto remaining = detail::remainingTransferBudget(initialBytes, maxTotalBytes);
            if (!remaining) {
                return Result<DownloadResult>::fail(remaining.error());
            }
            context.downloaded = initialBytes;
            context.total = maxTotalBytes;
            context.maxTotalBytes = maxTotalBytes;
            context.writableStatus = appending ? 206 : 200;
            context.cancel = &cancel;
            HeaderContext headerContext{&context.response, &context.callbackError, remaining.value(),
                                        context.writableStatus, ErrorCode::DownloadFailed};

            curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
            curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, writeFile);
            curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &context);
            curl_easy_setopt(curl.get(), CURLOPT_HEADERFUNCTION, writeHeader);
            curl_easy_setopt(curl.get(), CURLOPT_HEADERDATA, &headerContext);
            applyCommonOptions(curl.get(), options);

            struct curl_slist* headers = nullptr;
            if (appending) {
                const auto range = std::to_string(resume->offset) + "-";
                curl_easy_setopt(curl.get(), CURLOPT_RANGE, range.c_str());
                if (!resume->etag.empty()) {
                    const auto ifRange = "If-Range: " + resume->etag;
                    headers = curl_slist_append(headers, ifRange.c_str());
                } else if (!resume->lastModified.empty()) {
                    const auto ifRange = "If-Range: " + resume->lastModified;
                    headers = curl_slist_append(headers, ifRange.c_str());
                }
                if (headers) {
                    curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, headers);
                }
            }

            const CURLcode code = curl_easy_perform(curl.get());
            if (headers) {
                curl_slist_free_all(headers);
            }
            populateResponseInfo(curl.get(), context.response);

            if (cancel.isCancelled()) {
                return Result<DownloadResult>::fail({ErrorCode::Cancelled, "Operation cancelled"});
            }
            if (!context.callbackError.ok()) {
                return Result<DownloadResult>::fail(context.callbackError);
            }
            if (!context.writeError.ok()) {
                return Result<DownloadResult>::fail(context.writeError);
            }
            const bool stoppedAfterHeaders =
                context.discardedResponseBody && code == CURLE_WRITE_ERROR && context.response.statusCode != 0;
            if (context.response.statusCode == context.writableStatus) {
                const auto actualBytes = context.downloaded - initialBytes;
                auto valid =
                    validateCompletedBody(context.response, actualBytes, remaining.value(), ErrorCode::DownloadFailed);
                if (!valid) {
                    return Result<DownloadResult>::fail(valid.error());
                }
            }
            if (code != CURLE_OK && !stoppedAfterHeaders) {
                return Result<DownloadResult>::fail({ErrorCode::DownloadFailed, curl_easy_strerror(code)});
            }

            DownloadResult result;
            result.response = std::move(context.response);
            result.bytesWritten = context.downloaded;
            result.etag = responseHeader(result.response, "etag");
            result.lastModified = responseHeader(result.response, "last-modified");
            return Result<DownloadResult>::ok(result);
        } catch (...) {
            return Result<DownloadResult>::fail({ErrorCode::DownloadFailed, "Unexpected curl download failure"});
        }
    }
};

} // namespace

std::shared_ptr<INetworkClient> createDefaultNetworkClient() {
    return std::make_shared<CurlNetworkClient>();
}

} // namespace autoupdater

#endif
