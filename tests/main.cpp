#include "TestCommon.h"

#include <vector>

void testVersionParsingAndOrdering();
void testManifestParsing();
void testManifestRejectsPathTraversal();
void testManifestRejectsUnsafePaths();
void testManifestRejectsLocalPathTraversal();
void testManifestRejectsInvalidSha256();
void testManifestRejectsInvalidSize();
void testManifestRejectsUnsafeReleaseId();
void testJsonParsesUnicodeEscapes();
void testJsonRejectsInvalidUnicodeEscapes();
void testJsonRejectsUnescapedControlCharacters();
void testJsonRejectsDeepNesting();
void testJsonNumbersRejectOutOfRangeConversions();
void testJsonStrictWhitespace();
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
void testApplyExecutorReclaimsStaleLock();
void testApplyExecutorRejectsDuplicateTargets();
void testApplyExecutorRecoversInterruptedTransaction();
void testDownloadExecutorSucceedsAndStagesFile();
void testDownloadExecutorRetriesAndReportsAttempts();
void testDownloadExecutorHashMismatchCleansUpAndFails();
void testDownloadExecutorCancelStopsDownload();
void testDownloadExecutorResumesPartialDownload();
void testDownloadExecutorRejectsNegativeRetryCount();
void testSha256Provider();
void testOpenSslSignatureVerifier();
void testStateStoreDownloadResume();
void testStateStoreRecoversFromCorruptFile();
void testUpdaterQueuedCallbacksOutliveUpdater();
void testUpdaterCanSuppressIntermediateCheckResultBeforeDownload();
void testUpdaterStillReportsTerminalCheckResultWhenSuppressed();
void testDownloadAsyncPersistsPendingUpdate();
void testFuzzSmokeParsersAndPaths();

int main() {
    const std::vector<TestCase> tests = {
        {"VersionParsingAndOrdering", testVersionParsingAndOrdering},
        {"ManifestParsing", testManifestParsing},
        {"ManifestRejectsPathTraversal", testManifestRejectsPathTraversal},
        {"ManifestRejectsUnsafePaths", testManifestRejectsUnsafePaths},
        {"ManifestRejectsLocalPathTraversal", testManifestRejectsLocalPathTraversal},
        {"ManifestRejectsInvalidSha256", testManifestRejectsInvalidSha256},
        {"ManifestRejectsInvalidSize", testManifestRejectsInvalidSize},
        {"ManifestRejectsUnsafeReleaseId", testManifestRejectsUnsafeReleaseId},
        {"JsonParsesUnicodeEscapes", testJsonParsesUnicodeEscapes},
        {"JsonRejectsInvalidUnicodeEscapes", testJsonRejectsInvalidUnicodeEscapes},
        {"JsonRejectsUnescapedControlCharacters", testJsonRejectsUnescapedControlCharacters},
        {"JsonRejectsDeepNesting", testJsonRejectsDeepNesting},
        {"JsonNumbersRejectOutOfRangeConversions", testJsonNumbersRejectOutOfRangeConversions},
        {"JsonStrictWhitespace", testJsonStrictWhitespace},
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
        {"ApplyExecutorReclaimsStaleLock", testApplyExecutorReclaimsStaleLock},
        {"ApplyExecutorRejectsDuplicateTargets", testApplyExecutorRejectsDuplicateTargets},
        {"ApplyExecutorRecoversInterruptedTransaction", testApplyExecutorRecoversInterruptedTransaction},
        {"DownloadExecutorSucceedsAndStagesFile", testDownloadExecutorSucceedsAndStagesFile},
        {"DownloadExecutorRetriesAndReportsAttempts", testDownloadExecutorRetriesAndReportsAttempts},
        {"DownloadExecutorHashMismatchCleansUpAndFails", testDownloadExecutorHashMismatchCleansUpAndFails},
        {"DownloadExecutorCancelStopsDownload", testDownloadExecutorCancelStopsDownload},
        {"DownloadExecutorResumesPartialDownload", testDownloadExecutorResumesPartialDownload},
        {"DownloadExecutorRejectsNegativeRetryCount", testDownloadExecutorRejectsNegativeRetryCount},
        {"Sha256Provider", testSha256Provider},
        {"OpenSslSignatureVerifier", testOpenSslSignatureVerifier},
        {"StateStoreDownloadResume", testStateStoreDownloadResume},
        {"StateStoreRecoversFromCorruptFile", testStateStoreRecoversFromCorruptFile},
        {"UpdaterQueuedCallbacksOutliveUpdater", testUpdaterQueuedCallbacksOutliveUpdater},
        {"UpdaterCanSuppressIntermediateCheckResultBeforeDownload",
         testUpdaterCanSuppressIntermediateCheckResultBeforeDownload},
        {"UpdaterStillReportsTerminalCheckResultWhenSuppressed",
         testUpdaterStillReportsTerminalCheckResultWhenSuppressed},
        {"DownloadAsyncPersistsPendingUpdate", testDownloadAsyncPersistsPendingUpdate},
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
