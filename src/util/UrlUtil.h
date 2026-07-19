#pragma once

#include "libAutoUpdater/Result.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace autoupdater::util {

enum class UrlScheme { Http, Https, File };

/// A strict, normalized absolute URL. canonical contains the serialized form
/// used for policy comparisons and redirect-loop detection.
struct ParsedUrl {
    UrlScheme scheme = UrlScheme::Https;
    std::string host;
    std::uint16_t port = 0;
    bool ipv6Host = false;
    std::string path;
    std::string query;
    bool hasQuery = false;
    std::string canonical;
};

Result<ParsedUrl> parseAbsoluteUrl(std::string_view url) noexcept;
Result<ParsedUrl> resolveUrlReference(const ParsedUrl& base, std::string_view reference) noexcept;
Result<ParsedUrl> resolveUrlReference(std::string_view base, std::string_view reference) noexcept;
Result<ParsedUrl> directoryUrl(const ParsedUrl& url) noexcept;
Result<ParsedUrl> asDirectoryUrl(const ParsedUrl& url) noexcept;
Result<ParsedUrl> appendUrlPathSuffix(const ParsedUrl& url, std::string_view suffix) noexcept;
Result<ParsedUrl> appendEncodedPath(const ParsedUrl& base, std::string_view portablePath) noexcept;
bool urlsEquivalent(const ParsedUrl& left, const ParsedUrl& right) noexcept;
bool sameOrigin(const ParsedUrl& left, const ParsedUrl& right) noexcept;

std::string joinUrl(const std::string& baseUrl, const std::string& relativePath);
bool urlStartsWithAny(const std::string& url, const std::vector<std::string>& allowed);
bool isFileUrl(const std::string& url);
std::filesystem::path fileUrlToPath(const std::string& url);

} // namespace autoupdater::util
