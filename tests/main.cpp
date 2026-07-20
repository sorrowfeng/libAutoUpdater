#include "TestCommon.h"

#include <string>
#include <vector>

void testStructuredDiagnosticsAreStableAndNonSensitive();
void testVersionParsingAndOrdering();
void testManifestParsing();
void testManifestRejectsPathTraversal();
void testManifestRejectsConflictingManagedTargets();
void testManifestRejectsReservedUpdaterTargetsFromEveryTargetForm();
void testManifestAllowsSharedSourceForDistinctManagedTargets();
void testManifestAllowsNonConflictingTargetPrefixesAndSiblings();
void testManifestFetcherRoutesIndexManifest();
void testManifestFetcherRanksWildcardIndexTargets();
void testManifestFetcherRejectsAmbiguousIndexTargets();
void testManifestFetcherRequiresConcreteClientRouteDimensions();
void testManifestFetcherRejectsInvalidReleaseBehindSignedIndex();
void testManifestFetcherRejectsDisallowedIndexTarget();
void testManifestFetcherRejectsAllowedBaseUrlPrefixBypass();
void testManifestFetcherRejectsInitialUrlBeforeNetwork();
void testManifestFetcherResolvesIndexTargetFromEffectiveUrl();
void testManifestFetcherResolvesSignaturesFromEffectiveUrl();
void testManifestFetcherKeepsQueriesInTheirUriComponent();
void testManifestFetcherResolvesEmptyAndRelativeArtifactBases();
void testUrlPolicyFailsClosedAndRejectsMalformedUrls();
void testUrlPolicyCanonicalizesAndEnforcesScopeBoundaries();
void testUrlPolicyRejectsLocalAndAmbiguousAddressLiterals();
void testUrlUtilitiesPreserveUriComponentSemantics();
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
void testJsonRfc8259SyntaxAndUnicode();
void testJsonExactNumericContract();
void testMetadataSchemasRequireExactTypesAndRanges();
void testStateAndJournalSchemasFailClosed();
void testRfc3339ProfileAndNormalization();
void testMetadataTimestampsUseStrictProfile();
void testManifestAndSignatureResourceLimits();
void testApplyPlanWriterReconcilesPublishedCommitAcknowledgement();
void testUpdatePlannerCreatesOperations();
void testUpdatePlannerPercentEncodesArtifactPaths();
void testUpdatePlannerEnforcesRfc3339ExpiryBoundary();
void testUpdatePlannerRequiresVerifiedLocalDowngradeAuthorization();
void testUpdatePlannerUsesHighestDowngradeBaseline();
void testUpdatePlannerRejectsUnauthorizedDowngradeBeforeReinstallDecision();
void testUpdatePlannerPreservesNormalUpgradeAndSameVersionSemantics();
void testUpdatePlannerRejectsProgrammaticManagedTargetConflictsEarly();
void testUpdatePlannerAllowsSharedSourceForDistinctManagedTargets();
void testUpdatePlannerIndexesLargeSnapshots();
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
void testApplyExecutorReportsRestartFailurePhase();
void testApplyExecutorRecoversAfterForcedTermination();
void testApplyExecutorRejectsSourceTargetBackupLinks();
void testApplyExecutorRejectsProgrammaticManagedTargetConflictsEarly();
void testApplyExecutorValidatesProcessWaitInputs();
void testApplyExecutorReconcilesPublishedCommitAcknowledgements();
void testRootedFileSystemPinsHandlesAndRejectsSwaps();
void testLocalSnapshotUsesOneOpenedFileHandle();
void testDownloadExecutorContainsSwapsAndHardLinks();
void testDownloadExecutorKeepsValidatorsBoundToTheirResource();
void testDefaultNetworkAndDownloadExecutorReportCompleteFileProgress();
void testDownloadResumeUsesOpaqueStableResourceKeys();
void testDownloadExecutorBatchesResumePersistence();
void testDownloadExecutorPropagatesResumePersistenceFailures();
void testDownloadExecutorReportsHashAndPublicationFailures();
void testApplyExecutorUsesSafePosixPermissions();
void testProcessLauncherReportsSetupAndExecFailures();
void testProcessLauncherPreservesArgumentsAndWorkingDirectory();
void testProcessLauncherRejectsLossyArguments();
void testSha256Provider();
void testSha256DistinguishesReadFailureFromEof();
void testStdFileSystemAtomicReplacementPreservesTargetOnFailure();
void testStdFileSystemAtomicTextWritesPreserveContentAndPermissions();
void testStdFileSystemCopyPublishesOnlyCompleteFiles();
void testRootedPermissionCopyPreservesNativeState();
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
void testUpdaterConfirmsLegacyPendingFromTerminalSnapshot();
void testUpdaterScopesCustomStagingByManifest();
void testUpdaterHealthConfirmationDeadlineAndRetention();
void testUpdaterReconcilesCompletedRollbackPendingState();
void testUpdaterReconcilesLegacyCompletedRollbackPendingState();
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
        {"StructuredDiagnosticsAreStableAndNonSensitive", testStructuredDiagnosticsAreStableAndNonSensitive},
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
        {"ManifestFetcherRanksWildcardIndexTargets", testManifestFetcherRanksWildcardIndexTargets},
        {"ManifestFetcherRejectsAmbiguousIndexTargets", testManifestFetcherRejectsAmbiguousIndexTargets},
        {"ManifestFetcherRequiresConcreteClientRouteDimensions",
         testManifestFetcherRequiresConcreteClientRouteDimensions},
        {"ManifestFetcherRejectsInvalidReleaseBehindSignedIndex",
         testManifestFetcherRejectsInvalidReleaseBehindSignedIndex},
        {"ManifestFetcherRejectsDisallowedIndexTarget", testManifestFetcherRejectsDisallowedIndexTarget},
        {"ManifestFetcherRejectsAllowedBaseUrlPrefixBypass", testManifestFetcherRejectsAllowedBaseUrlPrefixBypass},
        {"ManifestFetcherRejectsInitialUrlBeforeNetwork", testManifestFetcherRejectsInitialUrlBeforeNetwork},
        {"ManifestFetcherResolvesIndexTargetFromEffectiveUrl", testManifestFetcherResolvesIndexTargetFromEffectiveUrl},
        {"ManifestFetcherResolvesSignaturesFromEffectiveUrl", testManifestFetcherResolvesSignaturesFromEffectiveUrl},
        {"ManifestFetcherKeepsQueriesInTheirUriComponent", testManifestFetcherKeepsQueriesInTheirUriComponent},
        {"ManifestFetcherResolvesEmptyAndRelativeArtifactBases",
         testManifestFetcherResolvesEmptyAndRelativeArtifactBases},
        {"UrlPolicyFailsClosedAndRejectsMalformedUrls", testUrlPolicyFailsClosedAndRejectsMalformedUrls},
        {"UrlPolicyCanonicalizesAndEnforcesScopeBoundaries", testUrlPolicyCanonicalizesAndEnforcesScopeBoundaries},
        {"UrlPolicyRejectsLocalAndAmbiguousAddressLiterals", testUrlPolicyRejectsLocalAndAmbiguousAddressLiterals},
        {"UrlUtilitiesPreserveUriComponentSemantics", testUrlUtilitiesPreserveUriComponentSemantics},
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
        {"JsonRfc8259SyntaxAndUnicode", testJsonRfc8259SyntaxAndUnicode},
        {"JsonExactNumericContract", testJsonExactNumericContract},
        {"MetadataSchemasRequireExactTypesAndRanges", testMetadataSchemasRequireExactTypesAndRanges},
        {"StateAndJournalSchemasFailClosed", testStateAndJournalSchemasFailClosed},
        {"Rfc3339ProfileAndNormalization", testRfc3339ProfileAndNormalization},
        {"MetadataTimestampsUseStrictProfile", testMetadataTimestampsUseStrictProfile},
        {"ManifestAndSignatureResourceLimits", testManifestAndSignatureResourceLimits},
        {"ApplyPlanWriterReconcilesPublishedCommitAcknowledgement",
         testApplyPlanWriterReconcilesPublishedCommitAcknowledgement},
        {"UpdatePlannerCreatesOperations", testUpdatePlannerCreatesOperations},
        {"UpdatePlannerPercentEncodesArtifactPaths", testUpdatePlannerPercentEncodesArtifactPaths},
        {"UpdatePlannerEnforcesRfc3339ExpiryBoundary", testUpdatePlannerEnforcesRfc3339ExpiryBoundary},
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
        {"UpdatePlannerIndexesLargeSnapshots", testUpdatePlannerIndexesLargeSnapshots},
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
        {"ApplyExecutorReportsRestartFailurePhase", testApplyExecutorReportsRestartFailurePhase},
        {"ApplyExecutorRecoversAfterForcedTermination", testApplyExecutorRecoversAfterForcedTermination},
        {"ApplyExecutorRejectsSourceTargetBackupLinks", testApplyExecutorRejectsSourceTargetBackupLinks},
        {"ApplyExecutorRejectsProgrammaticManagedTargetConflictsEarly",
         testApplyExecutorRejectsProgrammaticManagedTargetConflictsEarly},
        {"ApplyExecutorValidatesProcessWaitInputs", testApplyExecutorValidatesProcessWaitInputs},
        {"ApplyExecutorReconcilesPublishedCommitAcknowledgements",
         testApplyExecutorReconcilesPublishedCommitAcknowledgements},
        {"RootedFileSystemPinsHandlesAndRejectsSwaps", testRootedFileSystemPinsHandlesAndRejectsSwaps},
        {"LocalSnapshotUsesOneOpenedFileHandle", testLocalSnapshotUsesOneOpenedFileHandle},
        {"DownloadExecutorContainsSwapsAndHardLinks", testDownloadExecutorContainsSwapsAndHardLinks},
        {"DownloadExecutorKeepsValidatorsBoundToTheirResource",
         testDownloadExecutorKeepsValidatorsBoundToTheirResource},
        {"DefaultNetworkAndDownloadExecutorReportCompleteFileProgress",
         testDefaultNetworkAndDownloadExecutorReportCompleteFileProgress},
        {"DownloadResumeUsesOpaqueStableResourceKeys", testDownloadResumeUsesOpaqueStableResourceKeys},
        {"DownloadExecutorBatchesResumePersistence", testDownloadExecutorBatchesResumePersistence},
        {"DownloadExecutorPropagatesResumePersistenceFailures",
         testDownloadExecutorPropagatesResumePersistenceFailures},
        {"DownloadExecutorReportsHashAndPublicationFailures", testDownloadExecutorReportsHashAndPublicationFailures},
        {"ApplyExecutorUsesSafePosixPermissions", testApplyExecutorUsesSafePosixPermissions},
        {"ProcessLauncherReportsSetupAndExecFailures", testProcessLauncherReportsSetupAndExecFailures},
        {"ProcessLauncherPreservesArgumentsAndWorkingDirectory",
         testProcessLauncherPreservesArgumentsAndWorkingDirectory},
        {"ProcessLauncherRejectsLossyArguments", testProcessLauncherRejectsLossyArguments},
        {"Sha256Provider", testSha256Provider},
        {"Sha256DistinguishesReadFailureFromEof", testSha256DistinguishesReadFailureFromEof},
        {"StdFileSystemAtomicReplacementPreservesTargetOnFailure",
         testStdFileSystemAtomicReplacementPreservesTargetOnFailure},
        {"StdFileSystemAtomicTextWritesPreserveContentAndPermissions",
         testStdFileSystemAtomicTextWritesPreserveContentAndPermissions},
        {"StdFileSystemCopyPublishesOnlyCompleteFiles", testStdFileSystemCopyPublishesOnlyCompleteFiles},
        {"RootedPermissionCopyPreservesNativeState", testRootedPermissionCopyPreservesNativeState},
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
        {"UpdaterConfirmsLegacyPendingFromTerminalSnapshot",
         testUpdaterConfirmsLegacyPendingFromTerminalSnapshot},
        {"UpdaterScopesCustomStagingByManifest", testUpdaterScopesCustomStagingByManifest},
        {"UpdaterHealthConfirmationDeadlineAndRetention",
         testUpdaterHealthConfirmationDeadlineAndRetention},
        {"UpdaterReconcilesCompletedRollbackPendingState",
         testUpdaterReconcilesCompletedRollbackPendingState},
        {"UpdaterReconcilesLegacyCompletedRollbackPendingState",
         testUpdaterReconcilesLegacyCompletedRollbackPendingState},
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
