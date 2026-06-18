#include "TestCommon.h"

#include <vector>

void testVersionParsingAndOrdering();
void testManifestParsing();
void testManifestRejectsPathTraversal();
void testManifestFetcherRoutesIndexManifest();
void testManifestFetcherRejectsDisallowedIndexTarget();
void testManifestFetcherRejectsAllowedBaseUrlPrefixBypass();
void testUpdatePlannerCreatesOperations();
void testApplyPlanRoundTrip();
void testApplyPlanRoundTripPreservesUnicodePaths();
void testApplyLauncherUsesStagedUpdaterCopy();
void testApplyLauncherRejectsMissingUpdaterExecutable();
void testApplyExecutorRollsBackCurrentFailedOperation();
void testApplyExecutorRejectsExistingLock();
void testApplyExecutorReplacesFilesInUnicodeDirectory();
void testSha256Provider();
void testOpenSslSignatureVerifier();
void testStateStoreDownloadResume();
void testUpdaterQueuedCallbacksOutliveUpdater();
void testUpdaterCanSuppressIntermediateCheckResultBeforeDownload();
void testUpdaterStillReportsTerminalCheckResultWhenSuppressed();
void testFuzzSmokeParsersAndPaths();

int main() {
    const std::vector<TestCase> tests = {
        {"VersionParsingAndOrdering", testVersionParsingAndOrdering},
        {"ManifestParsing", testManifestParsing},
        {"ManifestRejectsPathTraversal", testManifestRejectsPathTraversal},
        {"ManifestFetcherRoutesIndexManifest", testManifestFetcherRoutesIndexManifest},
        {"ManifestFetcherRejectsDisallowedIndexTarget", testManifestFetcherRejectsDisallowedIndexTarget},
        {"ManifestFetcherRejectsAllowedBaseUrlPrefixBypass", testManifestFetcherRejectsAllowedBaseUrlPrefixBypass},
        {"UpdatePlannerCreatesOperations", testUpdatePlannerCreatesOperations},
        {"ApplyPlanRoundTrip", testApplyPlanRoundTrip},
        {"ApplyPlanRoundTripPreservesUnicodePaths", testApplyPlanRoundTripPreservesUnicodePaths},
        {"ApplyLauncherUsesStagedUpdaterCopy", testApplyLauncherUsesStagedUpdaterCopy},
        {"ApplyLauncherRejectsMissingUpdaterExecutable", testApplyLauncherRejectsMissingUpdaterExecutable},
        {"ApplyExecutorRollsBackCurrentFailedOperation", testApplyExecutorRollsBackCurrentFailedOperation},
        {"ApplyExecutorRejectsExistingLock", testApplyExecutorRejectsExistingLock},
        {"ApplyExecutorReplacesFilesInUnicodeDirectory", testApplyExecutorReplacesFilesInUnicodeDirectory},
        {"Sha256Provider", testSha256Provider},
        {"OpenSslSignatureVerifier", testOpenSslSignatureVerifier},
        {"StateStoreDownloadResume", testStateStoreDownloadResume},
        {"UpdaterQueuedCallbacksOutliveUpdater", testUpdaterQueuedCallbacksOutliveUpdater},
        {"UpdaterCanSuppressIntermediateCheckResultBeforeDownload",
         testUpdaterCanSuppressIntermediateCheckResultBeforeDownload},
        {"UpdaterStillReportsTerminalCheckResultWhenSuppressed",
         testUpdaterStillReportsTerminalCheckResultWhenSuppressed},
        {"FuzzSmokeParsersAndPaths", testFuzzSmokeParsersAndPaths},
    };

    int failed = 0;
    for (const auto& test : tests) {
        try {
            test.fn();
            std::cout << "[PASS] " << test.name << "\n";
        } catch (const std::exception& ex) {
            ++failed;
            std::cerr << "[FAIL] " << test.name << ": " << ex.what() << "\n";
        }
    }
    return failed == 0 ? 0 : 1;
}
