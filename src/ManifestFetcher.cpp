#include "ManifestFetcher.h"

namespace autoupdater {

Result<ManifestEnvelope> fetchAndVerifyManifest(const Config& config,
                                                INetworkClient& network,
                                                IHashProvider& hashProvider,
                                                ISignatureVerifier& signatureVerifier,
                                                CancellationToken& cancel) {
    if (config.manifestUrl.empty()) {
        return Result<ManifestEnvelope>::fail({ErrorCode::InvalidConfig, "manifestUrl is required"});
    }

    auto raw = network.getText(config.manifestUrl, config.network, cancel);
    if (!raw) {
        return Result<ManifestEnvelope>::fail(raw.error());
    }

    if (config.security.requireManifestSignature) {
        const auto signatureUrl = config.security.manifestSignatureUrl.empty()
            ? config.manifestUrl + ".sig"
            : config.security.manifestSignatureUrl;
        auto signature = network.getText(signatureUrl, config.network, cancel);
        if (!signature) {
            return Result<ManifestEnvelope>::fail(signature.error());
        }
        auto verified = signatureVerifier.verify(raw.value(), signature.value(), config.security.publicKeyPem);
        if (!verified) {
            return Result<ManifestEnvelope>::fail(verified.error());
        }
    }

    auto parsed = Manifest::parse(raw.value());
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

