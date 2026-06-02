#include "libAutoUpdater/interfaces/INetworkClient.h"

#ifdef LIBAUTOUPDATER_HAS_WINHTTP

#include "util/UrlUtil.h"

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace autoupdater {

namespace {

class WinHttpHandle {
  public:
    WinHttpHandle() = default;
    explicit WinHttpHandle(HINTERNET handle) : handle_(handle) {}
    ~WinHttpHandle() {
        if (handle_) {
            WinHttpCloseHandle(handle_);
        }
    }

    WinHttpHandle(const WinHttpHandle&) = delete;
    WinHttpHandle& operator=(const WinHttpHandle&) = delete;

    WinHttpHandle(WinHttpHandle&& other) noexcept : handle_(std::exchange(other.handle_, nullptr)) {}
    WinHttpHandle& operator=(WinHttpHandle&& other) noexcept {
        if (this != &other) {
            if (handle_) {
                WinHttpCloseHandle(handle_);
            }
            handle_ = std::exchange(other.handle_, nullptr);
        }
        return *this;
    }

    HINTERNET get() const noexcept {
        return handle_;
    }
    explicit operator bool() const noexcept {
        return handle_ != nullptr;
    }

  private:
    HINTERNET handle_ = nullptr;
};

struct WinHttpRequest {
    WinHttpHandle session;
    WinHttpHandle connection;
    WinHttpHandle request;

    HINTERNET get() const noexcept {
        return request.get();
    }
};

std::wstring widen(const std::string& text) {
    if (text.empty()) {
        return {};
    }
    const int count = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (count <= 0) {
        return {};
    }
    std::wstring output(static_cast<std::size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), output.data(), count);
    return output;
}

std::string narrow(const std::wstring& text) {
    if (text.empty()) {
        return {};
    }
    const int count =
        WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (count <= 0) {
        return {};
    }
    std::string output(static_cast<std::size_t>(count), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), output.data(), count, nullptr, nullptr);
    return output;
}

std::string lastWindowsError(const char* action) {
    const DWORD code = GetLastError();
    wchar_t* buffer = nullptr;
    const DWORD size = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);

    std::wstring message;
    if (size > 0 && buffer) {
        message.assign(buffer, size);
        while (!message.empty() && (message.back() == L'\r' || message.back() == L'\n')) {
            message.pop_back();
        }
        LocalFree(buffer);
    }

    std::string output = action;
    output += " failed";
    if (!message.empty()) {
        output += ": ";
        output += narrow(message);
    } else {
        output += " with Windows error " + std::to_string(code);
    }
    return output;
}

template <class T> Result<T> failLast(ErrorCode code, const char* action) {
    return Result<T>::fail({code, lastWindowsError(action)});
}

int timeoutMillis(std::chrono::milliseconds value) {
    const auto count = value.count();
    if (count <= 0) {
        return 0;
    }
    const auto maxValue = static_cast<long long>(std::numeric_limits<int>::max());
    return static_cast<int>(std::min<long long>(count, maxValue));
}

struct ParsedUrl {
    std::wstring host;
    std::wstring path;
    INTERNET_PORT port = 0;
    bool secure = false;
};

