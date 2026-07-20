#include "TestCommon.h"

#include "libAutoUpdater/Error.h"

#include <array>
#include <string>
#include <utility>

void testStructuredDiagnosticsAreStableAndNonSensitive() {
    const autoupdater::Error legacyAggregate{autoupdater::ErrorCode::DownloadFailed, "legacy diagnostic"};
    LAU_REQUIRE(legacyAggregate.phase == autoupdater::ErrorPhase::General);

    const std::array<std::pair<autoupdater::ErrorPhase, std::string>, 6> phases = {{
        {autoupdater::ErrorPhase::General, "General"},
        {autoupdater::ErrorPhase::Apply, "Apply"},
        {autoupdater::ErrorPhase::Rollback, "Rollback"},
        {autoupdater::ErrorPhase::Recovery, "Recovery"},
        {autoupdater::ErrorPhase::StatePersistence, "StatePersistence"},
        {autoupdater::ErrorPhase::Restart, "Restart"},
    }};
    for (const auto& [phase, expected] : phases) {
        LAU_REQUIRE(std::string(autoupdater::toString(phase)) == expected);
    }

    const std::string credential = "TOP_SECRET_CREDENTIAL";
    const std::string signature = "BASE64_SIGNATURE_MATERIAL";
    const std::string privateMaterial = "PRIVATE_KEY_MATERIAL";
    const autoupdater::Error sensitive{
        autoupdater::ErrorCode::ApplyLaunchFailed,
        "request https://user:password@example.test/release?token=" + credential +
            "&X-Amz-Signature=" + signature + " failed; signature=" + signature +
            "; -----BEGIN PRIVATE KEY----- " + privateMaterial + " -----END PRIVATE KEY-----",
        autoupdater::ErrorPhase::Restart,
    };

    const auto diagnostic = autoupdater::formatDiagnostic(sensitive);
    LAU_REQUIRE(diagnostic == "phase=Restart code=ApplyLaunchFailed");
    for (const auto* secret : {"user", "password", "token", "X-Amz-Signature", credential.c_str(),
                               signature.c_str(), "PRIVATE KEY", privateMaterial.c_str()}) {
        LAU_REQUIRE(diagnostic.find(secret) == std::string::npos);
    }
}
