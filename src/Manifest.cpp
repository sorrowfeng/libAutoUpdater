#include "libAutoUpdater/Manifest.h"

#include "util/Json.h"
#include "util/PathUtil.h"
#include "util/Rfc3339.h"
#include "util/Sha256.h"

#include <algorithm>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <map>
#include <set>
#include <sstream>

namespace autoupdater {

namespace {

constexpr int kSupportedManifestSchema = 1;

Result<std::string> requiredString(const util::Json& object, const std::string& key) {
    const auto* value = object.get(key);
    if (!value || !value->isString()) {
        return Result<std::string>::fail({ErrorCode::ManifestParseFailed, "Required string field missing: " + key});
    }
    return Result<std::string>::ok(value->asString());
}

Result<std::string> optionalString(const util::Json& object, const std::string& key) {
    const auto* value = object.get(key);
    if (!value) {
        return Result<std::string>::ok({});
    }
    if (!value->isString()) {
        return Result<std::string>::fail({ErrorCode::ManifestParseFailed, "Optional field must be a string: " + key});
    }
    return Result<std::string>::ok(value->asString());
}

Result<bool> optionalBool(const util::Json& object, const std::string& key, bool fallback) {
    const auto* value = object.get(key);
    if (!value) {
        return Result<bool>::ok(fallback);
    }
    if (!value->isBool()) {
        return Result<bool>::fail({ErrorCode::ManifestParseFailed, "Optional field must be a boolean: " + key});
    }
    return Result<bool>::ok(value->asBool());
}

Result<void> validateOptionalTimestamp(const util::Json& object, const std::string& key) {
    const auto* value = object.get(key);
    if (!value) {
        return Result<void>::ok();
    }
    if (!value->isString() || value->asString().empty() || !util::parseRfc3339(value->asString())) {
        return Result<void>::fail(
            {ErrorCode::ManifestParseFailed, key + " must use the documented RFC 3339 timestamp profile"});
    }
    return Result<void>::ok();
}

Result<void> requireOnlyKeys(const util::Json& object, std::initializer_list<const char*> allowedKeys,
                             const std::string& context) {
    for (const auto& entry : object.asObject()) {
        const auto allowed =
            std::any_of(allowedKeys.begin(), allowedKeys.end(), [&](const char* key) { return entry.first == key; });
        if (!allowed) {
            return Result<void>::fail(
                {ErrorCode::ManifestParseFailed, context + " contains unknown field: " + entry.first});
        }
    }
    return Result<void>::ok();
}

bool isNonNegativeInteger(const util::Json& value) {
    return value.isUnsignedInteger() || (value.isSignedInteger() && value.asInt64(-1) >= 0);
}

std::string portableSourceKey(std::string path) {
    std::transform(path.begin(), path.end(), path.begin(), [](unsigned char character) {
        if (character == '/') {
            return '\0';
        }
        if (character >= 'A' && character <= 'Z') {
            return static_cast<char>(character - 'A' + 'a');
        }
        return static_cast<char>(character);
    });
    return path;
}

Result<void>
validateSourceTopology(const std::map<std::string, std::pair<std::string, std::uint64_t>>& sourceEvidence) {
    if (sourceEvidence.empty()) {
        return Result<void>::ok();
    }
    auto previous = sourceEvidence.begin();
    for (auto current = std::next(previous); current != sourceEvidence.end(); ++current, ++previous) {
        if (current->first.size() > previous->first.size() &&
            current->first.compare(0, previous->first.size(), previous->first) == 0 &&
            current->first[previous->first.size()] == '\0') {
            return Result<void>::fail(
                {ErrorCode::ManifestParseFailed, "Manifest source paths have an ancestor/descendant conflict"});
        }
    }
    return Result<void>::ok();
}

Result<std::optional<Version>> optionalVersion(const util::Json& object, const std::string& key) {
    const auto* value = object.get(key);
    if (!value || value->isNull()) {
        return Result<std::optional<Version>>::ok(std::nullopt);
    }
    if (!value->isString()) {
        return Result<std::optional<Version>>::fail(
            {ErrorCode::ManifestParseFailed, "Version field must be string: " + key});
    }
    auto parsed = Version::parse(value->asString());
    if (!parsed) {
        return Result<std::optional<Version>>::fail(parsed.error());
    }
    return Result<std::optional<Version>>::ok(parsed.value());
}

void addString(util::Json::Object& object, const std::string& key, const std::string& value) {
    if (!value.empty()) {
        object.emplace(key, value);
    }
}

Result<std::uint64_t> artifactSize(const util::Json& object, const ResourceLimits& limits) {
    const auto* value = object.get("size");
    if (!value || !isNonNegativeInteger(*value)) {
        return Result<std::uint64_t>::fail({ErrorCode::ManifestParseFailed, "file size is required"});
    }
    const auto parsed = value->asUInt64();
    if (parsed > limits.maxArtifactBytes) {
        return Result<std::uint64_t>::fail(
            {ErrorCode::ResourceLimitExceeded, "Artifact exceeds the per-file byte limit"});
    }
    return Result<std::uint64_t>::ok(parsed);
}

bool checkedAdd(std::uint64_t left, std::uint64_t right, std::uint64_t& result) {
    if (right > std::numeric_limits<std::uint64_t>::max() - left) {
        return false;
    }
    result = left + right;
    return true;
}

} // namespace

Result<Manifest> Manifest::parse(const std::string& jsonText) noexcept {
    return parse(jsonText, ResourceLimits{});
}

Result<Manifest> Manifest::parse(const std::string& jsonText, const ResourceLimits& limits) noexcept {
    try {
        if (jsonText.size() > limits.maxManifestBytes) {
            return Result<Manifest>::fail({ErrorCode::ResourceLimitExceeded, "Manifest exceeds its byte limit"});
        }
        auto json = util::Json::parse(jsonText, limits.json);
        if (!json) {
            return Result<Manifest>::fail(json.error());
        }
        if (!json.value().isObject()) {
            return Result<Manifest>::fail({ErrorCode::ManifestParseFailed, "Manifest root must be object"});
        }
        Manifest manifest;
        std::uint64_t totalArtifactBytes = 0;
        std::vector<std::string> managedTargets;
        std::map<std::string, std::pair<std::string, std::uint64_t>> sourceEvidence;
        const auto* schema = json.value().get("schemaVersion");
        if (!schema || !isNonNegativeInteger(*schema)) {
            return Result<Manifest>::fail({ErrorCode::ManifestParseFailed, "schemaVersion is required"});
        }
        if (schema->asUInt64() != static_cast<std::uint64_t>(kSupportedManifestSchema)) {
            return Result<Manifest>::fail({ErrorCode::UnsupportedManifestSchema, "Unsupported manifest schemaVersion"});
        }
        manifest.schemaVersion = kSupportedManifestSchema;
        auto rootKeys =
            requireOnlyKeys(json.value(),
                            {"schemaVersion", "appId", "channel", "platform", "arch", "version", "releaseId",
                             "releaseDate", "publishedAt", "expiresAt", "minVersion", "minClientVersion", "mandatory",
                             "allowDowngrade", "notes", "baseUrl", "files", "remove"},
                            "Manifest");
        if (!rootKeys) {
            return Result<Manifest>::fail(rootKeys.error());
        }

        auto versionText = requiredString(json.value(), "version");
        if (!versionText) {
            return Result<Manifest>::fail(versionText.error());
        }
        auto version = Version::parse(versionText.value());
        if (!version) {
            return Result<Manifest>::fail(version.error());
        }
        manifest.version = version.value();

        auto appId = optionalString(json.value(), "appId");
        auto channel = optionalString(json.value(), "channel");
        auto platform = optionalString(json.value(), "platform");
        auto arch = optionalString(json.value(), "arch");
        auto releaseId = optionalString(json.value(), "releaseId");
        auto releaseDate = optionalString(json.value(), "releaseDate");
        auto publishedAt = optionalString(json.value(), "publishedAt");
        auto expiresAt = optionalString(json.value(), "expiresAt");
        auto notes = optionalString(json.value(), "notes");
        auto baseUrl = optionalString(json.value(), "baseUrl");
        auto mandatory = optionalBool(json.value(), "mandatory", false);
        auto allowDowngrade = optionalBool(json.value(), "allowDowngrade", false);
        if (!appId)
            return Result<Manifest>::fail(appId.error());
        if (!channel)
            return Result<Manifest>::fail(channel.error());
        if (!platform)
            return Result<Manifest>::fail(platform.error());
        if (!arch)
            return Result<Manifest>::fail(arch.error());
        if (!releaseId)
            return Result<Manifest>::fail(releaseId.error());
        if (!releaseDate)
            return Result<Manifest>::fail(releaseDate.error());
        if (!publishedAt)
            return Result<Manifest>::fail(publishedAt.error());
        if (!expiresAt)
            return Result<Manifest>::fail(expiresAt.error());
        if (!notes)
            return Result<Manifest>::fail(notes.error());
        if (!baseUrl)
            return Result<Manifest>::fail(baseUrl.error());
        if (!mandatory)
            return Result<Manifest>::fail(mandatory.error());
        if (!allowDowngrade)
            return Result<Manifest>::fail(allowDowngrade.error());
        for (const auto* timestampField : {"releaseDate", "publishedAt", "expiresAt"}) {
            auto validTimestamp = validateOptionalTimestamp(json.value(), timestampField);
            if (!validTimestamp) {
                return Result<Manifest>::fail(validTimestamp.error());
            }
        }
        manifest.appId = std::move(appId.value());
        manifest.channel = std::move(channel.value());
        manifest.platform = std::move(platform.value());
        manifest.arch = std::move(arch.value());
        manifest.releaseId = std::move(releaseId.value());
        manifest.releaseDate = std::move(releaseDate.value());
        manifest.publishedAt = std::move(publishedAt.value());
        manifest.expiresAt = std::move(expiresAt.value());
        manifest.notes = std::move(notes.value());
        manifest.baseUrl = std::move(baseUrl.value());
        manifest.mandatory = mandatory.value();
        manifest.allowDowngrade = allowDowngrade.value();

        auto minVersion = optionalVersion(json.value(), "minVersion");
        if (!minVersion) {
            return Result<Manifest>::fail(minVersion.error());
        }
        manifest.minVersion = minVersion.value();

        auto minClientVersion = optionalVersion(json.value(), "minClientVersion");
        if (!minClientVersion) {
            return Result<Manifest>::fail(minClientVersion.error());
        }
        manifest.minClientVersion = minClientVersion.value();

        const auto* files = json.value().get("files");
        if (files) {
            if (!files->isArray()) {
                return Result<Manifest>::fail({ErrorCode::ManifestParseFailed, "files must be array"});
            }
            for (const auto& item : files->asArray()) {
                if (!item.isObject()) {
                    return Result<Manifest>::fail({ErrorCode::ManifestParseFailed, "files item must be object"});
                }
                auto fileKeys = requireOnlyKeys(item, {"path", "localPath", "sha256", "size"}, "Manifest file");
                if (!fileKeys) {
                    return Result<Manifest>::fail(fileKeys.error());
                }
                ManifestFile file;
                auto path = requiredString(item, "path");
                auto sha = requiredString(item, "sha256");
                if (!path) {
                    return Result<Manifest>::fail(path.error());
                }
                if (!sha) {
                    return Result<Manifest>::fail(sha.error());
                }
                file.path = path.value();
                auto localPath = optionalString(item, "localPath");
                if (!localPath) {
                    return Result<Manifest>::fail(localPath.error());
                }
                file.localPath = std::move(localPath.value());
                file.sha256 = sha.value();
                if (!util::isLowerHexSha256(file.sha256)) {
                    return Result<Manifest>::fail(
                        {ErrorCode::ManifestParseFailed, "file sha256 must be a lowercase SHA-256 digest"});
                }
                auto size = artifactSize(item, limits);
                if (!size) {
                    return Result<Manifest>::fail(size.error());
                }
                file.size = size.value();
                std::uint64_t updatedTotal = 0;
                if (!checkedAdd(totalArtifactBytes, file.size, updatedTotal) ||
                    updatedTotal > limits.maxTotalArtifactBytes) {
                    return Result<Manifest>::fail(
                        {ErrorCode::ResourceLimitExceeded, "Manifest artifacts exceed the total byte limit"});
                }
                totalArtifactBytes = updatedTotal;

                auto validPath = util::validateManagedPath(file.path);
                if (!validPath) {
                    return Result<Manifest>::fail(validPath.error());
                }
                if (!file.localPath.empty()) {
                    auto validLocalPath = util::validateManagedPath(file.localPath);
                    if (!validLocalPath) {
                        return Result<Manifest>::fail(validLocalPath.error());
                    }
                }
                const auto sourceKey = portableSourceKey(file.path);
                const auto existingEvidence = sourceEvidence.find(sourceKey);
                if (existingEvidence == sourceEvidence.end()) {
                    sourceEvidence.emplace(sourceKey, std::make_pair(file.sha256, file.size));
                } else if (existingEvidence->second.first != file.sha256 ||
                           existingEvidence->second.second != file.size) {
                    return Result<Manifest>::fail(
                        {ErrorCode::ManifestParseFailed,
                         "Shared manifest source paths must use identical size and SHA-256 evidence"});
                }
                managedTargets.push_back(file.localPath.empty() ? file.path : file.localPath);
                manifest.files.push_back(std::move(file));
            }
        }

        const auto* remove = json.value().get("remove");
        if (remove) {
            if (!remove->isArray()) {
                return Result<Manifest>::fail({ErrorCode::ManifestParseFailed, "remove must be array"});
            }
            for (const auto& item : remove->asArray()) {
                if (!item.isString()) {
                    return Result<Manifest>::fail({ErrorCode::ManifestParseFailed, "remove item must be string"});
                }
                auto validPath = util::validateManagedPath(item.asString());
                if (!validPath) {
                    return Result<Manifest>::fail(validPath.error());
                }
                managedTargets.push_back(item.asString());
                manifest.remove.push_back(item.asString());
            }
        }

        auto validSources = validateSourceTopology(sourceEvidence);
        if (!validSources) {
            return Result<Manifest>::fail(validSources.error());
        }

        auto validTargets = util::validateManagedTargetPaths(managedTargets);
        if (!validTargets) {
            return Result<Manifest>::fail(validTargets.error());
        }

        return Result<Manifest>::ok(std::move(manifest));
    } catch (...) {
        return Result<Manifest>::fail({ErrorCode::ManifestParseFailed, "Unexpected manifest parse failure"});
    }
}

std::string Manifest::toJson() const {
    util::Json::Object root;
    root.emplace("schemaVersion", schemaVersion);
    addString(root, "appId", appId);
    addString(root, "channel", channel);
    addString(root, "platform", platform);
    addString(root, "arch", arch);
    root.emplace("version", version.toString());
    addString(root, "releaseId", releaseId);
    addString(root, "releaseDate", releaseDate);
    addString(root, "publishedAt", publishedAt);
    addString(root, "expiresAt", expiresAt);
    if (minVersion) {
        root.emplace("minVersion", minVersion->toString());
    }
    if (minClientVersion) {
        root.emplace("minClientVersion", minClientVersion->toString());
    }
    root.emplace("mandatory", mandatory);
    root.emplace("allowDowngrade", allowDowngrade);
    addString(root, "notes", notes);
    addString(root, "baseUrl", baseUrl);

    util::Json::Array fileArray;
    for (const auto& file : files) {
        util::Json::Object item;
        item.emplace("path", file.path);
        if (!file.localPath.empty()) {
            item.emplace("localPath", file.localPath);
        }
        item.emplace("sha256", file.sha256);
        item.emplace("size", file.size);
        fileArray.emplace_back(std::move(item));
    }
    root.emplace("files", std::move(fileArray));

    util::Json::Array removeArray;
    for (const auto& path : remove) {
        removeArray.emplace_back(path);
    }
    root.emplace("remove", std::move(removeArray));

    return util::Json(std::move(root)).stringify(2);
}

Result<IndexManifest> IndexManifest::parse(const std::string& jsonText) noexcept {
    return parse(jsonText, ResourceLimits{});
}

Result<IndexManifest> IndexManifest::parse(const std::string& jsonText, const ResourceLimits& limits) noexcept {
    try {
        if (jsonText.size() > limits.maxIndexBytes) {
            return Result<IndexManifest>::fail(
                {ErrorCode::ResourceLimitExceeded, "Index manifest exceeds its byte limit"});
        }
        auto json = util::Json::parse(jsonText, limits.json);
        if (!json) {
            return Result<IndexManifest>::fail(json.error());
        }
        if (!json.value().isObject()) {
            return Result<IndexManifest>::fail({ErrorCode::ManifestParseFailed, "Index manifest root must be object"});
        }
        IndexManifest manifest;
        const auto* schema = json.value().get("schemaVersion");
        if (!schema || !isNonNegativeInteger(*schema)) {
            return Result<IndexManifest>::fail({ErrorCode::ManifestParseFailed, "schemaVersion is required"});
        }
        if (schema->asUInt64() != static_cast<std::uint64_t>(kSupportedManifestSchema)) {
            return Result<IndexManifest>::fail(
                {ErrorCode::UnsupportedManifestSchema, "Unsupported index manifest schemaVersion"});
        }
        manifest.schemaVersion = kSupportedManifestSchema;
        auto rootKeys = requireOnlyKeys(json.value(), {"schemaVersion", "appId", "channel", "generatedAt", "targets"},
                                        "Index manifest");
        if (!rootKeys) {
            return Result<IndexManifest>::fail(rootKeys.error());
        }
        auto appId = optionalString(json.value(), "appId");
        auto channel = optionalString(json.value(), "channel");
        auto generatedAt = optionalString(json.value(), "generatedAt");
        if (!appId)
            return Result<IndexManifest>::fail(appId.error());
        if (!channel)
            return Result<IndexManifest>::fail(channel.error());
        if (!generatedAt)
            return Result<IndexManifest>::fail(generatedAt.error());
        auto validGeneratedAt = validateOptionalTimestamp(json.value(), "generatedAt");
        if (!validGeneratedAt) {
            return Result<IndexManifest>::fail(validGeneratedAt.error());
        }
        manifest.appId = std::move(appId.value());
        manifest.channel = std::move(channel.value());
        manifest.generatedAt = std::move(generatedAt.value());

        const auto* targets = json.value().get("targets");
        if (!targets || !targets->isArray()) {
            return Result<IndexManifest>::fail({ErrorCode::ManifestParseFailed, "targets array is required"});
        }
        std::set<std::pair<std::string, std::string>> selectors;
        for (const auto& item : targets->asArray()) {
            if (!item.isObject()) {
                return Result<IndexManifest>::fail({ErrorCode::ManifestParseFailed, "target must be object"});
            }
            auto targetKeys = requireOnlyKeys(item, {"platform", "arch", "manifestUrl"}, "Index target");
            if (!targetKeys) {
                return Result<IndexManifest>::fail(targetKeys.error());
            }
            IndexTarget target;
            auto platform = optionalString(item, "platform");
            auto arch = optionalString(item, "arch");
            if (!platform)
                return Result<IndexManifest>::fail(platform.error());
            if (!arch)
                return Result<IndexManifest>::fail(arch.error());
            target.platform = std::move(platform.value());
            target.arch = std::move(arch.value());
            auto url = requiredString(item, "manifestUrl");
            if (!url) {
                return Result<IndexManifest>::fail(url.error());
            }
            if (url.value().empty()) {
                return Result<IndexManifest>::fail(
                    {ErrorCode::ManifestParseFailed, "Index target manifestUrl must not be empty"});
            }
            target.manifestUrl = url.value();
            if (!selectors.emplace(target.platform, target.arch).second) {
                return Result<IndexManifest>::fail(
                    {ErrorCode::ManifestParseFailed, "Index target selectors must be unique"});
            }
            manifest.targets.push_back(std::move(target));
        }
        return Result<IndexManifest>::ok(std::move(manifest));
    } catch (...) {
        return Result<IndexManifest>::fail({ErrorCode::ManifestParseFailed, "Unexpected index manifest parse failure"});
    }
}

} // namespace autoupdater