Result<ParsedUrl> parseUrl(const std::string& url) {
    const auto wideUrl = widen(url);
    if (wideUrl.empty()) {
        return Result<ParsedUrl>::fail({ErrorCode::NetworkError, "Invalid URL"});
    }

    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwSchemeLength = static_cast<DWORD>(-1);
    parts.dwHostNameLength = static_cast<DWORD>(-1);
    parts.dwUrlPathLength = static_cast<DWORD>(-1);
    parts.dwExtraInfoLength = static_cast<DWORD>(-1);

    if (!WinHttpCrackUrl(wideUrl.c_str(), 0, 0, &parts)) {
        return failLast<ParsedUrl>(ErrorCode::NetworkError, "WinHttpCrackUrl");
    }

    std::wstring scheme(parts.lpszScheme, parts.dwSchemeLength);
    std::transform(scheme.begin(), scheme.end(), scheme.begin(),
                   [](wchar_t ch) { return static_cast<wchar_t>(towlower(ch)); });
    if (scheme != L"http" && scheme != L"https") {
        return Result<ParsedUrl>::fail({ErrorCode::NetworkError, "WinHTTP supports only HTTP and HTTPS URLs"});
    }

    ParsedUrl parsed;
    if (parts.lpszHostName && parts.dwHostNameLength > 0) {
        parsed.host.assign(parts.lpszHostName, parts.dwHostNameLength);
    }
    if (parts.lpszUrlPath && parts.dwUrlPathLength > 0) {
        parsed.path.assign(parts.lpszUrlPath, parts.dwUrlPathLength);
    }
    if (parts.lpszExtraInfo && parts.dwExtraInfoLength > 0) {
        parsed.path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    }
    if (parsed.path.empty()) {
        parsed.path = L"/";
    }
    parsed.port = parts.nPort;
    parsed.secure = scheme == L"https";
    return Result<ParsedUrl>::ok(std::move(parsed));
}

Result<WinHttpRequest> openRequest(const std::string& url, const NetworkOptions& options,
                                   const std::wstring& extraHeaders) {
    auto parsed = parseUrl(url);
    if (!parsed) {
        return Result<WinHttpRequest>::fail(parsed.error());
    }

    WinHttpHandle session(WinHttpOpen(L"libAutoUpdater/0.1", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                                      WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) {
        return failLast<WinHttpRequest>(ErrorCode::NetworkError, "WinHttpOpen");
    }

    WinHttpSetTimeouts(session.get(), timeoutMillis(options.connectTimeout), timeoutMillis(options.connectTimeout),
                       timeoutMillis(options.transferTimeout), timeoutMillis(options.transferTimeout));

    WinHttpHandle connection(WinHttpConnect(session.get(), parsed.value().host.c_str(), parsed.value().port, 0));
    if (!connection) {
        return failLast<WinHttpRequest>(ErrorCode::NetworkError, "WinHttpConnect");
    }

    const DWORD flags = parsed.value().secure ? WINHTTP_FLAG_SECURE : 0;
    WinHttpHandle requestHandle(WinHttpOpenRequest(connection.get(), L"GET", parsed.value().path.c_str(), nullptr,
                                                   WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags));
    if (!requestHandle) {
        return failLast<WinHttpRequest>(ErrorCode::NetworkError, "WinHttpOpenRequest");
    }

    DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
    WinHttpSetOption(requestHandle.get(), WINHTTP_OPTION_REDIRECT_POLICY, &redirectPolicy, sizeof(redirectPolicy));

    if (!options.verifyTls && parsed.value().secure) {
        DWORD securityFlags = SECURITY_FLAG_IGNORE_UNKNOWN_CA | SECURITY_FLAG_IGNORE_CERT_DATE_INVALID |
                              SECURITY_FLAG_IGNORE_CERT_CN_INVALID | SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
        WinHttpSetOption(requestHandle.get(), WINHTTP_OPTION_SECURITY_FLAGS, &securityFlags, sizeof(securityFlags));
    }

    const LPCWSTR headers = extraHeaders.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : extraHeaders.c_str();
    const DWORD headerLength = extraHeaders.empty() ? 0 : static_cast<DWORD>(extraHeaders.size());
    if (!WinHttpSendRequest(requestHandle.get(), headers, headerLength, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
        return failLast<WinHttpRequest>(ErrorCode::NetworkError, "WinHttpSendRequest");
    }
    if (!WinHttpReceiveResponse(requestHandle.get(), nullptr)) {
        return failLast<WinHttpRequest>(ErrorCode::NetworkError, "WinHttpReceiveResponse");
    }

    WinHttpRequest opened;
    opened.session = std::move(session);
    opened.connection = std::move(connection);
    opened.request = std::move(requestHandle);
    return Result<WinHttpRequest>::ok(std::move(opened));
}

Result<DWORD> statusCode(HINTERNET request) {
    DWORD status = 0;
    DWORD size = sizeof(status);
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX)) {
        return failLast<DWORD>(ErrorCode::NetworkError, "WinHttpQueryHeaders");
    }
    return Result<DWORD>::ok(status);
}

