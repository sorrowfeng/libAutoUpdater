#include "TestCommon.h"

#include "libAutoUpdater/interfaces/IStateStore.h"

#include <filesystem>
#include <fstream>

void testStateStoreDownloadResume() {
    const auto root = std::filesystem::temp_directory_path() / "libAutoUpdater-state-test";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root, ec);

    auto store = autoupdater::createJsonStateStore(root / "state.json");
    autoupdater::DownloadResumeState state;
    state.key = "https://example.com/file.bin";
    state.offset = 42;
    state.etag = "\"abc\"";
    state.lastModified = "Mon, 01 Jun 2026 10:00:00 GMT";
    state.sha256 = "hash";

    auto saved = store->saveDownloadResume(state);
    LAU_REQUIRE(saved);

    auto loaded = store->loadDownloadResume(state.key);
    LAU_REQUIRE(loaded);
    LAU_REQUIRE(loaded.value().has_value());
    LAU_REQUIRE(loaded.value()->offset == 42);
    LAU_REQUIRE(loaded.value()->etag == "\"abc\"");
    LAU_REQUIRE(loaded.value()->sha256 == "hash");

    auto cleared = store->clearDownloadResume(state.key);
    LAU_REQUIRE(cleared);
    auto afterClear = store->loadDownloadResume(state.key);
    LAU_REQUIRE(afterClear);
    LAU_REQUIRE(!afterClear.value().has_value());

    autoupdater::PendingUpdate pending;
    pending.version = autoupdater::Version::parse("2.0.0").value();
    pending.releaseId = "release-2";
    pending.backupDir = root / "backup";
    pending.applyPlanPath = root / "apply-plan.json";
    pending.applyPlanDigest = std::string(64, 'a');
    LAU_REQUIRE(store->savePendingUpdate(pending));
    auto loadedPending = store->loadPendingUpdate();
    LAU_REQUIRE(loadedPending);
    LAU_REQUIRE(loadedPending.value().has_value());
    LAU_REQUIRE(loadedPending.value()->applyPlanDigest == pending.applyPlanDigest);
    LAU_REQUIRE(loadedPending.value()->backupDir == pending.backupDir);

    const auto limitedPath = root / "limited-state.json";
    {
        std::ofstream output(limitedPath, std::ios::binary | std::ios::trunc);
        output << "12345";
    }
    autoupdater::ResourceLimits limits;
    limits.maxStateBytes = 4;
    auto limitedStore = autoupdater::createJsonStateStore(limitedPath, limits);
    auto oversizedLoad = limitedStore->loadLastAcceptedVersion();
    LAU_REQUIRE(!oversizedLoad);
    LAU_REQUIRE(oversizedLoad.error().code == autoupdater::ErrorCode::ResourceLimitExceeded);

    std::filesystem::remove(limitedPath, ec);
    state.offset = limits.maxArtifactBytes + 1;
    auto oversizedResume = limitedStore->saveDownloadResume(state);
    LAU_REQUIRE(!oversizedResume);
    LAU_REQUIRE(oversizedResume.error().code == autoupdater::ErrorCode::ResourceLimitExceeded);

    std::filesystem::remove_all(root, ec);
}
