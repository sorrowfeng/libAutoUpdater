#include "util/UrlUtil.h"

#include "util/PathUtil.h"

#include <algorithm>
#include <array>
#include <limits>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace autoupdater::util {

namespace {

Error invalidUrl(const std::string& message) {
    return {ErrorCode::SecurityPolicyViolation, message};
}

bool isControlOrSpace(unsigned char ch) {
    return ch <= 0x20 || ch == 0x7f;
}

bool isControl(unsigned char ch) {
    return ch < 0x20 || ch == 0x7f;
}

int hexValue(char ch) {
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return 10 + (ch - 'a');
    }
    if (ch >= 'A' && ch <= 'F') {
        return 10 + (ch - 'A');
    }
    return -1;
}

char upperHex(unsigned int value) {
    constexpr char digits[] = "0123456789ABCDEF";
    return digits[value & 0x0fU];
}

bool isUnreserved(unsigned char ch) {
    return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-' || ch == '.' ||
           ch == '_' || ch == '~';
}

bool isSubDelimiter(unsigned char ch) {
    switch (ch) {
    case '!':
    case '$':
    case '&':
    case '\'':
    case '(':
    case ')':
    case '*':
    case '+':
    case ',':
    case ';':
    case '=':
        return true;
    default:
        return false;
    }
}

enum class UriComponent { PathSegment, Query };

bool isAllowedRawCharacter(unsigned char ch, UriComponent component) {
    if (isUnreserved(ch) || isSubDelimiter(ch) || ch == ':' || ch == '@') {
        return true;
    }
    return component == UriComponent::Query && (ch == '/' || ch == '?');
}

bool isAsciiAlpha(unsigned char ch) {
    return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z');
}

bool isAsciiDigit(unsigned char ch) {
    return ch >= '0' && ch <= '9';
}

bool isAsciiAlnum(unsigned char ch) {
    return isAsciiAlpha(ch) || isAsciiDigit(ch);
}

std::string lowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) { return static_cast<char>(ch >= 'A' && ch <= 'Z' ? ch + ('a' - 'A') : ch); });
    return value;
}

bool equalsAsciiCaseInsensitive(std::string_view left, std::string_view right) {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t i = 0; i < left.size(); ++i) {
        const auto leftChar = static_cast<unsigned char>(left[i]);
        const auto rightChar = static_cast<unsigned char>(right[i]);
        const auto lowerLeft = leftChar >= 'A' && leftChar <= 'Z' ? leftChar + ('a' - 'A') : leftChar;
        const auto lowerRight = rightChar >= 'A' && rightChar <= 'Z' ? rightChar + ('a' - 'A') : rightChar;
        if (lowerLeft != lowerRight) {
            return false;
        }
    }
    return true;
}

Result<std::string> normalizePercentEncoded(std::string_view input, UriComponent component) {
    std::string output;
    output.reserve(input.size());
    for (std::size_t i = 0; i < input.size(); ++i) {
        const auto raw = static_cast<unsigned char>(input[i]);
        if (isControlOrSpace(raw) || raw == '\\' || raw >= 0x80) {
            return Result<std::string>::fail(invalidUrl("URL contains whitespace, control characters, or backslashes"));
        }
        if (raw != '%') {
            if (!isAllowedRawCharacter(raw, component)) {
                return Result<std::string>::fail(
                    invalidUrl("URL contains a character not allowed in this URI component"));
            }
            output.push_back(static_cast<char>(raw));
            continue;
        }
        if (i + 2 >= input.size()) {
            return Result<std::string>::fail(invalidUrl("URL contains an incomplete percent escape"));
        }
        const int high = hexValue(input[i + 1]);
        const int low = hexValue(input[i + 2]);
        if (high < 0 || low < 0) {
            return Result<std::string>::fail(invalidUrl("URL contains an invalid percent escape"));
        }
        const auto decoded = static_cast<unsigned char>((high << 4) | low);
        if (isControl(decoded) || decoded == '\\' || (component == UriComponent::PathSegment && decoded == '/')) {
            return Result<std::string>::fail(invalidUrl("URL contains an encoded separator or control character"));
        }
        if (isUnreserved(decoded)) {
            output.push_back(static_cast<char>(decoded));
        } else {
            output.push_back('%');
            output.push_back(upperHex(static_cast<unsigned int>(high)));
            output.push_back(upperHex(static_cast<unsigned int>(low)));
        }
        i += 2;
    }
    return Result<std::string>::ok(std::move(output));
}

