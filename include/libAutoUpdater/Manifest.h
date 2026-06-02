#pragma once

#include "libAutoUpdater/Result.h"
#include "libAutoUpdater/Version.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace autoupdater {

/// A single managed file in a release manifest.
///
/// path is the server-side path relative to Manifest::baseUrl. localPath, when
/// set, is the install-root-relative destination path. This split supports
/// content-addressed object storage.
struct ManifestFile {
    std::string path;
    std::string localPath;
    std::string sha256;
    std::uint64_t size = 0;
};

/// Release manifest describing the complete target state for one platform.
struct Manifest {
    int schemaVersion = 1;
    std::string appId;
    std::string channel;
    std::string platform;
    std::string arch;
    Version version;
    std::string releaseId;
    std::string releaseDate;
    std::string publishedAt;
    std::string expiresAt;
    std::optional<Version> minVersion;
    std::optional<Version> minClientVersion;
    bool mandatory = false;
    bool allowDowngrade = false;
    std::string notes;
    std::string baseUrl;
    std::vector<ManifestFile> files;
    std::vector<std::string> remove;

    static Result<Manifest> parse(const std::string& jsonText) noexcept;
    std::string toJson() const;
};

/// One platform/architecture routing entry in an index manifest.
struct IndexTarget {
    std::string platform;
    std::string arch;
    std::string manifestUrl;
};

/// Lightweight routing manifest that points clients to platform-specific releases.
struct IndexManifest {
    int schemaVersion = 1;
    std::string appId;
    std::string channel;
    std::string generatedAt;
    std::vector<IndexTarget> targets;

    static Result<IndexManifest> parse(const std::string& jsonText) noexcept;
};

} // namespace autoupdater
