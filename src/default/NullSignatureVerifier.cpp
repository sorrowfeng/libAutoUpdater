#include "libAutoUpdater/interfaces/ISignatureVerifier.h"

namespace autoupdater {

namespace {

class NullSignatureVerifier final : public ISignatureVerifier {
  public:
    Result<void> verify(std::string_view, std::string_view, std::string_view) noexcept override {
        return Result<void>::fail({ErrorCode::ManifestSignatureInvalid, "No signature verifier is available"});
    }
};

} // namespace

std::shared_ptr<ISignatureVerifier> createNullSignatureVerifier() {
    return std::make_shared<NullSignatureVerifier>();
}

#ifndef LIBAUTOUPDATER_HAS_OPENSSL
std::shared_ptr<ISignatureVerifier> createDefaultSignatureVerifier() {
    return createNullSignatureVerifier();
}
#endif

} // namespace autoupdater