bool containsEncodedDot(std::string_view segment) {
    for (std::size_t i = 0; i + 2 < segment.size(); ++i) {
        if (segment[i] == '%' && hexValue(segment[i + 1]) == 2 && hexValue(segment[i + 2]) == 14) {
            return true;
        }
    }
    return false;
}

Result<std::string> normalizePath(std::string_view rawPath) {
    if (rawPath.empty() || rawPath.front() != '/') {
        return Result<std::string>::fail(invalidUrl("URL path must be absolute"));
    }

    std::vector<std::string> segments;
    std::size_t start = 1;
    while (start <= rawPath.size()) {
        const auto end = rawPath.find('/', start);
        const auto rawSegment =
            rawPath.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
        auto normalized = normalizePercentEncoded(rawSegment, UriComponent::PathSegment);
        if (!normalized) {
            return normalized;
        }
        if ((normalized.value() == "." || normalized.value() == "..") && containsEncodedDot(rawSegment)) {
            return Result<std::string>::fail(invalidUrl("URL contains a percent-encoded dot segment"));
        }
        if (normalized.value() == ".") {
            if (end == std::string_view::npos) {
                segments.emplace_back();
            }
        } else if (normalized.value() == "..") {
            if (segments.empty()) {
                return Result<std::string>::fail(invalidUrl("URL path escapes above its root"));
            }
            segments.pop_back();
            if (end == std::string_view::npos) {
                segments.emplace_back();
            }
        } else {
            segments.push_back(std::move(normalized.value()));
        }
        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1;
    }

    std::string path = "/";
    for (std::size_t i = 0; i < segments.size(); ++i) {
        if (i != 0) {
            path.push_back('/');
        }
        path += segments[i];
    }
    return Result<std::string>::ok(std::move(path));
}

bool parseCanonicalIpv4(std::string_view text, std::array<unsigned char, 4>& bytes) {
    std::size_t start = 0;
    for (std::size_t part = 0; part < bytes.size(); ++part) {
        const auto end = text.find('.', start);
        if ((part + 1 < bytes.size() && end == std::string_view::npos) ||
            (part + 1 == bytes.size() && end != std::string_view::npos)) {
            return false;
        }
        const auto token = text.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
        if (token.empty() || (token.size() > 1 && token.front() == '0')) {
            return false;
        }
        unsigned int value = 0;
        for (const char ch : token) {
            if (ch < '0' || ch > '9') {
                return false;
            }
            value = value * 10U + static_cast<unsigned int>(ch - '0');
            if (value > 255U) {
                return false;
            }
        }
        bytes[part] = static_cast<unsigned char>(value);
        start = end == std::string_view::npos ? text.size() : end + 1;
    }
    return start == text.size();
}

bool isPublicIpv4(const std::array<unsigned char, 4>& ip) {
    const unsigned int a = ip[0];
    const unsigned int b = ip[1];
    const unsigned int c = ip[2];
    if (a == 0 || a == 10 || a == 127 || a >= 224) {
        return false;
    }
    if (a == 100 && b >= 64 && b <= 127) {
        return false;
    }
    if (a == 169 && b == 254) {
        return false;
    }
    if (a == 172 && b >= 16 && b <= 31) {
        return false;
    }
    if (a == 192 && ((b == 0) || (b == 168) || (b == 0 && c == 2))) {
        return false;
    }
    if (a == 198 && (b == 18 || b == 19 || (b == 51 && c == 100))) {
        return false;
    }
    if (a == 203 && b == 0 && c == 113) {
        return false;
    }
    return true;
}

