#pragma once

#include "libAutoUpdater/Result.h"
#include "libAutoUpdater/Version.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace autoupdater {

struct ManifestFile {
    std::string path;
    std::string localPath;
    std::string sha256;
    std::uint64_t size = 0;
};

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

struct IndexTarget {
    std::string platform;
    std::string arch;
    std::string manifestUrl;
};

struct IndexManifest {
    int schemaVersion = 1;
    std::string appId;
    std::string channel;
    std::string generatedAt;
    std::vector<IndexTarget> targets;

    static Result<IndexManifest> parse(const std::string& jsonText) noexcept;
};

} // namespace autoupdater

