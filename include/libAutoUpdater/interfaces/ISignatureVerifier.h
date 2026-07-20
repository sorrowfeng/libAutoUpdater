#pragma once

#include "libAutoUpdater/Result.h"

#include <memory>
#include <string_view>

namespace autoupdater {

/// Detached manifest signature verifier abstraction.
/// Implementations must not copy the payload, signature, or key material into
/// Error::message.
class ISignatureVerifier {
  public:
    virtual ~ISignatureVerifier() = default;

    virtual Result<void> verify(std::string_view payload, std::string_view signatureBytes,
                                std::string_view publicKeyPem) noexcept = 0;
};

std::shared_ptr<ISignatureVerifier> createDefaultSignatureVerifier();
std::shared_ptr<ISignatureVerifier> createNullSignatureVerifier();

} // namespace autoupdater