bool parseIpv6Side(std::string_view side, bool ipv4MayAppear, std::vector<std::uint16_t>& groups) {
    if (side.empty()) {
        return true;
    }
    std::size_t start = 0;
    while (start <= side.size()) {
        const auto end = side.find(':', start);
        const auto token = side.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
        if (token.empty()) {
            return false;
        }
        if (token.find('.') != std::string_view::npos) {
            if (!ipv4MayAppear || end != std::string_view::npos) {
                return false;
            }
            std::array<unsigned char, 4> ipv4{};
            if (!parseCanonicalIpv4(token, ipv4)) {
                return false;
            }
            groups.push_back(static_cast<std::uint16_t>((ipv4[0] << 8U) | ipv4[1]));
            groups.push_back(static_cast<std::uint16_t>((ipv4[2] << 8U) | ipv4[3]));
        } else {
            if (token.size() > 4) {
                return false;
            }
            std::uint16_t value = 0;
            for (const char ch : token) {
                const int digit = hexValue(ch);
                if (digit < 0) {
                    return false;
                }
                value = static_cast<std::uint16_t>((value << 4U) | static_cast<unsigned int>(digit));
            }
            groups.push_back(value);
        }
        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1;
    }
    return true;
}

bool parseIpv6(std::string_view text, std::array<unsigned char, 16>& bytes) {
    if (text.empty() || text.find('%') != std::string_view::npos) {
        return false;
    }
    const auto compressed = text.find("::");
    if (compressed != std::string_view::npos && text.find("::", compressed + 2) != std::string_view::npos) {
        return false;
    }

    std::vector<std::uint16_t> left;
    std::vector<std::uint16_t> right;
    if (compressed == std::string_view::npos) {
        if (!parseIpv6Side(text, true, left) || left.size() != 8) {
            return false;
        }
    } else {
        if (!parseIpv6Side(text.substr(0, compressed), false, left) ||
            !parseIpv6Side(text.substr(compressed + 2), true, right) || left.size() + right.size() >= 8) {
            return false;
        }
    }

    std::vector<std::uint16_t> groups = left;
    if (compressed != std::string_view::npos) {
        groups.insert(groups.end(), 8 - left.size() - right.size(), 0);
        groups.insert(groups.end(), right.begin(), right.end());
    }
    if (groups.size() != 8) {
        return false;
    }
    for (std::size_t i = 0; i < groups.size(); ++i) {
        bytes[i * 2] = static_cast<unsigned char>((groups[i] >> 8U) & 0xffU);
        bytes[i * 2 + 1] = static_cast<unsigned char>(groups[i] & 0xffU);
    }
    return true;
}

bool isPublicIpv6(const std::array<unsigned char, 16>& ip) {
    // Only globally routed unicast space is accepted. This rejects unspecified,
    // loopback, mapped IPv4, multicast, link-local, ULA, and legacy local ranges.
    if ((ip[0] & 0xe0U) != 0x20U) {
        return false;
    }
    // Documentation prefix 2001:db8::/32 is intentionally not globally routed.
    if (ip[0] == 0x20 && ip[1] == 0x01 && ip[2] == 0x0d && ip[3] == 0xb8) {
        return false;
    }
    return true;
}

std::string canonicalIpv6(const std::array<unsigned char, 16>& ip) {
    std::ostringstream output;
    output << std::hex;
    for (std::size_t i = 0; i < 8; ++i) {
        if (i != 0) {
            output << ':';
        }
        const auto group = static_cast<unsigned int>((ip[i * 2] << 8U) | ip[i * 2 + 1]);
        output << group;
    }
    return output.str();
}

Result<std::uint16_t> parsePort(std::string_view text) {
    if (text.empty()) {
        return Result<std::uint16_t>::fail(invalidUrl("URL contains an empty port"));
    }
    unsigned int value = 0;
    for (const char ch : text) {
        if (ch < '0' || ch > '9') {
            return Result<std::uint16_t>::fail(invalidUrl("URL port is not numeric"));
        }
        value = value * 10U + static_cast<unsigned int>(ch - '0');
        if (value > std::numeric_limits<std::uint16_t>::max()) {
            return Result<std::uint16_t>::fail(invalidUrl("URL port is out of range"));
        }
    }
    if (value == 0) {
        return Result<std::uint16_t>::fail(invalidUrl("URL port must be non-zero"));
    }
    return Result<std::uint16_t>::ok(static_cast<std::uint16_t>(value));
}

