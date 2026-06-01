#include "libAutoUpdater/interfaces/ISignatureVerifier.h"

#ifdef LIBAUTOUPDATER_HAS_OPENSSL

#include <openssl/evp.h>
#include <openssl/pem.h>

#include <memory>

namespace autoupdater {

namespace {

struct BioDeleter {
    void operator()(BIO* bio) const noexcept { BIO_free(bio); }
};

struct PKeyDeleter {
    void operator()(EVP_PKEY* key) const noexcept { EVP_PKEY_free(key); }
};

struct CtxDeleter {
    void operator()(EVP_MD_CTX* ctx) const noexcept { EVP_MD_CTX_free(ctx); }
};

class OpenSslSignatureVerifier final : public ISignatureVerifier {
public:
    Result<void> verify(std::string_view payload,
                        std::string_view signatureBytes,
                        std::string_view publicKeyPem) noexcept override {
        if (publicKeyPem.empty()) {
            return Result<void>::fail({ErrorCode::ManifestSignatureInvalid, "Public key is empty"});
        }
        try {
            std::unique_ptr<BIO, BioDeleter> bio(BIO_new_mem_buf(publicKeyPem.data(), static_cast<int>(publicKeyPem.size())));
            if (!bio) {
                return Result<void>::fail({ErrorCode::ManifestSignatureInvalid, "Failed to load public key BIO"});
            }
            std::unique_ptr<EVP_PKEY, PKeyDeleter> key(PEM_read_bio_PUBKEY(bio.get(), nullptr, nullptr, nullptr));
            if (!key) {
                return Result<void>::fail({ErrorCode::ManifestSignatureInvalid, "Failed to parse public key"});
            }

            std::unique_ptr<EVP_MD_CTX, CtxDeleter> ctx(EVP_MD_CTX_new());
            if (!ctx) {
                return Result<void>::fail({ErrorCode::ManifestSignatureInvalid, "Failed to create OpenSSL context"});
            }

            const int keyType = EVP_PKEY_base_id(key.get());
            const EVP_MD* digest = keyType == EVP_PKEY_ED25519 ? nullptr : EVP_sha256();
            if (EVP_DigestVerifyInit(ctx.get(), nullptr, digest, nullptr, key.get()) != 1) {
                return Result<void>::fail({ErrorCode::ManifestSignatureInvalid, "EVP_DigestVerifyInit failed"});
            }

            int ok = 0;
            if (keyType == EVP_PKEY_ED25519) {
                ok = EVP_DigestVerify(
                    ctx.get(),
                    reinterpret_cast<const unsigned char*>(signatureBytes.data()),
                    signatureBytes.size(),
                    reinterpret_cast<const unsigned char*>(payload.data()),
                    payload.size());
            } else {
                if (EVP_DigestVerifyUpdate(ctx.get(), payload.data(), payload.size()) != 1) {
                    return Result<void>::fail({ErrorCode::ManifestSignatureInvalid, "EVP_DigestVerifyUpdate failed"});
                }
                ok = EVP_DigestVerifyFinal(
                    ctx.get(),
                    reinterpret_cast<const unsigned char*>(signatureBytes.data()),
                    signatureBytes.size());
            }

            if (ok != 1) {
                return Result<void>::fail({ErrorCode::ManifestSignatureInvalid, "Manifest signature is invalid"});
            }
            return Result<void>::ok();
        } catch (...) {
            return Result<void>::fail({ErrorCode::ManifestSignatureInvalid, "Unexpected signature verification failure"});
        }
    }
};

} // namespace

std::shared_ptr<ISignatureVerifier> createDefaultSignatureVerifier() {
    return std::make_shared<OpenSslSignatureVerifier>();
}

} // namespace autoupdater

#endif