std::string queryHeader(HINTERNET request, const wchar_t* name) {
    DWORD size = 0;
    WinHttpQueryHeaders(request, WINHTTP_QUERY_CUSTOM, name, WINHTTP_NO_OUTPUT_BUFFER, &size, WINHTTP_NO_HEADER_INDEX);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || size == 0) {
        return {};
    }

    std::wstring value(size / sizeof(wchar_t), L'\0');
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_CUSTOM, name, value.data(), &size, WINHTTP_NO_HEADER_INDEX)) {
        return {};
    }
    value.resize(size / sizeof(wchar_t));
    while (!value.empty() && value.back() == L'\0') {
        value.pop_back();
    }
    return narrow(value);
}

std::uint64_t queryContentLength(HINTERNET request) {
    DWORD value = 0;
    DWORD size = sizeof(value);
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &value, &size, WINHTTP_NO_HEADER_INDEX)) {
        return 0;
    }
    return value;
}

Result<std::vector<char>> readResponseBytes(HINTERNET request, CancellationToken& cancel, ProgressCallback progress,
                                            const std::string& currentFile, std::uint64_t initialBytes,
                                            std::ofstream* output) {
    std::vector<char> bytes;
    std::uint64_t downloaded = initialBytes;
    const std::uint64_t total = initialBytes + queryContentLength(request);

    for (;;) {
        if (cancel.isCancelled()) {
            return Result<std::vector<char>>::fail({ErrorCode::Cancelled, "Operation cancelled"});
        }

        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request, &available)) {
            return failLast<std::vector<char>>(ErrorCode::NetworkError, "WinHttpQueryDataAvailable");
        }
        if (available == 0) {
            break;
        }

        std::vector<char> buffer(available);
        DWORD read = 0;
        if (!WinHttpReadData(request, buffer.data(), available, &read)) {
            return failLast<std::vector<char>>(ErrorCode::NetworkError, "WinHttpReadData");
        }
        if (read == 0) {
            break;
        }

        if (output) {
            output->write(buffer.data(), static_cast<std::streamsize>(read));
            if (!*output) {
                return Result<std::vector<char>>::fail({ErrorCode::DownloadFailed, "Failed to write target file"});
            }
        } else {
            bytes.insert(bytes.end(), buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(read));
        }
        downloaded += read;
        if (progress) {
            progress({downloaded, total, currentFile});
        }
    }

    return Result<std::vector<char>>::ok(std::move(bytes));
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
    if (url.find("://") == std::string::npos) {
        return Result<std::filesystem::path>::ok(url);
    }
    return Result<std::filesystem::path>::fail({ErrorCode::NetworkError, "WinHTTP supports only HTTP and HTTPS URLs"});
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

