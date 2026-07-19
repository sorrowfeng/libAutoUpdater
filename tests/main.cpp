#include "TestCommon.h"

#include <vector>

void testVersionParsingAndOrdering();
void testManifestParsing();
void testManifestRejectsPathTraversal();
void testManifestFetcherRoutesIndexManifest();
void testManifestFetcherRejectsDisallowedIndexTarget();
void testManifestFetcherRejectsAllowedBaseUrlPrefixBypass();
void testManifestFetcherRejectsInitialUrlBeforeNetwork();
void testManifestFetcherResolvesIndexTargetFromEffectiveUrl();
void testManifestFetcherResolvesSignaturesFromEffectiveUrl();
void testManifestFetcherResolvesEmptyAndRelativeArtifactBases();
void testUrlPolicyFailsClosedAndRejectsMalformedUrls();
void testUrlPolicyCanonicalizesAndEnforcesScopeBoundaries();
void testUrlPolicyRejectsLocalAndAmbiguousAddressLiterals();
void testNetworkRequestRejectsInitialUrlBeforeTransport();
void testNetworkRequestFollowsAllAllowedRedirectStatuses();
void testNetworkRequestEnforcesRedirectOriginsAndProtocols();
void testNetworkRequestRequiresOneLocationHeader();
void testNetworkRequestDetectsLoopsAndRedirectLimit();
void testNetworkRequestRejectsForgedEffectiveUrl();
void testNetworkDownloadRestoresBodiesWrittenByRedirects();
void testNetworkDownloadResetsResumeAndRejectsUnexpectedSuccessStatus();
void testNetworkDownloadRestartsIgnoredHttpResume();
void testNetworkDownloadPreservesLocalFileResume();
void testNetworkResourceLimits();
void testJsonResourceLimits();
void testManifestAndSignatureResourceLimits();
void testUpdatePlannerCreatesOperations();
void testUpdatePlannerPercentEncodesArtifactPaths();
void testApplyPlanRoundTrip();
void testApplyPlanRoundTripPreservesUnicodePaths();
void testApplyExecutorUsesSafeAtomicJournalName();
void testApplyExecutorRequiresWritableJournal();
void testApplyExecutorRollsBackCurrentFailedOperation();
void testApplyExecutorRejectsExistingLock();
void testApplyExecutorReplacesFilesInUnicodeDirectory();
void testApplyExecutorRecoversAfterForcedTermination();
void testApplyExecutorRejectsSourceTargetBackupLinks();
void testRootedFileSystemPinsHandlesAndRejectsSwaps();
void testLocalSnapshotUsesOneOpenedFileHandle();
void testDownloadExecutorContainsSwapsAndHardLinks();
void testDownloadExecutorKeepsValidatorsBoundToTheirResource();
void testApplyExecutorUsesSafePosixPermissions();
void testSha256Provider();
void testOpenSslSignatureVerifier();
void testStateStoreDownloadResume();
void testUpdaterQueuedCallbacksOutliveUpdater();
void testFuzzSmokeParsersAndPaths();

