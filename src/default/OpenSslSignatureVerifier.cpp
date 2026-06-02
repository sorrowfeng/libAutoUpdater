#include "libAutoUpdater/interfaces/ISignatureVerifier.h"

#ifdef LIBAUTOUPDATER_HAS_OPENSSL

#include <openssl/evp.h>
#include <openssl/pem.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace autoupdater {

namespace {

struct BioDeleter {
    void operator()(BIO* bio) const noexcept {
        BIO_free(bio);
    }
};

struct PKeyDeleter {
    void operator()(EVP_PKEY* key) const noexcept {
        EVP_PKEY_free(key);
    }
};

struct CtxDeleter {
    void operator()(EVP_MD_CTX* ctx) const noexcept {
        EVP_MD_CTX_free(ctx);
    }
};

std::string removeAsciiWhitespace(std::string_view input) {
    std::string output;
    output.reserve(input.size());
    for (const auto ch : input) {
        if (!std::isspace(static_cast<unsigned char>(ch))) {
            output.push_back(ch);
        }
    }
    return output;
}

std::optional<std::string> base64Decode(std::string_view input) {
    const auto clean = removeAsciiWhitespace(input);
    if (clean.empty() || clean.size() % 4 != 0) {
        return std::nullopt;
    }

    std::array<int, 256> table{};
    table.fill(-1);
    const std::string alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for (std::size_t i = 0; i < alphabet.size(); ++i) {
        table[static_cast<unsigned char>(alphabet[i])] = static_cast<int>(i);
    }

    std::string output;
    output.reserve(clean.size() / 4 * 3);
    for (std::size_t i = 0; i < clean.size(); i += 4) {
        int values[4]{};
        int padding = 0;
        for (int j = 0; j < 4; ++j) {
            const auto ch = clean[i + static_cast<std::size_t>(j)];
            if (ch == '=') {
                values[j] = 0;
                ++padding;
            } else {
                const int value = table[static_cast<unsigned char>(ch)];
                if (value < 0) {
                    return std::nullopt;
                }
                values[j] = value;
            }
        }

        const auto triple = (values[0] << 18) | (values[1] << 12) | (values[2] << 6) | values[3];
        output.push_back(static_cast<char>((triple >> 16) & 0xff));
        if (padding < 2) {
            output.push_back(static_cast<char>((triple >> 8) & 0xff));
        }
        if (padding < 1) {
            output.push_back(static_cast<char>(triple & 0xff));
        }
    }
    return output;
}

Result<void> verifyWithKey(EVP_PKEY* key, std::string_view payload, std::string_view signatureBytes) noexcept {
    std::unique_ptr<EVP_MD_CTX, CtxDeleter> ctx(EVP_MD_CTX_new());
    if (!ctx) {
        return Result<void>::fail({ErrorCode::ManifestSignatureInvalid, "Failed to create OpenSSL context"});
    }

    const int keyType = EVP_PKEY_base_id(key);
#ifdef EVP_PKEY_ED25519
    const bool isEd25519 = keyType == EVP_PKEY_ED25519;
#else
    const bool isEd25519 = false;
#endif
    const EVP_MD* digest = isEd25519 ? nullptr : EVP_sha256();
    if (EVP_DigestVerifyInit(ctx.get(), nullptr, digest, nullptr, key) != 1) {
        return Result<void>::fail({ErrorCode::ManifestSignatureInvalid, "EVP_DigestVerifyInit failed"});
    }

    int ok = 0;
    if (isEd25519) {
        ok = EVP_DigestVerify(ctx.get(), reinterpret_cast<const unsigned char*>(signatureBytes.data()),
                              signatureBytes.size(), reinterpret_cast<const unsigned char*>(payload.data()),
                              payload.size());
    } else {
        if (EVP_DigestVerifyUpdate(ctx.get(), payload.data(), payload.size()) != 1) {
            return Result<void>::fail({ErrorCode::ManifestSignatureInvalid, "EVP_DigestVerifyUpdate failed"});
        }
        ok = EVP_DigestVerifyFinal(ctx.get(), reinterpret_cast<const unsigned char*>(signatureBytes.data()),
                                   signatureBytes.size());
    }

    if (ok != 1) {
        return Result<void>::fail({ErrorCode::ManifestSignatureInvalid, "Manifest signature is invalid"});
    }
    return Result<void>::ok();
}

class OpenSslSignatureVerifier final : public ISignatureVerifier {
  public:
    Result<void> verify(std::string_view payload, std::string_view signatureBytes,
                        std::string_view publicKeyPem) noexcept override {
        if (publicKeyPem.empty()) {
            return Result<void>::fail({ErrorCode::ManifestSignatureInvalid, "Public key is empty"});
        }
        try {
            std::unique_ptr<BIO, BioDeleter> bio(
                BIO_new_mem_buf(publicKeyPem.data(), static_cast<int>(publicKeyPem.size())));
            if (!bio) {
                return Result<void>::fail({ErrorCode::ManifestSignatureInvalid, "Failed to load public key BIO"});
            }
            std::unique_ptr<EVP_PKEY, PKeyDeleter> key(PEM_read_bio_PUBKEY(bio.get(), nullptr, nullptr, nullptr));
            if (!key) {
                return Result<void>::fail({ErrorCode::ManifestSignatureInvalid, "Failed to parse public key"});
            }

            auto raw = verifyWithKey(key.get(), payload, signatureBytes);
            if (raw) {
                return raw;
            }

            const auto decoded = base64Decode(signatureBytes);
            if (decoded) {
                auto base64 = verifyWithKey(key.get(), payload, *decoded);
                if (base64) {
                    return base64;
                }
            }

            return raw;
        } catch (...) {
            return Result<void>::fail(
                {ErrorCode::ManifestSignatureInvalid, "Unexpected signature verification failure"});
        }
    }
};

} // namespace

std::shared_ptr<ISignatureVerifier> createDefaultSignatureVerifier() {
    return std::make_shared<OpenSslSignatureVerifier>();
}

} // namespace autoupdater

#endif