Result<std::pair<std::string, bool>> normalizeHost(std::string_view rawHost) {
    if (rawHost.empty()) {
        return Result<std::pair<std::string, bool>>::fail(invalidUrl("URL host is empty"));
    }
    if (rawHost.front() == '[') {
        if (rawHost.size() < 3 || rawHost.back() != ']') {
            return Result<std::pair<std::string, bool>>::fail(invalidUrl("URL contains an invalid IPv6 literal"));
        }
        std::array<unsigned char, 16> address{};
        if (!parseIpv6(rawHost.substr(1, rawHost.size() - 2), address) || !isPublicIpv6(address)) {
            return Result<std::pair<std::string, bool>>::fail(
                invalidUrl("URL contains a non-public or invalid IPv6 literal"));
        }
        return Result<std::pair<std::string, bool>>::ok({canonicalIpv6(address), true});
    }

    std::string host(rawHost);
    for (const unsigned char ch : host) {
        if (ch >= 0x80 || isControlOrSpace(ch)) {
            return Result<std::pair<std::string, bool>>::fail(invalidUrl("URL host must use ASCII DNS syntax"));
        }
    }
    host = lowerAscii(std::move(host));
    if (!host.empty() && host.back() == '.') {
        host.pop_back();
    }
    if (host.empty() || host == "localhost" ||
        (host.size() > 10 && host.compare(host.size() - 10, 10, ".localhost") == 0)) {
        return Result<std::pair<std::string, bool>>::fail(invalidUrl("Localhost URLs are not allowed"));
    }

    const bool digitsAndDots =
        std::all_of(host.begin(), host.end(), [](unsigned char ch) { return isAsciiDigit(ch) || ch == '.'; });
    if (digitsAndDots) {
        std::array<unsigned char, 4> address{};
        if (!parseCanonicalIpv4(host, address) || !isPublicIpv4(address)) {
            return Result<std::pair<std::string, bool>>::fail(
                invalidUrl("URL contains a non-public, ambiguous, or invalid IPv4 literal"));
        }
        return Result<std::pair<std::string, bool>>::ok({std::to_string(address[0]) + "." + std::to_string(address[1]) +
                                                             "." + std::to_string(address[2]) + "." +
                                                             std::to_string(address[3]),
                                                         false});
    }

    if (isAsciiDigit(static_cast<unsigned char>(host.front()))) {
        const bool numericLooking = std::all_of(host.begin(), host.end(), [](unsigned char ch) {
            return isAsciiDigit(ch) || (ch >= 'a' && ch <= 'f') || ch == 'x' || ch == '.';
        });
        if (numericLooking) {
            return Result<std::pair<std::string, bool>>::fail(invalidUrl("URL contains an ambiguous numeric host"));
        }
    }
    if (host.size() > 253) {
        return Result<std::pair<std::string, bool>>::fail(invalidUrl("URL host is too long"));
    }
    std::size_t labelStart = 0;
    while (labelStart <= host.size()) {
        const auto labelEnd = host.find('.', labelStart);
        const auto label =
            host.substr(labelStart, labelEnd == std::string::npos ? std::string::npos : labelEnd - labelStart);
        if (label.empty() || label.size() > 63 || label.front() == '-' || label.back() == '-' ||
            !std::all_of(label.begin(), label.end(), [](unsigned char ch) { return isAsciiAlnum(ch) || ch == '-'; })) {
            return Result<std::pair<std::string, bool>>::fail(invalidUrl("URL host contains an invalid DNS label"));
        }
        if (labelEnd == std::string::npos) {
            break;
        }
        labelStart = labelEnd + 1;
    }
    return Result<std::pair<std::string, bool>>::ok({std::move(host), false});
}

std::string schemeName(UrlScheme scheme) {
    switch (scheme) {
    case UrlScheme::Http:
        return "http";
    case UrlScheme::Https:
        return "https";
    case UrlScheme::File:
        return "file";
    }
    return {};
}

std::uint16_t defaultPort(UrlScheme scheme) {
    return scheme == UrlScheme::Https ? 443 : (scheme == UrlScheme::Http ? 80 : 0);
}

Result<void> validateFileUrlPath(std::string_view path) {
#ifdef _WIN32
    if (path.size() < 4 || path[0] != '/' || !isAsciiAlpha(static_cast<unsigned char>(path[1])) || path[2] != ':' ||
        path[3] != '/') {
        return Result<void>::fail(invalidUrl("Windows file URLs must contain an absolute drive path"));
    }
#else
    (void)path;
#endif
    return Result<void>::ok();
}

