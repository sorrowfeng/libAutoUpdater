#include "TestCommon.h"

#include <vector>

void testVersionParsingAndOrdering();
void testManifestParsing();
void testManifestRejectsPathTraversal();
void testUpdatePlannerCreatesOperations();
void testApplyPlanRoundTrip();
void testSha256Provider();
void testStateStoreDownloadResume();

int main() {
    const std::vector<TestCase> tests = {
        {"VersionParsingAndOrdering", testVersionParsingAndOrdering},
        {"ManifestParsing", testManifestParsing},
        {"ManifestRejectsPathTraversal", testManifestRejectsPathTraversal},
        {"UpdatePlannerCreatesOperations", testUpdatePlannerCreatesOperations},
        {"ApplyPlanRoundTrip", testApplyPlanRoundTrip},
        {"Sha256Provider", testSha256Provider},
        {"StateStoreDownloadResume", testStateStoreDownloadResume},
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
