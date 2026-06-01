#pragma once

#include "UpdateTypes.h"
#include "libAutoUpdater/interfaces/IHashProvider.h"
#include "libAutoUpdater/interfaces/INetworkClient.h"
#include "libAutoUpdater/interfaces/ISignatureVerifier.h"

namespace autoupdater {

Result<ManifestEnvelope> fetchAndVerifyManifest(const Config& config,
                                                INetworkClient& network,
                                                IHashProvider& hashProvider,
                                                ISignatureVerifier& signatureVerifier,
                                                CancellationToken& cancel);

} // namespace autoupdater