std::string serialize(const ParsedUrl& url, bool includeQuery = true) {
    std::string output = schemeName(url.scheme) + "://";
    if (url.scheme != UrlScheme::File) {
        if (url.ipv6Host) {
            output += "[" + url.host + "]";
        } else {
            output += url.host;
        }
        if (url.port != defaultPort(url.scheme)) {
            output += ":" + std::to_string(url.port);
        }
    }
    output += url.path;
    if (includeQuery && url.hasQuery) {
        output += "?" + url.query;
    }
    return output;
}

bool pathMatchesBoundary(std::string_view value, std::string_view prefix) {
    if (prefix == "/") {
        return !value.empty() && value.front() == '/';
    }
    if (value.size() < prefix.size() || value.compare(0, prefix.size(), prefix) != 0) {
        return false;
    }
    return value.size() == prefix.size() || prefix.back() == '/' || value[prefix.size()] == '/';
}

std::string percentDecode(std::string_view value) {
    std::string decoded;
    decoded.reserve(value.size());
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '%' && i + 2 < value.size()) {
            const int high = hexValue(value[i + 1]);
            const int low = hexValue(value[i + 2]);
            if (high >= 0 && low >= 0) {
                decoded.push_back(static_cast<char>((high << 4) | low));
                i += 2;
                continue;
            }
        }
        decoded.push_back(value[i]);
    }
    return decoded;
}

} // namespace

Result<ParsedUrl> parseAbsoluteUrl(std::string_view url) noexcept {
    try {
        if (url.empty()) {
            return Result<ParsedUrl>::fail(invalidUrl("URL is empty"));
        }
        for (const unsigned char ch : url) {
            if (isControlOrSpace(ch) || ch == '\\' || ch >= 0x80) {
                return Result<ParsedUrl>::fail(
                    invalidUrl("URL contains whitespace, control characters, or backslashes"));
            }
        }
        if (url.find('#') != std::string_view::npos) {
            return Result<ParsedUrl>::fail(invalidUrl("URL fragments are not allowed"));
        }

        const auto colon = url.find(':');
        if (colon == std::string_view::npos || colon == 0) {
            return Result<ParsedUrl>::fail(invalidUrl("URL must be absolute and include a scheme"));
        }
        for (std::size_t i = 0; i < colon; ++i) {
            const unsigned char ch = static_cast<unsigned char>(url[i]);
            if ((i == 0 && !isAsciiAlpha(ch)) || (i != 0 && !isAsciiAlnum(ch) && ch != '+' && ch != '-' && ch != '.')) {
                return Result<ParsedUrl>::fail(invalidUrl("URL contains an invalid scheme"));
            }
        }
        const auto scheme = lowerAscii(std::string(url.substr(0, colon)));
        ParsedUrl parsed;
        if (scheme == "http") {
            parsed.scheme = UrlScheme::Http;
        } else if (scheme == "https") {
            parsed.scheme = UrlScheme::Https;
        } else if (scheme == "file") {
            parsed.scheme = UrlScheme::File;
        } else {
            return Result<ParsedUrl>::fail(invalidUrl("URL scheme is not allowed"));
        }

        if (url.substr(colon + 1, 2) != "//") {
            return Result<ParsedUrl>::fail(invalidUrl("URL must use an authority introduced by //"));
        }
        const std::size_t authorityStart = colon + 3;
        const auto authorityEnd = url.find_first_of("/?", authorityStart);
        const auto authority =
            url.substr(authorityStart,
                       authorityEnd == std::string_view::npos ? std::string_view::npos : authorityEnd - authorityStart);
        if (authority.find('@') != std::string_view::npos) {
            return Result<ParsedUrl>::fail(invalidUrl("URL userinfo is not allowed"));
        }

        if (parsed.scheme == UrlScheme::File) {
            if (!authority.empty()) {
                return Result<ParsedUrl>::fail(invalidUrl("file URLs must not contain an authority"));
            }
            parsed.port = 0;
        } else {
            std::string_view rawHost = authority;
            std::string_view rawPort;
            bool hasExplicitPort = false;
            if (!authority.empty() && authority.front() == '[') {
                const auto closing = authority.find(']');
                if (closing == std::string_view::npos) {
                    return Result<ParsedUrl>::fail(invalidUrl("URL contains an invalid IPv6 authority"));
                }
                rawHost = authority.substr(0, closing + 1);
                const auto remainder = authority.substr(closing + 1);
                if (!remainder.empty()) {
                    if (remainder.front() != ':') {
                        return Result<ParsedUrl>::fail(invalidUrl("URL contains invalid data after its host"));
                    }
                    rawPort = remainder.substr(1);
                    hasExplicitPort = true;
                }
            } else {
                const auto portSeparator = authority.rfind(':');
                if (portSeparator != std::string_view::npos) {
                    if (authority.find(':') != portSeparator) {
                        return Result<ParsedUrl>::fail(invalidUrl("IPv6 URL hosts must use brackets"));
                    }
                    rawHost = authority.substr(0, portSeparator);
                    rawPort = authority.substr(portSeparator + 1);
                    hasExplicitPort = true;
                }
            }
            auto host = normalizeHost(rawHost);
            if (!host) {
                return Result<ParsedUrl>::fail(host.error());
            }
            parsed.host = std::move(host.value().first);
            parsed.ipv6Host = host.value().second;
            parsed.port = defaultPort(parsed.scheme);
            if (hasExplicitPort) {
                auto port = parsePort(rawPort);
                if (!port) {
                    return Result<ParsedUrl>::fail(port.error());
                }
                parsed.port = port.value();
            }
        }

        std::string_view pathAndQuery =
            authorityEnd == std::string_view::npos ? std::string_view{} : url.substr(authorityEnd);
        const auto querySeparator = pathAndQuery.find('?');
        auto rawPath = pathAndQuery.substr(0, querySeparator);
        if (rawPath.empty()) {
            rawPath = "/";
        }
        auto path = normalizePath(rawPath);
        if (!path) {
            return Result<ParsedUrl>::fail(path.error());
        }
        parsed.path = std::move(path.value());
        if (querySeparator != std::string_view::npos) {
            if (parsed.scheme == UrlScheme::File) {
                return Result<ParsedUrl>::fail(invalidUrl("file URLs must not contain a query"));
            }
            auto query = normalizePercentEncoded(pathAndQuery.substr(querySeparator + 1), UriComponent::Query);
            if (!query) {
                return Result<ParsedUrl>::fail(query.error());
            }
            parsed.query = std::move(query.value());
            parsed.hasQuery = true;
        }
        if (parsed.scheme == UrlScheme::File) {
            auto validFilePath = validateFileUrlPath(parsed.path);
            if (!validFilePath) {
                return Result<ParsedUrl>::fail(validFilePath.error());
            }
        }
        parsed.canonical = serialize(parsed);
        return Result<ParsedUrl>::ok(std::move(parsed));
    } catch (...) {
        return Result<ParsedUrl>::fail(invalidUrl("Unexpected URL parsing failure"));
    }
}

