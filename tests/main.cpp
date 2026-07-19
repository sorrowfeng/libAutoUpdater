#include "TestCommon.h"

#include <string>
#include <vector>

void testVersionParsingAndOrdering();
void testManifestParsing();
void testManifestRejectsPathTraversal();
void testManifestRejectsConflictingManagedTargets();
void testManifestRejectsReservedUpdaterTargetsFromEveryTargetForm();
void testManifestAllowsSharedSourceForDistinctManagedTargets();
void testManifestAllowsNonConflictingTargetPrefixesAndSiblings();
void testManifestFetcherRoutesIndexManifest();
void testManifestFetcherRejectsInvalidReleaseBehindSignedIndex();
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
void testUpdatePlannerRequiresVerifiedLocalDowngradeAuthorization();
void testUpdatePlannerUsesHighestDowngradeBaseline();
void testUpdatePlannerRejectsUnauthorizedDowngradeBeforeReinstallDecision();
void testUpdatePlannerPreservesNormalUpgradeAndSameVersionSemantics();
void testUpdatePlannerRejectsProgrammaticManagedTargetConflictsEarly();
void testUpdatePlannerAllowsSharedSourceForDistinctManagedTargets();
void testApplyPlanRoundTrip();
void testApplyPlanRoundTripPreservesUnicodePaths();
void testApplyPlanRollbackContract();
void testApplyPlanRejectsConflictingManagedTargets();
void testApplyPlanAllowsSharedSourceForDistinctManagedTargets();
void testApplyExecutorUsesSafeAtomicJournalName();
void testApplyExecutorRequiresWritableJournal();
void testApplyExecutorRollsBackCurrentFailedOperation();
void testApplyExecutorRejectsExistingLock();
void testApplyExecutorReplacesFilesInUnicodeDirectory();
void testApplyExecutorExecutesOperationFreePublicRollback();
void testApplyExecutorRecoversFailedPublicRollbackOfRollback();
void testApplyExecutorRecoversAfterForcedTermination();
void testApplyExecutorRejectsSourceTargetBackupLinks();
void testApplyExecutorRejectsProgrammaticManagedTargetConflictsEarly();
void testApplyExecutorValidatesProcessWaitInputs();
void testRootedFileSystemPinsHandlesAndRejectsSwaps();
void testLocalSnapshotUsesOneOpenedFileHandle();
void testDownloadExecutorContainsSwapsAndHardLinks();
void testDownloadExecutorKeepsValidatorsBoundToTheirResource();
void testApplyExecutorUsesSafePosixPermissions();
void testProcessLauncherReportsSetupAndExecFailures();
void testProcessLauncherPreservesArgumentsAndWorkingDirectory();
void testProcessLauncherRejectsLossyArguments();
void testSha256Provider();
void testOpenSslSignatureVerifier();
void testStateStoreDownloadResume();
void testStateStoreDistinguishesMissingAndCorruptState();
void testStateStorePreservesLastKnownGoodSnapshot();
void testStateStoreConcurrentInstancesDoNotLoseUpdates();
void testStateStoreHealthyCommitUsesCompareAndSet();
void testStateStoreWriteFailuresPreservePrimary();
void testStateStoreCrossProcessLockingAndCrashRecovery();
int runStateStoreHelper(int argc, char* argv[]);
void testUpdaterQueuedCallbacksOutliveUpdater();
void testUpdaterDirectCallbacksAreExceptionSafeAndReentrant();
void testUpdaterOverlappingChecksAreNonBlockingAndCancellationIsolated();
void testUpdaterCanBeDestroyedFromDirectCallback();
void testUpdaterNewGenerationInvalidatesReadyPlan();
void testUpdaterQueuedDownloadKeepsRequestedGeneration();
void testUpdaterRequiresPersistedPendingBeforeReady();
void testUpdaterApplyRequiresMatchingPersistedPending();
void testUpdaterHealthyMarkPreservesFuturePending();
void testUpdaterPeriodicCheckPreservesReadyGeneration();
void testUpdaterQueueOverflowErrorReentryIsBounded();
void testUpdaterQueuedDispatcherSuppressesStaleGenerationAfterDestruction();
void testUpdaterHealthyMarkRequiresMatchingTerminalReceipt();
void testApplyLauncherBoundsProcessWaitTimeout();
void testUpdaterDelegatesRollbackToTerminalBoundExternalPlan();
void testUpdaterFailsClosedWhenAcceptedStateIsUnreadable();
void testFuzzSmokeParsersAndPaths();

