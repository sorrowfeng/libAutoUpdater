#include "libAutoUpdater/interfaces/INetworkClient.h"

#ifdef LIBAUTOUPDATER_HAS_CURL

#include "util/PathUtil.h"

#include <curl/curl.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <mutex>

namespace autoupdater {

namespace {

std::once_flag curlInitFlag;

void ensureCurlGlobalInit() {
    std::call_once(curlInitFlag, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

struct TextContext {
    std::string data;
    CancellationToken* cancel = nullptr;
};

struct FileContext {
    std::ofstream* output = nullptr;
    ProgressCallback progress;
    std::string currentFile;
    std::uint64_t downloaded = 0;
    std::uint64_t total = 0;
    std::string etag;
    std::string lastModified;
    CancellationToken* cancel = nullptr;
};

std::size_t writeText(char* ptr, std::size_t size, std::size_t nmemb, void* userdata) {
    auto* context = static_cast<TextContext*>(userdata);
    if (context->cancel && context->cancel->isCancelled()) {
        return 0;
    }
    const auto bytes = size * nmemb;
    context->data.append(ptr, bytes);
    return bytes;
}

std::size_t writeFile(char* ptr, std::size_t size, std::size_t nmemb, void* userdata) {
    auto* context = static_cast<FileContext*>(userdata);
    if (context->cancel && context->cancel->isCancelled()) {
        return 0;
    }
    const auto bytes = size * nmemb;
    context->output->write(ptr, static_cast<std::streamsize>(bytes));
    context->downloaded += static_cast<std::uint64_t>(bytes);
    if (context->progress) {
        context->progress({context->downloaded, context->total, context->currentFile});
    }
    return bytes;
}

std::size_t writeHeader(char* ptr, std::size_t size, std::size_t nmemb, void* userdata) {
    auto* context = static_cast<FileContext*>(userdata);
    const auto bytes = size * nmemb;
    std::string line(ptr, bytes);
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
        line.pop_back();
    }
    const auto colon = line.find(':');
    if (colon != std::string::npos) {
        auto key = line.substr(0, colon);
        auto value = line.substr(colon + 1);
        while (!value.empty() && value.front() == ' ') {
            value.erase(value.begin());
        }
        std::transform(key.begin(), key.end(), key.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        if (key == "etag") {
            context->etag = value;
        } else if (key == "last-modified") {
            context->lastModified = value;
        }
    }
    return bytes;
}

void applyCommonOptions(CURL* curl, const NetworkOptions& options) {
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(options.connectTimeout.count()));
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(options.transferTimeout.count()));
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, options.verifyTls ? 1L : 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, options.verifyTls ? 2L : 0L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
}

class CurlNetworkClient final : public INetworkClient {
  public:
    Result<std::string> getText(const std::string& url, const NetworkOptions& options,
                                CancellationToken& cancel) noexcept override {
        ensureCurlGlobalInit();
        CURL* curl = curl_easy_init();
        if (!curl) {
            return Result<std::string>::fail({ErrorCode::NetworkError, "curl_easy_init failed"});
        }
        TextContext context;
        context.cancel = &cancel;
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeText);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &context);
        applyCommonOptions(curl, options);

        const CURLcode code = curl_easy_perform(curl);
        long response = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response);
        curl_easy_cleanup(curl);

        if (cancel.isCancelled()) {
            return Result<std::string>::fail({ErrorCode::Cancelled, "Operation cancelled"});
        }
        if (code != CURLE_OK) {
            return Result<std::string>::fail({ErrorCode::NetworkError, curl_easy_strerror(code)});
        }
        if (response >= 400) {
            return Result<std::string>::fail({ErrorCode::NetworkError, "HTTP error " + std::to_string(response)});
        }
        return Result<std::string>::ok(std::move(context.data));
    }

    Result<DownloadResult> downloadToFile(const std::string& url, const std::filesystem::path& target,
                                          const NetworkOptions& options,
                                          const std::optional<DownloadResumeInfo>& resume, ProgressCallback progress,
                                          CancellationToken& cancel) noexcept override {
        ensureCurlGlobalInit();
        try {
            std::error_code ec;
            if (!target.parent_path().empty()) {
                std::filesystem::create_directories(target.parent_path(), ec);
                if (ec) {
                    return Result<DownloadResult>::fail({ErrorCode::FileSystemError, ec.message()});
                }
            }

            const auto outputMode = (options.enableResume && resume && resume->offset > 0)
                                        ? (std::ios::binary | std::ios::app)
                                        : (std::ios::binary | std::ios::trunc);
            std::ofstream output(target, outputMode);
            if (!output) {
                return Result<DownloadResult>::fail({ErrorCode::DownloadFailed, "Failed to open target file"});
            }

            CURL* curl = curl_easy_init();
            if (!curl) {
                return Result<DownloadResult>::fail({ErrorCode::NetworkError, "curl_easy_init failed"});
            }

            FileContext context;
            context.output = &output;
            context.progress = std::move(progress);
            context.currentFile = util::pathToUtf8(target);
            context.downloaded = resume ? resume->offset : 0;
            context.cancel = &cancel;

            curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeFile);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, &context);
            curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, writeHeader);
            curl_easy_setopt(curl, CURLOPT_HEADERDATA, &context);
            applyCommonOptions(curl, options);

            struct curl_slist* headers = nullptr;
            if (options.enableResume && resume && resume->offset > 0) {
                const auto range = std::to_string(resume->offset) + "-";
                curl_easy_setopt(curl, CURLOPT_RANGE, range.c_str());
                if (!resume->etag.empty()) {
                    const auto ifRange = "If-Range: " + resume->etag;
                    headers = curl_slist_append(headers, ifRange.c_str());
                } else if (!resume->lastModified.empty()) {
                    const auto ifRange = "If-Range: " + resume->lastModified;
                    headers = curl_slist_append(headers, ifRange.c_str());
                }
                if (headers) {
                    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
                }
            }

            const CURLcode code = curl_easy_perform(curl);
            double contentLength = 0;
            curl_easy_getinfo(curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD, &contentLength);
            if (contentLength > 0) {
                context.total = static_cast<std::uint64_t>(contentLength);
            }
            long response = 0;
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response);
            if (headers) {
                curl_slist_free_all(headers);
            }
            curl_easy_cleanup(curl);

            if (cancel.isCancelled()) {
                return Result<DownloadResult>::fail({ErrorCode::Cancelled, "Operation cancelled"});
            }
            if (code != CURLE_OK) {
                return Result<DownloadResult>::fail({ErrorCode::DownloadFailed, curl_easy_strerror(code)});
            }
            if (resume && resume->offset > 0 && response == 200) {
                std::error_code ec;
                std::filesystem::remove(target, ec);
                return Result<DownloadResult>::fail({ErrorCode::DownloadFailed, "Server ignored Range request"});
            }
            if (response >= 400) {
                return Result<DownloadResult>::fail(
                    {ErrorCode::DownloadFailed, "HTTP error " + std::to_string(response)});
            }

            DownloadResult result;
            result.bytesWritten = context.downloaded;
            result.etag = context.etag;
            result.lastModified = context.lastModified;
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
