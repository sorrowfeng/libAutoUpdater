#include "TestCommon.h"

#include "libAutoUpdater/interfaces/IStateStore.h"

#include <filesystem>

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

    std::filesystem::remove_all(root, ec);
}
