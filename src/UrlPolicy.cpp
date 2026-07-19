#include "UrlPolicy.h"

#include <algorithm>
#include <string>

namespace autoupdater {

namespace {

Error invalidConfig(const std::string& message) {
    return {ErrorCode::InvalidConfig, message};
}

Error rejected(const std::string& message) {
    return {ErrorCode::SecurityPolicyViolation, message};
}

bool pathMatchesBoundary(std::string_view path, std::string_view scope, bool caseInsensitive) {
    if (scope == "/") {
        return !path.empty() && path.front() == '/';
    }
    if (path.size() < scope.size()) {
        return false;
    }
#ifdef _WIN32
    if (caseInsensitive) {
        for (std::size_t i = 0; i < scope.size(); ++i) {
            const auto pathChar = static_cast<unsigned char>(path[i]);
            const auto scopeChar = static_cast<unsigned char>(scope[i]);
            const auto lowerPath = pathChar >= 'A' && pathChar <= 'Z' ? pathChar + ('a' - 'A') : pathChar;
            const auto lowerScope = scopeChar >= 'A' && scopeChar <= 'Z' ? scopeChar + ('a' - 'A') : scopeChar;
            if (lowerPath != lowerScope) {
                return false;
            }
        }
    } else if (path.compare(0, scope.size(), scope) != 0) {
        return false;
    }
#else
    (void)caseInsensitive;
    if (path.compare(0, scope.size(), scope) != 0) {
        return false;
    }
#endif
    return path.size() == scope.size() || scope.back() == '/' || path[scope.size()] == '/';
}

Result<UrlPolicy> failConfigFrom(const Error& error, const std::string& context) {
    return Result<UrlPolicy>::fail(invalidConfig(context + ": " + error.message));
}

} // namespace

Result<UrlPolicy> UrlPolicy::fromConfig(const Config& config) noexcept {
    try {
        auto initial = util::parseAbsoluteUrl(config.manifestUrl);
        if (!initial) {
            return failConfigFrom(initial.error(), "Invalid manifestUrl");
        }

        UrlPolicy policy;
        policy.initial_ = std::move(initial.value());
        if (policy.initial_.scheme == util::UrlScheme::File) {
            if (!config.security.allowLocalFileUrls) {
                return Result<UrlPolicy>::fail(
                    invalidConfig("file manifest URLs require security.allowLocalFileUrls=true"));
            }
            auto localRoot = util::directoryUrl(policy.initial_);
            if (!localRoot) {
                return failConfigFrom(localRoot.error(), "Invalid local manifest URL root");
            }
            policy.mode_ = Mode::LocalFile;
            policy.localRoot_ = std::move(localRoot.value());
            return Result<UrlPolicy>::ok(std::move(policy));
        }

        if (config.security.allowedBaseUrls.empty()) {
            return Result<UrlPolicy>::fail(
                invalidConfig("security.allowedBaseUrls must not be empty for HTTP(S) updates"));
        }
        policy.mode_ = Mode::Network;
        policy.networkScopes_.reserve(config.security.allowedBaseUrls.size());
        for (const auto& configuredScope : config.security.allowedBaseUrls) {
            auto scope = util::parseAbsoluteUrl(configuredScope);
            if (!scope) {
                return failConfigFrom(scope.error(), "Invalid allowedBaseUrls entry");
            }
            if (scope.value().scheme == util::UrlScheme::File || scope.value().hasQuery) {
                return Result<UrlPolicy>::fail(
                    invalidConfig("allowedBaseUrls entries must be query-free HTTP(S) URLs"));
            }
            policy.networkScopes_.push_back(std::move(scope.value()));
        }
        if (!policy.matchesNetworkScope(policy.initial_)) {
            return Result<UrlPolicy>::fail(invalidConfig("manifestUrl is outside security.allowedBaseUrls"));
        }
        return Result<UrlPolicy>::ok(std::move(policy));
    } catch (...) {
        return Result<UrlPolicy>::fail(invalidConfig("Unexpected URL policy construction failure"));
    }
}

const util::ParsedUrl& UrlPolicy::initialUrl() const noexcept {
    return initial_;
}

Result<util::ParsedUrl> UrlPolicy::authorize(std::string_view url) const noexcept {
    auto parsed = util::parseAbsoluteUrl(url);
    if (!parsed) {
        return parsed;
    }
    auto authorized = authorize(parsed.value());
    if (!authorized) {
        return Result<util::ParsedUrl>::fail(authorized.error());
    }
    return parsed;
}

Result<void> UrlPolicy::authorize(const util::ParsedUrl& url) const noexcept {
    if (mode_ == Mode::LocalFile) {
        if (!matchesLocalScope(url)) {
            return Result<void>::fail(rejected("Local URL is outside the configured manifest directory"));
        }
        return Result<void>::ok();
    }
    if (!matchesNetworkScope(url)) {
        return Result<void>::fail(rejected("URL is outside security.allowedBaseUrls"));
    }
    return Result<void>::ok();
}

Result<void> UrlPolicy::authorizeTransition(const util::ParsedUrl& from, const util::ParsedUrl& to) const noexcept {
    auto sourceAllowed = authorize(from);
    if (!sourceAllowed) {
        return sourceAllowed;
    }
    auto targetAllowed = authorize(to);
    if (!targetAllowed) {
        return targetAllowed;
    }
    if (from.scheme == util::UrlScheme::Https && to.scheme == util::UrlScheme::Http) {
        return Result<void>::fail(rejected("HTTPS-to-HTTP URL downgrade is not allowed"));
    }
    if ((from.scheme == util::UrlScheme::File) != (to.scheme == util::UrlScheme::File)) {
        return Result<void>::fail(rejected("Switching between local file and network URLs is not allowed"));
    }
    return Result<void>::ok();
}

Result<util::ParsedUrl> UrlPolicy::resolveAndAuthorize(const util::ParsedUrl& base,
                                                       std::string_view reference) const noexcept {
    auto baseAllowed = authorize(base);
    if (!baseAllowed) {
        return Result<util::ParsedUrl>::fail(baseAllowed.error());
    }
    auto resolved = util::resolveUrlReference(base, reference);
    if (!resolved) {
        return resolved;
    }
    auto allowed = authorizeTransition(base, resolved.value());
    if (!allowed) {
        return Result<util::ParsedUrl>::fail(allowed.error());
    }
    return resolved;
}

bool UrlPolicy::matchesNetworkScope(const util::ParsedUrl& url) const noexcept {
    if (url.scheme == util::UrlScheme::File) {
        return false;
    }
    return std::any_of(networkScopes_.begin(), networkScopes_.end(), [&](const util::ParsedUrl& scope) {
        return util::sameOrigin(url, scope) && pathMatchesBoundary(url.path, scope.path, false);
    });
}

bool UrlPolicy::matchesLocalScope(const util::ParsedUrl& url) const noexcept {
    return url.scheme == util::UrlScheme::File && pathMatchesBoundary(url.path, localRoot_.path, true);
}

} // namespace autoupdater