int main() {
    const std::vector<TestCase> tests = {
        {"VersionParsingAndOrdering", testVersionParsingAndOrdering},
        {"ManifestParsing", testManifestParsing},
        {"ManifestRejectsPathTraversal", testManifestRejectsPathTraversal},
        {"ManifestFetcherRoutesIndexManifest", testManifestFetcherRoutesIndexManifest},
        {"ManifestFetcherRejectsDisallowedIndexTarget", testManifestFetcherRejectsDisallowedIndexTarget},
        {"ManifestFetcherRejectsAllowedBaseUrlPrefixBypass", testManifestFetcherRejectsAllowedBaseUrlPrefixBypass},
        {"ManifestFetcherRejectsInitialUrlBeforeNetwork", testManifestFetcherRejectsInitialUrlBeforeNetwork},
        {"ManifestFetcherResolvesIndexTargetFromEffectiveUrl", testManifestFetcherResolvesIndexTargetFromEffectiveUrl},
        {"ManifestFetcherResolvesSignaturesFromEffectiveUrl", testManifestFetcherResolvesSignaturesFromEffectiveUrl},
        {"ManifestFetcherResolvesEmptyAndRelativeArtifactBases",
         testManifestFetcherResolvesEmptyAndRelativeArtifactBases},
        {"UrlPolicyFailsClosedAndRejectsMalformedUrls", testUrlPolicyFailsClosedAndRejectsMalformedUrls},
        {"UrlPolicyCanonicalizesAndEnforcesScopeBoundaries", testUrlPolicyCanonicalizesAndEnforcesScopeBoundaries},
        {"UrlPolicyRejectsLocalAndAmbiguousAddressLiterals", testUrlPolicyRejectsLocalAndAmbiguousAddressLiterals},
        {"NetworkRequestRejectsInitialUrlBeforeTransport", testNetworkRequestRejectsInitialUrlBeforeTransport},
        {"NetworkRequestFollowsAllAllowedRedirectStatuses", testNetworkRequestFollowsAllAllowedRedirectStatuses},
        {"NetworkRequestEnforcesRedirectOriginsAndProtocols", testNetworkRequestEnforcesRedirectOriginsAndProtocols},
        {"NetworkRequestRequiresOneLocationHeader", testNetworkRequestRequiresOneLocationHeader},
        {"NetworkRequestDetectsLoopsAndRedirectLimit", testNetworkRequestDetectsLoopsAndRedirectLimit},
        {"NetworkRequestRejectsForgedEffectiveUrl", testNetworkRequestRejectsForgedEffectiveUrl},
        {"NetworkDownloadRestoresBodiesWrittenByRedirects", testNetworkDownloadRestoresBodiesWrittenByRedirects},
        {"NetworkDownloadResetsResumeAndRejectsUnexpectedSuccessStatus",
         testNetworkDownloadResetsResumeAndRejectsUnexpectedSuccessStatus},
        {"NetworkDownloadRestartsIgnoredHttpResume", testNetworkDownloadRestartsIgnoredHttpResume},
        {"NetworkDownloadPreservesLocalFileResume", testNetworkDownloadPreservesLocalFileResume},
        {"NetworkResourceLimits", testNetworkResourceLimits},
        {"JsonResourceLimits", testJsonResourceLimits},
        {"ManifestAndSignatureResourceLimits", testManifestAndSignatureResourceLimits},
        {"UpdatePlannerCreatesOperations", testUpdatePlannerCreatesOperations},
        {"UpdatePlannerPercentEncodesArtifactPaths", testUpdatePlannerPercentEncodesArtifactPaths},
        {"ApplyPlanRoundTrip", testApplyPlanRoundTrip},
        {"ApplyPlanRoundTripPreservesUnicodePaths", testApplyPlanRoundTripPreservesUnicodePaths},
        {"ApplyExecutorUsesSafeAtomicJournalName", testApplyExecutorUsesSafeAtomicJournalName},
        {"ApplyExecutorRequiresWritableJournal", testApplyExecutorRequiresWritableJournal},
        {"ApplyExecutorRollsBackCurrentFailedOperation", testApplyExecutorRollsBackCurrentFailedOperation},
        {"ApplyExecutorRejectsExistingLock", testApplyExecutorRejectsExistingLock},
        {"ApplyExecutorReplacesFilesInUnicodeDirectory", testApplyExecutorReplacesFilesInUnicodeDirectory},
        {"ApplyExecutorRecoversAfterForcedTermination", testApplyExecutorRecoversAfterForcedTermination},
        {"ApplyExecutorRejectsSourceTargetBackupLinks", testApplyExecutorRejectsSourceTargetBackupLinks},
        {"RootedFileSystemPinsHandlesAndRejectsSwaps", testRootedFileSystemPinsHandlesAndRejectsSwaps},
        {"LocalSnapshotUsesOneOpenedFileHandle", testLocalSnapshotUsesOneOpenedFileHandle},
        {"DownloadExecutorContainsSwapsAndHardLinks", testDownloadExecutorContainsSwapsAndHardLinks},
        {"DownloadExecutorKeepsValidatorsBoundToTheirResource",
         testDownloadExecutorKeepsValidatorsBoundToTheirResource},
        {"ApplyExecutorUsesSafePosixPermissions", testApplyExecutorUsesSafePosixPermissions},
        {"Sha256Provider", testSha256Provider},
        {"OpenSslSignatureVerifier", testOpenSslSignatureVerifier},
        {"StateStoreDownloadResume", testStateStoreDownloadResume},
        {"UpdaterQueuedCallbacksOutliveUpdater", testUpdaterQueuedCallbacksOutliveUpdater},
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
