#include "TestCommon.h"

#include "libAutoUpdater/Manifest.h"

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
