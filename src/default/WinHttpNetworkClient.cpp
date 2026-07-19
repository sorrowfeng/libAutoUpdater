#include "libAutoUpdater/interfaces/INetworkClient.h"

#ifdef LIBAUTOUPDATER_HAS_WINHTTP

#include "util/UrlUtil.h"
#include "util/PathUtil.h"

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

    DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    if (!WinHttpSetOption(requestHandle.get(), WINHTTP_OPTION_REDIRECT_POLICY, &redirectPolicy,
                          sizeof(redirectPolicy))) {
        return failLast<WinHttpRequest>(ErrorCode::NetworkError, "WinHttpSetOption redirect policy");
    }

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

void trimOptionalWhitespace(std::string& value) {
    const auto begin = value.find_first_not_of(" \t");
    if (begin == std::string::npos) {
        value.clear();
        return;
    }
    const auto end = value.find_last_not_of(" \t");
    value = value.substr(begin, end - begin + 1);
}

Result<std::vector<NetworkHeader>> responseHeaders(HINTERNET request) {
    DWORD size = 0;
    WinHttpQueryHeaders(request, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, WINHTTP_NO_OUTPUT_BUFFER,
                        &size, WINHTTP_NO_HEADER_INDEX);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || size == 0) {
        return failLast<std::vector<NetworkHeader>>(ErrorCode::NetworkError, "WinHttpQueryHeaders raw headers");
    }

    std::wstring raw(size / sizeof(wchar_t), L'\0');
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, raw.data(), &size,
                             WINHTTP_NO_HEADER_INDEX)) {
        return failLast<std::vector<NetworkHeader>>(ErrorCode::NetworkError, "WinHttpQueryHeaders raw headers");
    }
    raw.resize(size / sizeof(wchar_t));
    while (!raw.empty() && raw.back() == L'\0') {
        raw.pop_back();
    }

    std::vector<NetworkHeader> headers;
    std::size_t position = 0;
    bool firstLine = true;
    while (position <= raw.size()) {
        const auto end = raw.find(L"\r\n", position);
        const auto count = end == std::wstring::npos ? raw.size() - position : end - position;
        const auto line = narrow(raw.substr(position, count));
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
        if (end == std::wstring::npos) {
            break;
        }
        position = end + 2;
    }
    return Result<std::vector<NetworkHeader>>::ok(std::move(headers));
}

Result<std::string> responseUrl(HINTERNET request) {
    DWORD size = 0;
    WinHttpQueryOption(request, WINHTTP_OPTION_URL, WINHTTP_NO_OUTPUT_BUFFER, &size);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || size == 0) {
        return failLast<std::string>(ErrorCode::NetworkError, "WinHttpQueryOption URL");
    }

    std::wstring url(size / sizeof(wchar_t), L'\0');
    if (!WinHttpQueryOption(request, WINHTTP_OPTION_URL, url.data(), &size)) {
        return failLast<std::string>(ErrorCode::NetworkError, "WinHttpQueryOption URL");
    }
    url.resize(size / sizeof(wchar_t));
    while (!url.empty() && url.back() == L'\0') {
        url.pop_back();
    }
    return Result<std::string>::ok(narrow(url));
}

Result<NetworkResponseInfo> responseInfo(HINTERNET request) {
    auto status = statusCode(request);
    if (!status) {
        return Result<NetworkResponseInfo>::fail(status.error());
    }
    auto headers = responseHeaders(request);
    if (!headers) {
        return Result<NetworkResponseInfo>::fail(headers.error());
    }
    auto url = responseUrl(request);
    if (!url) {
        return Result<NetworkResponseInfo>::fail(url.error());
    }

    NetworkResponseInfo response;
    response.statusCode = static_cast<int>(status.value());
    response.headers = std::move(headers.value());
    response.effectiveUrl = std::move(url.value());
    return Result<NetworkResponseInfo>::ok(std::move(response));
}

std::string responseHeader(const NetworkResponseInfo& response, const std::string& name) {
    for (const auto& header : response.headers) {
        if (header.name == name) {
            return header.value;
        }
    }
    return {};
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
                                            IRootedFile* output) {
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
            auto written = output->write(buffer.data(), static_cast<std::size_t>(read));
            if (!written) {
                return Result<std::vector<char>>::fail(written.error());
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
    return Result<std::filesystem::path>::fail(
        {ErrorCode::NetworkError, "WinHTTP accepts only HTTP, HTTPS, and explicit file: URLs"});
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
    Result<TextResponse> getText(const std::string& url, const NetworkOptions& options,
                                 CancellationToken& cancel) noexcept override {
        if (!isHttpUrl(url)) {
            return readLocalText(url, cancel);
        }

        try {
            auto request = openRequest(url, options, {});
            if (!request) {
                return Result<TextResponse>::fail(request.error());
            }
            auto info = responseInfo(request.value().get());
            if (!info) {
                return Result<TextResponse>::fail(info.error());
            }

            auto bytes = readResponseBytes(request.value().get(), cancel, {}, {}, 0, nullptr);
            if (!bytes) {
                return Result<TextResponse>::fail(bytes.error());
            }
            TextResponse response;
            response.response = std::move(info.value());
            response.body.assign(bytes.value().begin(), bytes.value().end());
            return Result<TextResponse>::ok(std::move(response));
        } catch (...) {
            return Result<TextResponse>::fail({ErrorCode::NetworkError, "Unexpected WinHTTP request failure"});
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
            const auto requestHeaders = resumeHeaders(options, resume);
            auto request = openRequest(url, options, requestHeaders);
            if (!request) {
                return Result<DownloadResult>::fail(request.error());
            }

            auto info = responseInfo(request.value().get());
            if (!info) {
                return Result<DownloadResult>::fail(info.error());
            }
            if (cancel.isCancelled()) {
                return Result<DownloadResult>::fail({ErrorCode::Cancelled, "Operation cancelled"});
            }

            const bool appending = options.enableResume && resume && resume->offset > 0;
            const auto initialBytes = appending ? resume->offset : 0;
            const int writableStatus = appending ? 206 : 200;
            if (info.value().statusCode == writableStatus) {
                auto bytes =
                    readResponseBytes(request.value().get(), cancel, std::move(progress), {}, initialBytes, &target);
                if (!bytes) {
                    return Result<DownloadResult>::fail(
                        {bytes.error().code == ErrorCode::NetworkError ? ErrorCode::DownloadFailed : bytes.error().code,
                         bytes.error().message});
                }
            }

            DownloadResult result;
            result.response = std::move(info.value());
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
