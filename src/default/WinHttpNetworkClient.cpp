#include "libAutoUpdater/interfaces/INetworkClient.h"

#ifdef LIBAUTOUPDATER_HAS_WINHTTP

#include "NetworkLimits.h"
#include "default/LocalNetworkFile.h"

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cwctype>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace autoupdater {

namespace {

constexpr std::size_t kResponseReadBufferBytes = 64 * 1024;

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

    std::wstring requestHeaders = L"Accept-Encoding: identity\r\n";
    requestHeaders += extraHeaders;
    const LPCWSTR headers = requestHeaders.c_str();
    const DWORD headerLength = static_cast<DWORD>(requestHeaders.size());
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
    if (static_cast<std::uint64_t>(size) > detail::kMaxNetworkResponseHeaderBytes) {
        return Result<std::vector<NetworkHeader>>::fail(
            {ErrorCode::ResourceLimitExceeded, "HTTP response headers exceed their byte limit"});
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

Result<ReadResponseResult> readResponseBytes(HINTERNET request, const NetworkResponseInfo& response,
                                             std::uint64_t maxResponseBytes, std::uint64_t initialBytes,
                                             std::uint64_t totalBytes, ErrorCode readErrorCode,
                                             CancellationToken& cancel, ProgressCallback progress,
                                             const std::string& currentFile, IRootedFile* output) {
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
    std::array<char, kResponseReadBufferBytes> buffer{};

    for (;;) {
        if (cancel.isCancelled()) {
            return Result<ReadResponseResult>::fail({ErrorCode::Cancelled, "Operation cancelled"});
        }

        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request, &available)) {
            return failLast<ReadResponseResult>(readErrorCode, "WinHttpQueryDataAvailable");
        }
        if (available == 0) {
            break;
        }

        const auto remaining = bodyLimit - result.actualBytes;
        DWORD requested = static_cast<DWORD>(std::min<std::size_t>(static_cast<std::size_t>(available), buffer.size()));
        if (remaining < requested) {
            requested = static_cast<DWORD>(remaining + 1);
        }

        DWORD read = 0;
        if (!WinHttpReadData(request, buffer.data(), requested, &read)) {
            return failLast<ReadResponseResult>(readErrorCode, "WinHttpReadData");
        }
        if (read == 0) {
            break;
        }
        if (static_cast<std::uint64_t>(read) > remaining) {
            return Result<ReadResponseResult>::fail(
                {declared.value() ? readErrorCode : ErrorCode::ResourceLimitExceeded,
                 declared.value() ? "Response body exceeds its declared Content-Length"
                                  : "Response body exceeds its byte limit"});
        }

        if (output) {
            auto written = output->write(buffer.data(), static_cast<std::size_t>(read));
            if (!written) {
                return Result<ReadResponseResult>::fail(written.error());
            }
        } else {
            result.body.append(buffer.data(), static_cast<std::size_t>(read));
        }
        result.actualBytes += static_cast<std::uint64_t>(read);
        if (progress) {
            std::uint64_t downloaded = 0;
            if (!detail::checkedAdd(initialBytes, result.actualBytes, downloaded)) {
                return Result<ReadResponseResult>::fail(
                    {ErrorCode::ResourceLimitExceeded, "Download byte counter overflow"});
            }
            progress({downloaded, totalBytes, currentFile});
        }
    }

    auto completed = validateCompletedBody(response, result.actualBytes, maxResponseBytes, readErrorCode);
    if (!completed) {
        return Result<ReadResponseResult>::fail(completed.error());
    }
    return Result<ReadResponseResult>::ok(std::move(result));
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
    Result<TextResponse> getText(const std::string& url, const NetworkOptions& options, std::uint64_t maxResponseBytes,
                                 CancellationToken& cancel) noexcept override {
        if (!isHttpUrl(url)) {
            return detail::readLocalText(url, maxResponseBytes, cancel);
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

            TextResponse response;
            response.response = std::move(info.value());
            if (response.response.statusCode == 200) {
                auto bytes = readResponseBytes(request.value().get(), response.response, maxResponseBytes, 0,
                                               maxResponseBytes, ErrorCode::NetworkError, cancel, {}, {}, nullptr);
                if (!bytes) {
                    return Result<TextResponse>::fail(bytes.error());
                }
                response.body = std::move(bytes.value().body);
            }
            return Result<TextResponse>::ok(std::move(response));
        } catch (...) {
            return Result<TextResponse>::fail({ErrorCode::NetworkError, "Unexpected WinHTTP request failure"});
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

            const int writableStatus = appending ? 206 : 200;
            std::uint64_t responseBytes = 0;
            if (info.value().statusCode == writableStatus) {
                auto bytes = readResponseBytes(request.value().get(), info.value(), remaining.value(), initialBytes,
                                               maxTotalBytes, ErrorCode::DownloadFailed, cancel, std::move(progress),
                                               {}, &target);
                if (!bytes) {
                    return Result<DownloadResult>::fail(bytes.error());
                }
                responseBytes = bytes.value().actualBytes;
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