int main(int argc, char* argv[]) {
    if (argc >= 2) {
        const std::string command = argv[1];
        if (command == "--state-store-helper-write" || command == "--state-store-helper-crash-backup" ||
            command == "--state-store-helper-crash-primary") {
            return runStateStoreHelper(argc, argv);
        }
    }

    const std::vector<TestCase> tests = {
        {"VersionParsingAndOrdering", testVersionParsingAndOrdering},
        {"ManifestParsing", testManifestParsing},
        {"ManifestRejectsPathTraversal", testManifestRejectsPathTraversal},
        {"ManifestRejectsConflictingManagedTargets", testManifestRejectsConflictingManagedTargets},
        {"ManifestRejectsReservedUpdaterTargetsFromEveryTargetForm",
         testManifestRejectsReservedUpdaterTargetsFromEveryTargetForm},
        {"ManifestAllowsSharedSourceForDistinctManagedTargets",
         testManifestAllowsSharedSourceForDistinctManagedTargets},
        {"ManifestAllowsNonConflictingTargetPrefixesAndSiblings",
         testManifestAllowsNonConflictingTargetPrefixesAndSiblings},
        {"ManifestFetcherRoutesIndexManifest", testManifestFetcherRoutesIndexManifest},
        {"ManifestFetcherRejectsInvalidReleaseBehindSignedIndex",
         testManifestFetcherRejectsInvalidReleaseBehindSignedIndex},
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
        {"UpdatePlannerRequiresVerifiedLocalDowngradeAuthorization",
         testUpdatePlannerRequiresVerifiedLocalDowngradeAuthorization},
        {"UpdatePlannerUsesHighestDowngradeBaseline", testUpdatePlannerUsesHighestDowngradeBaseline},
        {"UpdatePlannerRejectsUnauthorizedDowngradeBeforeReinstallDecision",
         testUpdatePlannerRejectsUnauthorizedDowngradeBeforeReinstallDecision},
        {"UpdatePlannerPreservesNormalUpgradeAndSameVersionSemantics",
         testUpdatePlannerPreservesNormalUpgradeAndSameVersionSemantics},
        {"UpdatePlannerRejectsProgrammaticManagedTargetConflictsEarly",
         testUpdatePlannerRejectsProgrammaticManagedTargetConflictsEarly},
        {"UpdatePlannerAllowsSharedSourceForDistinctManagedTargets",
         testUpdatePlannerAllowsSharedSourceForDistinctManagedTargets},
        {"ApplyPlanRoundTrip", testApplyPlanRoundTrip},
        {"ApplyPlanRoundTripPreservesUnicodePaths", testApplyPlanRoundTripPreservesUnicodePaths},
        {"ApplyPlanRollbackContract", testApplyPlanRollbackContract},
        {"ApplyPlanRejectsConflictingManagedTargets", testApplyPlanRejectsConflictingManagedTargets},
        {"ApplyPlanAllowsSharedSourceForDistinctManagedTargets",
         testApplyPlanAllowsSharedSourceForDistinctManagedTargets},
        {"ApplyExecutorUsesSafeAtomicJournalName", testApplyExecutorUsesSafeAtomicJournalName},
        {"ApplyExecutorRequiresWritableJournal", testApplyExecutorRequiresWritableJournal},
        {"ApplyExecutorRollsBackCurrentFailedOperation", testApplyExecutorRollsBackCurrentFailedOperation},
        {"ApplyExecutorRejectsExistingLock", testApplyExecutorRejectsExistingLock},
        {"ApplyExecutorReplacesFilesInUnicodeDirectory", testApplyExecutorReplacesFilesInUnicodeDirectory},
        {"ApplyExecutorExecutesOperationFreePublicRollback", testApplyExecutorExecutesOperationFreePublicRollback},
        {"ApplyExecutorRecoversFailedPublicRollbackOfRollback",
         testApplyExecutorRecoversFailedPublicRollbackOfRollback},
        {"ApplyExecutorRecoversAfterForcedTermination", testApplyExecutorRecoversAfterForcedTermination},
        {"ApplyExecutorRejectsSourceTargetBackupLinks", testApplyExecutorRejectsSourceTargetBackupLinks},
        {"ApplyExecutorRejectsProgrammaticManagedTargetConflictsEarly",
         testApplyExecutorRejectsProgrammaticManagedTargetConflictsEarly},
        {"ApplyExecutorValidatesProcessWaitInputs", testApplyExecutorValidatesProcessWaitInputs},
        {"RootedFileSystemPinsHandlesAndRejectsSwaps", testRootedFileSystemPinsHandlesAndRejectsSwaps},
        {"LocalSnapshotUsesOneOpenedFileHandle", testLocalSnapshotUsesOneOpenedFileHandle},
        {"DownloadExecutorContainsSwapsAndHardLinks", testDownloadExecutorContainsSwapsAndHardLinks},
        {"DownloadExecutorKeepsValidatorsBoundToTheirResource",
         testDownloadExecutorKeepsValidatorsBoundToTheirResource},
        {"ApplyExecutorUsesSafePosixPermissions", testApplyExecutorUsesSafePosixPermissions},
        {"ProcessLauncherReportsSetupAndExecFailures", testProcessLauncherReportsSetupAndExecFailures},
        {"ProcessLauncherPreservesArgumentsAndWorkingDirectory",
         testProcessLauncherPreservesArgumentsAndWorkingDirectory},
        {"ProcessLauncherRejectsLossyArguments", testProcessLauncherRejectsLossyArguments},
        {"Sha256Provider", testSha256Provider},
        {"OpenSslSignatureVerifier", testOpenSslSignatureVerifier},
        {"StateStoreDownloadResume", testStateStoreDownloadResume},
        {"StateStoreDistinguishesMissingAndCorruptState", testStateStoreDistinguishesMissingAndCorruptState},
        {"StateStorePreservesLastKnownGoodSnapshot", testStateStorePreservesLastKnownGoodSnapshot},
        {"StateStoreConcurrentInstancesDoNotLoseUpdates", testStateStoreConcurrentInstancesDoNotLoseUpdates},
        {"StateStoreHealthyCommitUsesCompareAndSet", testStateStoreHealthyCommitUsesCompareAndSet},
        {"StateStoreWriteFailuresPreservePrimary", testStateStoreWriteFailuresPreservePrimary},
        {"StateStoreCrossProcessLockingAndCrashRecovery", testStateStoreCrossProcessLockingAndCrashRecovery},
        {"UpdaterQueuedCallbacksOutliveUpdater", testUpdaterQueuedCallbacksOutliveUpdater},
        {"UpdaterDirectCallbacksAreExceptionSafeAndReentrant",
         testUpdaterDirectCallbacksAreExceptionSafeAndReentrant},
        {"UpdaterOverlappingChecksAreNonBlockingAndCancellationIsolated",
         testUpdaterOverlappingChecksAreNonBlockingAndCancellationIsolated},
        {"UpdaterCanBeDestroyedFromDirectCallback", testUpdaterCanBeDestroyedFromDirectCallback},
        {"UpdaterNewGenerationInvalidatesReadyPlan", testUpdaterNewGenerationInvalidatesReadyPlan},
        {"UpdaterQueuedDownloadKeepsRequestedGeneration", testUpdaterQueuedDownloadKeepsRequestedGeneration},
        {"UpdaterRequiresPersistedPendingBeforeReady", testUpdaterRequiresPersistedPendingBeforeReady},
        {"UpdaterApplyRequiresMatchingPersistedPending", testUpdaterApplyRequiresMatchingPersistedPending},
        {"UpdaterHealthyMarkPreservesFuturePending", testUpdaterHealthyMarkPreservesFuturePending},
        {"UpdaterPeriodicCheckPreservesReadyGeneration", testUpdaterPeriodicCheckPreservesReadyGeneration},
        {"UpdaterQueueOverflowErrorReentryIsBounded", testUpdaterQueueOverflowErrorReentryIsBounded},
        {"UpdaterQueuedDispatcherSuppressesStaleGenerationAfterDestruction",
         testUpdaterQueuedDispatcherSuppressesStaleGenerationAfterDestruction},
        {"UpdaterHealthyMarkRequiresMatchingTerminalReceipt",
         testUpdaterHealthyMarkRequiresMatchingTerminalReceipt},
        {"ApplyLauncherBoundsProcessWaitTimeout", testApplyLauncherBoundsProcessWaitTimeout},
        {"UpdaterDelegatesRollbackToTerminalBoundExternalPlan",
         testUpdaterDelegatesRollbackToTerminalBoundExternalPlan},
        {"UpdaterFailsClosedWhenAcceptedStateIsUnreadable",
         testUpdaterFailsClosedWhenAcceptedStateIsUnreadable},
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
