#include "libAutoUpdater/interfaces/INetworkClient.h"

#ifdef LIBAUTOUPDATER_HAS_CURL

#include "util/PathUtil.h"
#include "util/UrlUtil.h"

#include <curl/curl.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <sstream>
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

Result<std::filesystem::path> localPathFromUrl(const std::string& url) {
    if (util::isFileUrl(url)) {
        return Result<std::filesystem::path>::ok(util::fileUrlToPath(url));
    }
    return Result<std::filesystem::path>::fail(
        {ErrorCode::NetworkError, "curl accepts only HTTP, HTTPS, and explicit file: URLs"});
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
        return Result<DownloadResult>::ok(std::move(result));
    } catch (...) {
        return Result<DownloadResult>::fail({ErrorCode::DownloadFailed, "Failed to copy local source"});
    }
}

struct TextContext {
    std::string data;
    NetworkResponseInfo response;
    Error callbackError;
    CancellationToken* cancel = nullptr;
};

struct FileContext {
    IRootedFile* output = nullptr;
    ProgressCallback progress;
    std::string currentFile;
    std::uint64_t downloaded = 0;
    std::uint64_t total = 0;
    int writableStatus = 200;
    NetworkResponseInfo response;
    Error callbackError;
    Error writeError;
    bool discardedResponseBody = false;
    CancellationToken* cancel = nullptr;
};

bool byteCount(std::size_t size, std::size_t nmemb, std::size_t& bytes) noexcept {
    if (size != 0 && nmemb > std::numeric_limits<std::size_t>::max() / size) {
        return false;
    }
    bytes = size * nmemb;
    return true;
}

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

std::size_t writeText(char* ptr, std::size_t size, std::size_t nmemb, void* userdata) {
    auto* context = static_cast<TextContext*>(userdata);
    if (context->cancel && context->cancel->isCancelled()) {
        return 0;
    }
    std::size_t bytes = 0;
    if (!byteCount(size, nmemb, bytes)) {
        context->callbackError = {ErrorCode::NetworkError, "HTTP response chunk is too large"};
        return 0;
    }
    try {
        context->data.append(ptr, bytes);
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
        context->callbackError = {ErrorCode::DownloadFailed, "HTTP response chunk is too large"};
        return 0;
    }

    if (context->response.statusCode != context->writableStatus) {
        context->discardedResponseBody = true;
        return 0;
    }

    try {
        auto written = context->output->write(ptr, bytes);
        if (!written) {
            context->writeError = written.error();
            return 0;
        }
        context->downloaded += static_cast<std::uint64_t>(bytes);
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
    auto* response = static_cast<NetworkResponseInfo*>(userdata);
    std::size_t bytes = 0;
    if (!byteCount(size, nmemb, bytes)) {
        return 0;
    }
    try {
        std::string line(ptr, bytes);
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
            line.pop_back();
        }

        int status = 0;
        if (parseStatusLine(line, status)) {
            response->statusCode = status;
            response->headers.clear();
            return bytes;
        }

        const auto colon = line.find(':');
        if (colon != std::string::npos) {
            auto key = line.substr(0, colon);
            auto value = line.substr(colon + 1);
            std::transform(key.begin(), key.end(), key.begin(),
                           [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
            trimOptionalWhitespace(value);
            response->headers.push_back({std::move(key), std::move(value)});
        }
    } catch (...) {
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
    Result<TextResponse> getText(const std::string& url, const NetworkOptions& options,
                                 CancellationToken& cancel) noexcept override {
        if (!isHttpUrl(url)) {
            return readLocalText(url, cancel);
        }
        try {
            ensureCurlGlobalInit();
            TextContext context;
            CurlEasyHandle curl;
            if (!curl) {
                return Result<TextResponse>::fail({ErrorCode::NetworkError, "curl_easy_init failed"});
            }
            context.cancel = &cancel;
            curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
            curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, writeText);
            curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &context);
            curl_easy_setopt(curl.get(), CURLOPT_HEADERFUNCTION, writeHeader);
            curl_easy_setopt(curl.get(), CURLOPT_HEADERDATA, &context.response);
            applyCommonOptions(curl.get(), options);

            const CURLcode code = curl_easy_perform(curl.get());
            populateResponseInfo(curl.get(), context.response);

            if (cancel.isCancelled()) {
                return Result<TextResponse>::fail({ErrorCode::Cancelled, "Operation cancelled"});
            }
            if (!context.callbackError.ok()) {
                return Result<TextResponse>::fail(context.callbackError);
            }
            if (code != CURLE_OK) {
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
                                          const std::optional<DownloadResumeInfo>& resume, ProgressCallback progress,
                                          CancellationToken& cancel) noexcept override {
        if (!isHttpUrl(url)) {
            const auto effectiveResume = options.enableResume ? resume : std::nullopt;
            return copyLocalToFile(url, target, effectiveResume, std::move(progress), cancel);
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
            context.downloaded = appending ? resume->offset : 0;
            context.writableStatus = appending ? 206 : 200;
            context.cancel = &cancel;

            curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
            curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, writeFile);
            curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &context);
            curl_easy_setopt(curl.get(), CURLOPT_HEADERFUNCTION, writeHeader);
            curl_easy_setopt(curl.get(), CURLOPT_HEADERDATA, &context.response);
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
            const bool stoppedAfterHeaders =
                context.discardedResponseBody && code == CURLE_WRITE_ERROR && context.response.statusCode != 0;
            if (code != CURLE_OK && !stoppedAfterHeaders) {
                if (!context.writeError.ok()) {
                    return Result<DownloadResult>::fail(context.writeError);
                }
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
