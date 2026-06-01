#include "TestCommon.h"

#include "libAutoUpdater/interfaces/ISignatureVerifier.h"

void testOpenSslSignatureVerifier() {
#ifdef LIBAUTOUPDATER_TEST_HAS_OPENSSL
    const std::string payload = "schemaVersion=1;version=1.2.3";
    const std::string publicKeyPem =
        "-----BEGIN PUBLIC KEY-----\n"
        "MIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEAl/VSV4RpEZxz+fnJPoG/\n"
        "keTuxgdzfubNMGWg4AXEEUSvAXp9mBsknCvO6KqK42YjMqiSpeDnHMtzGi7P7Rcd\n"
        "7/VP/epbLK7qtDAXdYTmvKG9sgQHcn5bzNMUGxyR+5BMg4mZmcACmAIk8llMP9D2\n"
        "+5gs7ycGurWD1yDfM5qjSIZbMhth2E2OzeITD62F53H35qrnrg2QKDx1ZJkwHf4E\n"
        "aJ7Cg6MCBalYougtT8SUcZrPxCfC7zG4ojBxVdZTlXFtxAhDGIIhmPh/iCpy0IQq\n"
        "WCdjDtAs1btArZRq0jskPbZ+spbl+5pJzuvvdars9clVOoMFB+NDw/tT+DXYl2Mi\n"
        "BwIDAQAB\n"
        "-----END PUBLIC KEY-----\n";
    const std::string signatureBase64 =
        "EnWYXHhiANWHABZS1i9iyl1QRzSr65UHeuCb9DDTWsNsdwXjlm7fytJ+N96kzENYBCWpu0sVKtDnXHBVDrMByT1Jix7GVe2AAEjAiWdkerA+6PE6ZebfoLWgeOmuqRdVSd5nmE1PO/+/0Xc2S4dsO3PaMXNPm91Frc5Oegj+m28HN+nQBmNI0DVszN5lbyPN9r4oFoam8nU/U0faOtZEoXAfRTJh7CnexfCcYtuFswXRUFhT2u6ijZGDXl73LI4wS74fI8tau6ieF9NT9WHGQjOxH9uUL9xWiAiK7PW8ocDFjGrq/Y78IHOe2sRFygX2CMWF5GdvIRWDWYxBk9hTXQ==";

    auto verifier = autoupdater::createDefaultSignatureVerifier();
    auto ok = verifier->verify(payload, signatureBase64, publicKeyPem);
    LAU_REQUIRE(ok);

    auto badPayload = verifier->verify(payload + "-tampered", signatureBase64, publicKeyPem);
    LAU_REQUIRE(!badPayload);
    LAU_REQUIRE(badPayload.error().code == autoupdater::ErrorCode::ManifestSignatureInvalid);
#else
    // OpenSSL is optional; this test is compiled on all builds and becomes
    // active when the default OpenSSL verifier is part of the target.
    LAU_REQUIRE(true);
#endif
}