Result<ParsedUrl> resolveUrlReference(const ParsedUrl& base, std::string_view reference) noexcept {
    try {
        for (const unsigned char ch : reference) {
            if (isControlOrSpace(ch) || ch == '\\' || ch >= 0x80) {
                return Result<ParsedUrl>::fail(
                    invalidUrl("URL reference contains whitespace, control characters, or backslashes"));
            }
        }
        if (reference.find('#') != std::string_view::npos) {
            return Result<ParsedUrl>::fail(invalidUrl("URL fragments are not allowed"));
        }

        const auto firstDelimiter = reference.find_first_of("/?");
        const auto schemeSeparator = reference.find(':');
        if (schemeSeparator != std::string_view::npos &&
            (firstDelimiter == std::string_view::npos || schemeSeparator < firstDelimiter)) {
            return parseAbsoluteUrl(reference);
        }
        if (reference.substr(0, 2) == "//") {
            return parseAbsoluteUrl(schemeName(base.scheme) + ":" + std::string(reference));
        }
        if (reference.empty()) {
            return Result<ParsedUrl>::ok(base);
        }

        const auto querySeparator = reference.find('?');
        const auto referencePath = reference.substr(0, querySeparator);
        const auto referenceQuery =
            querySeparator == std::string_view::npos ? std::string_view{} : reference.substr(querySeparator + 1);

        ParsedUrl candidate = base;
        candidate.query.clear();
        candidate.hasQuery = false;
        if (referencePath.empty()) {
            if (querySeparator == std::string_view::npos) {
                candidate.query = base.query;
                candidate.hasQuery = base.hasQuery;
            } else {
                if (candidate.scheme == UrlScheme::File) {
                    return Result<ParsedUrl>::fail(invalidUrl("file URLs must not contain a query"));
                }
                auto query = normalizePercentEncoded(referenceQuery, UriComponent::Query);
                if (!query) {
                    return Result<ParsedUrl>::fail(query.error());
                }
                candidate.query = std::move(query.value());
                candidate.hasQuery = true;
            }
            candidate.canonical = serialize(candidate);
            return Result<ParsedUrl>::ok(std::move(candidate));
        }

        std::string mergedPath;
        if (referencePath.front() == '/') {
            mergedPath.assign(referencePath);
        } else {
            const auto slash = base.path.find_last_of('/');
            mergedPath = base.path.substr(0, slash == std::string::npos ? 0 : slash + 1);
            mergedPath.append(referencePath);
        }
        auto normalizedPath = normalizePath(mergedPath);
        if (!normalizedPath) {
            return Result<ParsedUrl>::fail(normalizedPath.error());
        }
        candidate.path = std::move(normalizedPath.value());
        if (candidate.scheme == UrlScheme::File) {
            auto validFilePath = validateFileUrlPath(candidate.path);
            if (!validFilePath) {
                return Result<ParsedUrl>::fail(validFilePath.error());
            }
        }
        if (querySeparator != std::string_view::npos) {
            if (candidate.scheme == UrlScheme::File) {
                return Result<ParsedUrl>::fail(invalidUrl("file URLs must not contain a query"));
            }
            auto query = normalizePercentEncoded(referenceQuery, UriComponent::Query);
            if (!query) {
                return Result<ParsedUrl>::fail(query.error());
            }
            candidate.query = std::move(query.value());
            candidate.hasQuery = true;
        }
        candidate.canonical = serialize(candidate);
        return Result<ParsedUrl>::ok(std::move(candidate));
    } catch (...) {
        return Result<ParsedUrl>::fail(invalidUrl("Unexpected URL resolution failure"));
    }
}

