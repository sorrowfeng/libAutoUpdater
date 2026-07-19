#include "TestCommon.h"

#include "libAutoUpdater/Manifest.h"

#include <utility>
#include <vector>

namespace {

autoupdater::Result<autoupdater::Manifest>
parseManifestTargets(std::vector<autoupdater::ManifestFile> files, std::vector<std::string> remove = {}) {
    autoupdater::Manifest manifest;
    manifest.version = autoupdater::Version::parse("1.1.0").value();
    manifest.files = std::move(files);
    manifest.remove = std::move(remove);
    return autoupdater::Manifest::parse(manifest.toJson());
}

void requireTargetConflict(const autoupdater::Result<autoupdater::Manifest>& result) {
    LAU_REQUIRE(!result);
    LAU_REQUIRE(result.error().code == autoupdater::ErrorCode::SecurityPolicyViolation);
}

} // namespace

void testManifestParsing() {
    const std::string json = R"json({
      "schemaVersion": 1,
      "appId": "com.example.app",
      "channel": "stable",
      "platform": "windows",
      "arch": "x64",
      "version": "1.4.0",
      "releaseId": "1.4.0+20260601.1",
      "baseUrl": "file:///tmp/release",
      "mandatory": true,
      "files": [
        {"path": "bin/app.exe", "sha256": "abc", "size": 3}
      ],
      "remove": ["old/file.txt"]
    })json";

    auto manifest = autoupdater::Manifest::parse(json);
    LAU_REQUIRE(manifest);
    LAU_REQUIRE(manifest.value().version.toString() == "1.4.0");
    LAU_REQUIRE(manifest.value().mandatory);
    LAU_REQUIRE(manifest.value().files.size() == 1);
    LAU_REQUIRE(manifest.value().remove.size() == 1);
}

void testManifestRejectsPathTraversal() {
    const std::string json = R"json({
      "schemaVersion": 1,
      "version": "1.0.0",
      "files": [
        {"path": "../evil", "sha256": "abc", "size": 3}
      ]
    })json";

    auto manifest = autoupdater::Manifest::parse(json);
    LAU_REQUIRE(!manifest);
    LAU_REQUIRE(manifest.error().code == autoupdater::ErrorCode::PathTraversalRejected);
}

void testManifestRejectsConflictingManagedTargets() {
    requireTargetConflict(parseManifestTargets({
        {"objects/first.bin", "bin/app.exe", "hash", 4},
        {"objects/second.bin", "bin/app.exe", "hash", 4},
    }));

    requireTargetConflict(parseManifestTargets({
        {"objects/first.bin", "Bin/App.exe", "hash", 4},
        {"objects/second.bin", "bin/app.EXE", "hash", 4},
    }));

    requireTargetConflict(parseManifestTargets({}, {"obsolete.dll", "obsolete.dll"}));

    requireTargetConflict(parseManifestTargets(
        {{"objects/app.bin", "bin/app.exe", "hash", 4}}, {"bin/app.exe"}));

    requireTargetConflict(parseManifestTargets({
        {"bin/app.exe", "", "hash", 4},
        {"objects/app.bin", "bin/app.exe", "hash", 4},
    }));

    requireTargetConflict(parseManifestTargets({
        {"objects/directory", "bin", "hash", 4},
        {"objects/child.bin", "bin/app.exe", "hash", 4},
    }));
    requireTargetConflict(parseManifestTargets({
        {"objects/child.bin", "bin/app.exe", "hash", 4},
        {"objects/directory", "bin", "hash", 4},
    }));

    requireTargetConflict(parseManifestTargets({
        {"objects/parent.bin", "a", "hash", 4},
        {"objects/interloper.bin", "a-variant", "hash", 4},
        {"objects/child.bin", "a/child", "hash", 4},
    }));
}

void testManifestRejectsReservedUpdaterTargetsFromEveryTargetForm() {
    requireTargetConflict(parseManifestTargets({
        {".autoupdater/staging/payload.bin", "", "hash", 4},
    }));

    requireTargetConflict(parseManifestTargets({
        {"objects/payload.bin", ".AUToupdater/staging/payload.bin", "hash", 4},
    }));

    requireTargetConflict(parseManifestTargets({}, {".autoupdater/journal/active.json"}));
}

void testManifestAllowsSharedSourceForDistinctManagedTargets() {
    const auto manifest = parseManifestTargets({
        {"objects/shared.bin", "bin/first.exe", "same-hash", 9},
        {"objects/shared.bin", "bin/second.exe", "same-hash", 9},
    });

    LAU_REQUIRE(manifest);
    LAU_REQUIRE(manifest.value().files.size() == 2);
    LAU_REQUIRE(manifest.value().files[0].path == manifest.value().files[1].path);
    LAU_REQUIRE(manifest.value().files[0].localPath != manifest.value().files[1].localPath);
}

void testManifestAllowsNonConflictingTargetPrefixesAndSiblings() {
    const auto manifest = parseManifestTargets({
        {"objects/a.bin", "a", "hash", 4},
        {"objects/ab.bin", "ab", "hash", 4},
        {"objects/first.bin", "shared/first.bin", "hash", 4},
        {"objects/second.bin", "shared/second.bin", "hash", 4},
    });

    LAU_REQUIRE(manifest);
    LAU_REQUIRE(manifest.value().files.size() == 4);
}
