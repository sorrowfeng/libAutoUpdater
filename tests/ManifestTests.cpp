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
        {"path": "bin/app.exe", "sha256": "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef", "size": 3}
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
        {"path": "../evil", "sha256": "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef", "size": 3}
      ]
    })json";

    auto manifest = autoupdater::Manifest::parse(json);
    LAU_REQUIRE(!manifest);
    LAU_REQUIRE(manifest.error().code == autoupdater::ErrorCode::PathTraversalRejected);
}

namespace {

std::string manifestWithFile(const std::string& fileFields) {
    return std::string(R"json({
      "schemaVersion": 1,
      "version": "1.0.0",
      "files": [
        {)json") +
           fileFields + R"json(}
      ]
    })json";
}

void expectPathRejected(const std::string& path) {
    const auto json = manifestWithFile(
        "\"path\": \"" + path +
        "\", \"sha256\": \"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\", \"size\": 3");
    auto manifest = autoupdater::Manifest::parse(json);
    LAU_REQUIRE_MSG(!manifest, std::string("path was accepted: ") + path);
    LAU_REQUIRE_MSG(manifest.error().code == autoupdater::ErrorCode::PathTraversalRejected,
                    std::string("unexpected error for ") + path + ": " + manifest.error().message);
}

} // namespace

void testManifestRejectsUnsafePaths() {
    expectPathRejected("/etc/passwd");                // absolute POSIX
    expectPathRejected("bin\\\\evil");                // backslash (JSON-escaped)
    expectPathRejected("dir/file.txt:stream");        // NTFS alternate data stream
    expectPathRejected("dir/trailing.");              // Windows strips trailing dots
    expectPathRejected("dir/trailing ");              // Windows strips trailing spaces
    expectPathRejected("NUL");                        // reserved device name
    expectPathRejected("dir/com1.txt");               // reserved device name with extension
    expectPathRejected(".autoupdater/update.lock/x"); // updater internal state
}

void testManifestRejectsLocalPathTraversal() {
    const auto json = manifestWithFile(
        "\"path\": \"pkg/app.exe\", \"localPath\": \"../evil\", "
        "\"sha256\": \"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\", \"size\": 3");
    auto manifest = autoupdater::Manifest::parse(json);
    LAU_REQUIRE(!manifest);
    LAU_REQUIRE(manifest.error().code == autoupdater::ErrorCode::PathTraversalRejected);
}

void testManifestRejectsInvalidSha256() {
    const auto json = manifestWithFile("\"path\": \"bin/app.exe\", \"sha256\": \"abc\", \"size\": 3");
    auto manifest = autoupdater::Manifest::parse(json);
    LAU_REQUIRE(!manifest);
    LAU_REQUIRE(manifest.error().code == autoupdater::ErrorCode::ManifestParseFailed);
}

void testManifestRejectsInvalidSize() {
    const std::string sha = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    for (const auto* size : {"-1", "1e300", "1.5"}) {
        const auto json = manifestWithFile("\"path\": \"bin/app.exe\", \"sha256\": \"" + sha + "\", \"size\": " + size);
        auto manifest = autoupdater::Manifest::parse(json);
        LAU_REQUIRE(!manifest);
    }
}

void testManifestRejectsUnsafeReleaseId() {
    const std::string json = R"json({
      "schemaVersion": 1,
      "version": "1.0.0",
      "releaseId": "../../escape",
      "files": []
    })json";
    auto manifest = autoupdater::Manifest::parse(json);
    LAU_REQUIRE(!manifest);
    LAU_REQUIRE(manifest.error().code == autoupdater::ErrorCode::ManifestParseFailed);
}
