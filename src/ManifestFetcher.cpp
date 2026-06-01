#include "ManifestFetcher.h"

#include "util/UrlUtil.h"

namespace autoupdater {

namespace {

Result<std::string> fetchTextAndVerify(const std::string& url,
                                       const std::string& signatureUrlOverride,
                                       const Config& config,
                                       INetworkClient& network,
                                       ISignatureVerifier& signatureVerifier,
                                       CancellationToken& cancel) {
    auto raw = network.getText(url, config.network, cancel);
    if (!raw) {
        return Result<std::string>::fail(raw.error());
    }

    if (config.security.requireManifestSignature) {
        const auto signatureUrl = signatureUrlOverride.empty() ? url + ".sig" : signatureUrlOverride;
        auto signature = network.getText(signatureUrl, config.network, cancel);
        if (!signature) {
            return Result<std::string>::fail(signature.error());
        }
        auto verified = signatureVerifier.verify(raw.value(), signature.value(), config.security.publicKeyPem);
        if (!verified) {
            return Result<std::string>::fail(verified.error());
        }
    }

    return raw;
}

Result<std::string> selectIndexTarget(const Config& config, const IndexManifest& index) {
    if (!config.appId.empty() && !index.appId.empty() && config.appId != index.appId) {
        return Result<std::string>::fail({ErrorCode::SecurityPolicyViolation, "Index manifest appId does not match config"});
    }
    if (!config.channel.empty() && !index.channel.empty() && config.channel != index.channel) {
        return Result<std::string>::fail({ErrorCode::SecurityPolicyViolation, "Index manifest channel does not match config"});
    }

    for (const auto& target : index.targets) {
        const bool platformMatches = target.platform.empty() || config.platform.empty() || target.platform == config.platform;
        const bool archMatches = target.arch.empty() || config.arch.empty() || target.arch == config.arch;
        if (platformMatches && archMatches) {
            return Result<std::string>::ok(target.manifestUrl);
        }
    }

    return Result<std::string>::fail({ErrorCode::ManifestParseFailed, "No matching release manifest target in index manifest"});
}

} // namespace

Result<ManifestEnvelope> fetchAndVerifyManifest(const Config& config,
                                                INetworkClient& network,
                                                IHashProvider& hashProvider,
                                                ISignatureVerifier& signatureVerifier,
                                                CancellationToken& cancel) {
    if (config.manifestUrl.empty()) {
        return Result<ManifestEnvelope>::fail({ErrorCode::InvalidConfig, "manifestUrl is required"});
    }

    auto raw = fetchTextAndVerify(
        config.manifestUrl,
        config.security.manifestSignatureUrl,
        config,
        network,
        signatureVerifier,
        cancel);
    if (!raw) {
        return Result<ManifestEnvelope>::fail(raw.error());
    }

    auto parsed = Manifest::parse(raw.value());
    if (!parsed && parsed.error().code == ErrorCode::ManifestParseFailed) {
        auto index = IndexManifest::parse(raw.value());
        if (!index) {
            return Result<ManifestEnvelope>::fail(parsed.error());
        }
        auto target = selectIndexTarget(config, index.value());
        if (!target) {
            return Result<ManifestEnvelope>::fail(target.error());
        }
        if (!util::urlStartsWithAny(target.value(), config.security.allowedBaseUrls)) {
            return Result<ManifestEnvelope>::fail({ErrorCode::SecurityPolicyViolation, "Release manifest URL is not allowed"});
        }
        raw = fetchTextAndVerify(
            target.value(),
            {},
            config,
            network,
            signatureVerifier,
            cancel);
        if (!raw) {
            return Result<ManifestEnvelope>::fail(raw.error());
        }
        parsed = Manifest::parse(raw.value());
    }

    if (!parsed) {
        return Result<ManifestEnvelope>::fail(parsed.error());
    }
    auto manifestHash = hashProvider.sha256Bytes(raw.value());
    if (!manifestHash) {
        return Result<ManifestEnvelope>::fail(manifestHash.error());
    }

    ManifestEnvelope envelope;
    envelope.manifest = std::move(parsed.value());
    envelope.rawBytes = std::move(raw.value());
    envelope.sha256 = std::move(manifestHash.value());
    return Result<ManifestEnvelope>::ok(std::move(envelope));
}

} // namespace autoupdater