Result<ParsedUrl> resolveUrlReference(std::string_view base, std::string_view reference) noexcept {
    auto parsed = parseAbsoluteUrl(base);
    if (!parsed) {
        return parsed;
    }
    return resolveUrlReference(parsed.value(), reference);
}

Result<ParsedUrl> directoryUrl(const ParsedUrl& url) noexcept {
    try {
        ParsedUrl directory = url;
        const auto slash = directory.path.find_last_of('/');
        directory.path = directory.path.substr(0, slash == std::string::npos ? 1 : slash + 1);
        if (directory.path.empty()) {
            directory.path = "/";
        }
        directory.query.clear();
        directory.hasQuery = false;
        directory.canonical = serialize(directory);
        return Result<ParsedUrl>::ok(std::move(directory));
    } catch (...) {
        return Result<ParsedUrl>::fail(invalidUrl("Failed to derive URL directory"));
    }
}

Result<ParsedUrl> asDirectoryUrl(const ParsedUrl& url) noexcept {
    try {
        if (url.hasQuery) {
            return Result<ParsedUrl>::fail(invalidUrl("Directory URL must not contain a query"));
        }
        ParsedUrl directory = url;
        if (directory.path.empty()) {
            directory.path = "/";
        } else if (directory.path.back() != '/') {
            directory.path.push_back('/');
        }
        directory.canonical = serialize(directory);
        return Result<ParsedUrl>::ok(std::move(directory));
    } catch (...) {
        return Result<ParsedUrl>::fail(invalidUrl("Failed to normalize directory URL"));
    }
}

Result<ParsedUrl> appendUrlPathSuffix(const ParsedUrl& url, std::string_view suffix) noexcept {
    try {
        if (suffix.empty()) {
            return Result<ParsedUrl>::ok(url);
        }
        for (const unsigned char ch : suffix) {
            if (isControlOrSpace(ch) || ch == '\\' || ch == '/' || ch == '?' || ch == '#' || ch >= 0x80) {
                return Result<ParsedUrl>::fail(invalidUrl("URL path suffix contains an invalid character"));
            }
        }
        ParsedUrl result = url;
        result.path.append(suffix);
        auto normalized = normalizePath(result.path);
        if (!normalized) {
            return Result<ParsedUrl>::fail(normalized.error());
        }
        result.path = std::move(normalized.value());
        result.canonical = serialize(result);
        return Result<ParsedUrl>::ok(std::move(result));
    } catch (...) {
        return Result<ParsedUrl>::fail(invalidUrl("Failed to append URL path suffix"));
    }
}

