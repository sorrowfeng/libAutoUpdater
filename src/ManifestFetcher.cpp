#include "ManifestFetcher.h"

#include "NetworkRequest.h"
#include "UrlPolicy.h"
#include "util/UrlUtil.h"

#include <algorithm>
#include <string>
#include <utility>

namespace autoupdater {

namespace {

struct VerifiedTextResult {
    RedirectedTextResult document;
    bool signatureVerified = false;
};

Result<util::ParsedUrl> signatureUrlFor(const RedirectedTextResult& document, const std::string& signatureUrlOverride,
                                        const UrlPolicy& policy) {
    auto source = policy.authorize(document.effectiveUrl);
    if (!source) {
        return Result<util::ParsedUrl>::fail(source.error());
    }
    if (!signatureUrlOverride.empty()) {
        return policy.resolveAndAuthorize(source.value(), signatureUrlOverride);
    }

    auto signature = util::appendUrlPathSuffix(source.value(), ".sig");
    if (!signature) {
        return signature;
    }
    auto allowed = policy.authorizeTransition(source.value(), signature.value());
    if (!allowed) {
        return Result<util::ParsedUrl>::fail(allowed.error());
    }
    return signature;
}

Result<VerifiedTextResult> fetchTextAndVerify(const std::string& url, const std::string& signatureUrlOverride,
                                              std::uint64_t maxDocumentBytes, const Config& config,
                                              const UrlPolicy& policy, INetworkClient& network,
                                              ISignatureVerifier& signatureVerifier, CancellationToken& cancel) {
    auto raw = fetchTextWithRedirects(url, config.network, maxDocumentBytes, policy, network, cancel);
    if (!raw) {
        return Result<VerifiedTextResult>::fail(raw.error());
    }

    bool signatureVerified = false;
    if (config.security.requireManifestSignature) {
        auto signatureUrl = signatureUrlFor(raw.value(), signatureUrlOverride, policy);
        if (!signatureUrl) {
            return Result<VerifiedTextResult>::fail(signatureUrl.error());
        }
        auto signature = fetchTextWithRedirects(signatureUrl.value().canonical, config.network,
                                                config.resources.maxSignatureBytes, policy, network, cancel);
        if (!signature) {
            return Result<VerifiedTextResult>::fail(signature.error());
        }
        auto verified =
            signatureVerifier.verify(raw.value().body, signature.value().body, config.security.publicKeyPem);
        if (!verified) {
            return Result<VerifiedTextResult>::fail(verified.error());
        }
        signatureVerified = true;
    }

    VerifiedTextResult result;
    result.document = std::move(raw.value());
    result.signatureVerified = signatureVerified;
    return Result<VerifiedTextResult>::ok(std::move(result));
}

Result<std::string> selectIndexTarget(const Config& config, const IndexManifest& index) {
    if (!config.appId.empty() && !index.appId.empty() && config.appId != index.appId) {
        return Result<std::string>::fail(
            {ErrorCode::SecurityPolicyViolation, "Index manifest appId does not match config"});
    }
    if (!config.channel.empty() && !index.channel.empty() && config.channel != index.channel) {
        return Result<std::string>::fail(
            {ErrorCode::SecurityPolicyViolation, "Index manifest channel does not match config"});
    }

    for (const auto& target : index.targets) {
        const bool platformMatches =
            target.platform.empty() || config.platform.empty() || target.platform == config.platform;
        const bool archMatches = target.arch.empty() || config.arch.empty() || target.arch == config.arch;
        if (platformMatches && archMatches) {
            return Result<std::string>::ok(target.manifestUrl);
        }
    }

    return Result<std::string>::fail(
        {ErrorCode::ManifestParseFailed, "No matching release manifest target in index manifest"});
}

Result<util::ParsedUrl> resolveArtifactBase(const Manifest& manifest, const std::string& sourceUrl,
                                            const UrlPolicy& policy) {
    auto source = policy.authorize(sourceUrl);
    if (!source) {
        return Result<util::ParsedUrl>::fail(source.error());
    }

    Result<util::ParsedUrl> base = manifest.baseUrl.empty()
                                       ? util::directoryUrl(source.value())
                                       : policy.resolveAndAuthorize(source.value(), manifest.baseUrl);
    if (!base) {
        return base;
    }
    auto directory = util::asDirectoryUrl(base.value());
    if (!directory) {
        return directory;
    }
    auto allowed = policy.authorizeTransition(source.value(), directory.value());
    if (!allowed) {
        return Result<util::ParsedUrl>::fail(allowed.error());
    }
    return directory;
}

} // namespace

Result<ManifestEnvelope> fetchAndVerifyManifest(const Config& config, INetworkClient& network,
                                                IHashProvider& hashProvider, ISignatureVerifier& signatureVerifier,
                                                CancellationToken& cancel) {
    if (config.manifestUrl.empty()) {
        return Result<ManifestEnvelope>::fail({ErrorCode::InvalidConfig, "manifestUrl is required"});
    }
    auto policy = UrlPolicy::fromConfig(config);
    if (!policy) {
        return Result<ManifestEnvelope>::fail(policy.error());
    }

    const auto initialDocumentLimit = std::max(config.resources.maxIndexBytes, config.resources.maxManifestBytes);
    auto raw = fetchTextAndVerify(policy.value().initialUrl().canonical, config.security.manifestSignatureUrl,
                                  initialDocumentLimit, config, policy.value(), network, signatureVerifier, cancel);
    if (!raw) {
        return Result<ManifestEnvelope>::fail(raw.error());
    }

    const bool withinManifestLimit = raw.value().document.body.size() <= config.resources.maxManifestBytes;
    const bool withinIndexLimit = raw.value().document.body.size() <= config.resources.maxIndexBytes;
    auto parsed = withinManifestLimit
                      ? Manifest::parse(raw.value().document.body, config.resources)
                      : Result<Manifest>::fail({ErrorCode::ResourceLimitExceeded, "Manifest exceeds its byte limit"});
    const bool mayBeIndex = !parsed && (parsed.error().code == ErrorCode::ManifestParseFailed ||
                                        (!withinManifestLimit && withinIndexLimit));
    if (!parsed && mayBeIndex) {
        auto index = IndexManifest::parse(raw.value().document.body, config.resources);
        if (!index) {
            return Result<ManifestEnvelope>::fail(
                parsed.error().code == ErrorCode::ResourceLimitExceeded ? parsed.error() : index.error());
        }
        auto target = selectIndexTarget(config, index.value());
        if (!target) {
            return Result<ManifestEnvelope>::fail(target.error());
        }
        auto indexSource = policy.value().authorize(raw.value().document.effectiveUrl);
        if (!indexSource) {
            return Result<ManifestEnvelope>::fail(indexSource.error());
        }
        auto releaseUrl = policy.value().resolveAndAuthorize(indexSource.value(), target.value());
        if (!releaseUrl) {
            return Result<ManifestEnvelope>::fail(releaseUrl.error());
        }
        raw = fetchTextAndVerify(releaseUrl.value().canonical, {}, config.resources.maxManifestBytes, config,
                                 policy.value(), network, signatureVerifier, cancel);
        if (!raw) {
            return Result<ManifestEnvelope>::fail(raw.error());
        }
        parsed = Manifest::parse(raw.value().document.body, config.resources);
    }

    if (!parsed) {
        return Result<ManifestEnvelope>::fail(parsed.error());
    }
    auto artifactBase = resolveArtifactBase(parsed.value(), raw.value().document.effectiveUrl, policy.value());
    if (!artifactBase) {
        return Result<ManifestEnvelope>::fail(artifactBase.error());
    }
    auto manifestHash = hashProvider.sha256Bytes(raw.value().document.body);
    if (!manifestHash) {
        return Result<ManifestEnvelope>::fail(manifestHash.error());
    }

    ManifestEnvelope envelope;
    envelope.manifest = std::move(parsed.value());
    envelope.rawBytes = std::move(raw.value().document.body);
    envelope.sha256 = std::move(manifestHash.value());
    envelope.sourceUrl = std::move(raw.value().document.effectiveUrl);
    envelope.artifactBaseUrl = std::move(artifactBase.value().canonical);
    envelope.releaseManifestSignatureVerified = raw.value().signatureVerified;
    return Result<ManifestEnvelope>::ok(std::move(envelope));
}

} // namespace autoupdater
