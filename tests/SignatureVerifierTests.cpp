#include "TestCommon.h"

#include "libAutoUpdater/interfaces/ISignatureVerifier.h"

void testOpenSslSignatureVerifier() {
#ifdef LIBAUTOUPDATER_TEST_HAS_OPENSSL
    const std::string payload = "schemaVersion=1;version=1.2.3";
    const std::string rsa2048PublicKeyPem = "-----BEGIN PUBLIC KEY-----\n"
                                            "MIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEAl/VSV4RpEZxz+fnJPoG/\n"
                                            "keTuxgdzfubNMGWg4AXEEUSvAXp9mBsknCvO6KqK42YjMqiSpeDnHMtzGi7P7Rcd\n"
                                            "7/VP/epbLK7qtDAXdYTmvKG9sgQHcn5bzNMUGxyR+5BMg4mZmcACmAIk8llMP9D2\n"
                                            "+5gs7ycGurWD1yDfM5qjSIZbMhth2E2OzeITD62F53H35qrnrg2QKDx1ZJkwHf4E\n"
                                            "aJ7Cg6MCBalYougtT8SUcZrPxCfC7zG4ojBxVdZTlXFtxAhDGIIhmPh/iCpy0IQq\n"
                                            "WCdjDtAs1btArZRq0jskPbZ+spbl+5pJzuvvdars9clVOoMFB+NDw/tT+DXYl2Mi\n"
                                            "BwIDAQAB\n"
                                            "-----END PUBLIC KEY-----\n";
    const std::string rsa2048SignatureBase64 =
        "EnWYXHhiANWHABZS1i9iyl1QRzSr65UHeuCb9DDTWsNsdwXjlm7fytJ+N96kzENYBCWpu0sVKtDnXHBVDrMByT1Jix7GVe2AAEjAiWdkerA+"
        "6PE6ZebfoLWgeOmuqRdVSd5nmE1PO/+/0Xc2S4dsO3PaMXNPm91Frc5Oegj+m28HN+nQBmNI0DVszN5lbyPN9r4oFoam8nU/"
        "U0faOtZEoXAfRTJh7CnexfCcYtuFswXRUFhT2u6ijZGDXl73LI4wS74fI8tau6ieF9NT9WHGQjOxH9uUL9xWiAiK7PW8ocDFjGrq/"
        "Y78IHOe2sRFygX2CMWF5GdvIRWDWYxBk9hTXQ==";

    const std::string rsa1024PublicKeyPem = "-----BEGIN PUBLIC KEY-----\n"
                                            "MIGfMA0GCSqGSIb3DQEBAQUAA4GNADCBiQKBgQDu9jDkjqKLauqeiC1M20YZI0uj\n"
                                            "f6TypOvE19z+YJjld0B2nbzO6igyxxqFGNrQl1YgJV6JOT692XAh9sx1nlFMPv58\n"
                                            "ZeVEiQrZZjjWaDo/0mlBpACf791gTuJ2TajJXM+RVM5Xvqqwy2VTHjrM0Cfgh5Gy\n"
                                            "jY3Ytv4krA1KeaVqQwIDAQAB\n"
                                            "-----END PUBLIC KEY-----\n";
    const std::string rsa1024SignatureBase64 =
        "ZFJIUSJsk4WTIk/mZqhb4fUIGnWF7B2zd7dO/mnDA+kejWc07eDvfDYz/b83RegdK3YFWrx1iQt1aAB+9GT2uYrrYN+knpV/"
        "Kch0Wa24LZGH9bog/ndnxWgOiHCP28JppUfb5nNl51Jaj0xtF14mW+OgTJNrx/fe1Mlv7E4agS0=";

    const std::string ecPublicKeyPem = "-----BEGIN PUBLIC KEY-----\n"
                                       "MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAEj8OzQPwnRIORIbkklPDkMpA/oukI\n"
                                       "CtE9Cg0zPZaklxy6hRHGHllFtPoCSD9xjQbjoN2Kke2Z/UH5iLHdSD6Q9w==\n"
                                       "-----END PUBLIC KEY-----\n";
    const std::string ecSignatureBase64 =
        "MEYCIQChZCvChhMfTw++r1VBWLUICqHoRQpZMs9HvH5F1tkAoAIhAPkF6fkHNoN5Dkl9RYh0MJ/fdQpJviIVfRVenOJtfSjf";

    const std::string rsaPssPublicKeyPem = "-----BEGIN PUBLIC KEY-----\n"
                                           "MIIBIDALBgkqhkiG9w0BAQoDggEPADCCAQoCggEBAKbr55Yc18v8APkN4U+zedq1\n"
                                           "uLrOPv3ddT+ZZsrAg2Pet9UWT7KRH2fvjfsS5iPWVSATbKYSsVQyIEDciUO3VTCX\n"
                                           "/puBWVAjnGIzDzTby3dr0OmkRdtYRMGoGDwRq7MXViLJP1/vUEE/zLu/n5mO2UCo\n"
                                           "Reputy1eTQQhDlRbINyWYBY1z3lYRnvJ253mLdK+PSuus3eVHnNkJq8cQcu/lFnv\n"
                                           "kmrwYqXl3HtSDJ4abCKbfY947BxwAKnhu+FwDNWElZLfcINSw/7Ik3vu8KtP7Mry\n"
                                           "aQcVViVHIwl7sLM7uVGYnD38v3RF+jkds0s11+2F/7JeQBeNQgJPzvXfamzpaKEC\n"
                                           "AwEAAQ==\n"
                                           "-----END PUBLIC KEY-----\n";
    const std::string rsaPssSignatureBase64 =
        "EFl7v05az93tgDAmzuIsUUMNTE79b8MSZvCDqcJ6tK3EySlKcsljEa7gCAtLpvw93cDNFAEoJ3JPdnrztvre2KQAS2P7EXlvH2qVJz1/"
        "SANL1wUA8KSIWLIFpvsn9pQg9WP1aRKyi0se/LlbQ/mttEfQfNYFoJzx3A3F7MR7H40dbSy8fPI07eS4aO5rYFILbeGlNmDP3Xi5LxAWIy"
        "Ux8cJj54pGPe2TXQC9YMdLYGRyMcJa0+KVe+4uKvkGGWRz++2q6L+XOgBAF7yzhbzs54Rv3DVGSH1/eFqVTPLZzH37CTXImTG6tmUgGDYj"
        "Ojp+dMUZ4ZYyfs1J6rwDZP9EIQ==";

    const std::string ed25519PublicKeyPem = "-----BEGIN PUBLIC KEY-----\n"
                                            "MCowBQYDK2VwAyEA6PncMTonWYLA2fM1niXCEw063OUZVC08VLOiJslNj/c=\n"
                                            "-----END PUBLIC KEY-----\n";
    const std::string ed25519SignatureBase64 =
        "YAN+3vXsdG/80dsiqfJ+4th2ZDhExsMedmecPP2vQDqtt1ktYAxMQq9cOcRFQajhJaysOBSekPMWxk2iCGoDAQ==";

    auto verifier = autoupdater::createDefaultSignatureVerifier();
    auto rsa2048 = verifier->verify(payload, rsa2048SignatureBase64, rsa2048PublicKeyPem);
    LAU_REQUIRE(rsa2048);

    auto rsa2048Tampered = verifier->verify(payload + "-tampered", rsa2048SignatureBase64, rsa2048PublicKeyPem);
    LAU_REQUIRE(!rsa2048Tampered);
    LAU_REQUIRE(rsa2048Tampered.error().code == autoupdater::ErrorCode::ManifestSignatureInvalid);

    auto rsa1024 = verifier->verify(payload, rsa1024SignatureBase64, rsa1024PublicKeyPem);
    LAU_REQUIRE(!rsa1024);
    LAU_REQUIRE(rsa1024.error().code == autoupdater::ErrorCode::ManifestSignatureInvalid);

    auto ec = verifier->verify(payload, ecSignatureBase64, ecPublicKeyPem);
    LAU_REQUIRE(!ec);
    LAU_REQUIRE(ec.error().code == autoupdater::ErrorCode::ManifestSignatureInvalid);

    auto rsaPss = verifier->verify(payload, rsaPssSignatureBase64, rsaPssPublicKeyPem);
    LAU_REQUIRE(!rsaPss);
    LAU_REQUIRE(rsaPss.error().code == autoupdater::ErrorCode::ManifestSignatureInvalid);

    auto ed25519 = verifier->verify(payload, ed25519SignatureBase64, ed25519PublicKeyPem);
    LAU_REQUIRE(ed25519);

    auto ed25519Tampered = verifier->verify(payload + "-tampered", ed25519SignatureBase64, ed25519PublicKeyPem);
    LAU_REQUIRE(!ed25519Tampered);
    LAU_REQUIRE(ed25519Tampered.error().code == autoupdater::ErrorCode::ManifestSignatureInvalid);

    for (const auto& malformedBase64 : {std::string("AAAAA"), std::string("AAAA!AAA"), std::string("AA=A")}) {
        auto malformed = verifier->verify(payload, malformedBase64, rsa2048PublicKeyPem);
        LAU_REQUIRE(!malformed);
        LAU_REQUIRE(malformed.error().code == autoupdater::ErrorCode::ManifestSignatureInvalid);
    }

    for (const auto& invalidPublicKey : {std::string{}, std::string("not a PEM public key")}) {
        auto invalidKey = verifier->verify(payload, rsa2048SignatureBase64, invalidPublicKey);
        LAU_REQUIRE(!invalidKey);
        LAU_REQUIRE(invalidKey.error().code == autoupdater::ErrorCode::ManifestSignatureInvalid);
    }
#else
    LAU_SKIP("OpenSSL signature verifier is not part of this build");
#endif
}