Result<DownloadResult> copyLocalToFile(const std::string& url, const std::filesystem::path& target,
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
        const auto outputMode =
            (resume && resume->offset > 0) ? (std::ios::binary | std::ios::app) : (std::ios::binary | std::ios::trunc);
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

std::wstring resumeHeaders(const NetworkOptions& options, const std::optional<DownloadResumeInfo>& resume) {
    if (!options.enableResume || !resume || resume->offset == 0) {
        return {};
    }

    std::wstring headers = L"Range: bytes=" + widen(std::to_string(resume->offset)) + L"-\r\n";
    if (!resume->etag.empty()) {
        headers += L"If-Range: " + widen(resume->etag) + L"\r\n";
    } else if (!resume->lastModified.empty()) {
        headers += L"If-Range: " + widen(resume->lastModified) + L"\r\n";
    }
    return headers;
}

class WinHttpNetworkClient final : public INetworkClient {
  public:
    Result<std::string> getText(const std::string& url, const NetworkOptions& options,
                                CancellationToken& cancel) noexcept override {
        if (!isHttpUrl(url)) {
            return readLocalText(url, cancel);
        }

        try {
            auto request = openRequest(url, options, {});
            if (!request) {
                return Result<std::string>::fail(request.error());
            }
            auto status = statusCode(request.value().get());
            if (!status) {
                return Result<std::string>::fail(status.error());
            }
            if (status.value() >= 400) {
                return Result<std::string>::fail(
                    {ErrorCode::NetworkError, "HTTP error " + std::to_string(status.value())});
            }

            auto bytes = readResponseBytes(request.value().get(), cancel, {}, {}, 0, nullptr);
            if (!bytes) {
                return Result<std::string>::fail(bytes.error());
            }
            return Result<std::string>::ok(std::string(bytes.value().begin(), bytes.value().end()));
        } catch (...) {
            return Result<std::string>::fail({ErrorCode::NetworkError, "Unexpected WinHTTP request failure"});
        }
    }

    Result<DownloadResult> downloadToFile(const std::string& url, const std::filesystem::path& target,
                                          const NetworkOptions& options,
                                          const std::optional<DownloadResumeInfo>& resume, ProgressCallback progress,
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

            const auto requestHeaders = resumeHeaders(options, resume);
            auto request = openRequest(url, options, requestHeaders);
            if (!request) {
                return Result<DownloadResult>::fail(request.error());
            }

            auto status = statusCode(request.value().get());
            if (!status) {
                return Result<DownloadResult>::fail(status.error());
            }
            if (options.enableResume && resume && resume->offset > 0 && status.value() == 200) {
                std::filesystem::remove(target, ec);
                return Result<DownloadResult>::fail({ErrorCode::DownloadFailed, "Server ignored Range request"});
            }
            if (status.value() >= 400) {
                return Result<DownloadResult>::fail(
                    {ErrorCode::DownloadFailed, "HTTP error " + std::to_string(status.value())});
            }

            const auto outputMode = (options.enableResume && resume && resume->offset > 0)
                                        ? (std::ios::binary | std::ios::app)
                                        : (std::ios::binary | std::ios::trunc);
            std::ofstream output(target, outputMode);
            if (!output) {
                return Result<DownloadResult>::fail({ErrorCode::DownloadFailed, "Failed to open target file"});
            }

            const auto initialBytes = (options.enableResume && resume) ? resume->offset : 0;
            auto bytes = readResponseBytes(request.value().get(), cancel, std::move(progress), target.generic_string(),
                                           initialBytes, &output);
            if (!bytes) {
                return Result<DownloadResult>::fail(
                    {bytes.error().code == ErrorCode::NetworkError ? ErrorCode::DownloadFailed : bytes.error().code,
                     bytes.error().message});
            }

            DownloadResult result;
            result.bytesWritten = initialBytes;
            std::error_code sizeError;
            const auto finalSize = std::filesystem::file_size(target, sizeError);
            if (!sizeError) {
                result.bytesWritten = finalSize;
            }
            result.etag = queryHeader(request.value().get(), L"ETag");
            result.lastModified = queryHeader(request.value().get(), L"Last-Modified");
            return Result<DownloadResult>::ok(std::move(result));
        } catch (...) {
            return Result<DownloadResult>::fail({ErrorCode::DownloadFailed, "Unexpected WinHTTP download failure"});
        }
    }
};

} // namespace

std::shared_ptr<INetworkClient> createDefaultNetworkClient() {
    return std::make_shared<WinHttpNetworkClient>();
}

} // namespace autoupdater

#endif