Result<ParsedUrl> appendEncodedPath(const ParsedUrl& base, std::string_view portablePath) noexcept {
    try {
        if (base.path.empty() || base.path.back() != '/' || base.hasQuery) {
            return Result<ParsedUrl>::fail(invalidUrl("Artifact base URL must be a query-free directory URL"));
        }
        if (portablePath.empty() || portablePath.front() == '/' || portablePath.front() == '\\') {
            return Result<ParsedUrl>::fail(invalidUrl("Artifact path must be a non-empty relative path"));
        }

        std::string encoded;
        encoded.reserve(portablePath.size());
        std::size_t segmentStart = 0;
        while (segmentStart <= portablePath.size()) {
            const auto segmentEnd = portablePath.find('/', segmentStart);
            const auto segment =
                portablePath.substr(segmentStart, segmentEnd == std::string_view::npos ? std::string_view::npos
                                                                                       : segmentEnd - segmentStart);
            if (segment.empty() || segment == "." || segment == "..") {
                return Result<ParsedUrl>::fail(invalidUrl("Artifact path contains an unsafe segment"));
            }
            for (const unsigned char ch : segment) {
                if (isControl(ch) || ch == '\\') {
                    return Result<ParsedUrl>::fail(invalidUrl("Artifact path contains an unsafe character"));
                }
                if (isUnreserved(ch)) {
                    encoded.push_back(static_cast<char>(ch));
                } else {
                    encoded.push_back('%');
                    encoded.push_back(upperHex(ch >> 4U));
                    encoded.push_back(upperHex(ch));
                }
            }
            if (segmentEnd == std::string_view::npos) {
                break;
            }
            encoded.push_back('/');
            segmentStart = segmentEnd + 1;
        }

        return resolveUrlReference(base, encoded);
    } catch (...) {
        return Result<ParsedUrl>::fail(invalidUrl("Failed to append encoded artifact path"));
    }
}

bool urlsEquivalent(const ParsedUrl& left, const ParsedUrl& right) noexcept {
    return left.canonical == right.canonical;
}

bool sameOrigin(const ParsedUrl& left, const ParsedUrl& right) noexcept {
    return left.scheme == right.scheme && left.host == right.host && left.port == right.port;
}

std::string joinUrl(const std::string& baseUrl, const std::string& relativePath) {
    auto resolved = resolveUrlReference(baseUrl, relativePath);
    return resolved ? resolved.value().canonical : std::string{};
}

bool urlStartsWithAny(const std::string& url, const std::vector<std::string>& allowed) {
    if (allowed.empty()) {
        return false;
    }
    auto parsed = parseAbsoluteUrl(url);
    if (!parsed) {
        return false;
    }
    return std::any_of(allowed.begin(), allowed.end(), [&](const std::string& prefix) {
        auto rule = parseAbsoluteUrl(prefix);
        return rule && sameOrigin(parsed.value(), rule.value()) &&
               pathMatchesBoundary(parsed.value().path, rule.value().path);
    });
}

bool isFileUrl(const std::string& url) {
    const auto colon = url.find(':');
    return colon != std::string::npos && equalsAsciiCaseInsensitive(std::string_view(url).substr(0, colon), "file");
}

std::filesystem::path fileUrlToPath(const std::string& url) {
    auto parsed = parseAbsoluteUrl(url);
    if (!parsed || parsed.value().scheme != UrlScheme::File) {
        return {};
    }
    auto path = percentDecode(parsed.value().path);
#ifdef _WIN32
    if (path.size() >= 3 && path[0] == '/' && isAsciiAlpha(static_cast<unsigned char>(path[1])) && path[2] == ':') {
        path.erase(path.begin());
    }
    std::replace(path.begin(), path.end(), '/', '\\');
#endif
    return pathFromUtf8(path);
}

} // namespace autoupdater::util
