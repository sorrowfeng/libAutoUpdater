#include "libAutoUpdater/Manifest.h"

#include "util/Json.h"
#include "util/PathUtil.h"

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

std::string optionalString(const util::Json& object, const std::string& key) {
    const auto* value = object.get(key);
    return value && value->isString() ? value->asString() : std::string();
}

bool optionalBool(const util::Json& object, const std::string& key, bool fallback) {
    const auto* value = object.get(key);
    return value && value->isBool() ? value->asBool() : fallback;
}

Result<std::optional<Version>> optionalVersion(const util::Json& object, const std::string& key) {
    const auto* value = object.get(key);
    if (!value || value->isNull()) {
        return Result<std::optional<Version>>::ok(std::nullopt);
    }
    if (!value->isString()) {
        return Result<std::optional<Version>>::fail({ErrorCode::ManifestParseFailed, "Version field must be string: " + key});
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

} // namespace

Result<Manifest> Manifest::parse(const std::string& jsonText) noexcept {
    try {
        auto json = util::Json::parse(jsonText);
        if (!json) {
            return Result<Manifest>::fail(json.error());
        }
        if (!json.value().isObject()) {
            return Result<Manifest>::fail({ErrorCode::ManifestParseFailed, "Manifest root must be object"});
        }

        Manifest manifest;
        const auto* schema = json.value().get("schemaVersion");
        if (!schema || !schema->isNumber()) {
            return Result<Manifest>::fail({ErrorCode::ManifestParseFailed, "schemaVersion is required"});
        }
        manifest.schemaVersion = static_cast<int>(schema->asInt());
        if (manifest.schemaVersion != kSupportedManifestSchema) {
            return Result<Manifest>::fail({ErrorCode::UnsupportedManifestSchema, "Unsupported manifest schemaVersion"});
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

        manifest.appId = optionalString(json.value(), "appId");
        manifest.channel = optionalString(json.value(), "channel");
        manifest.platform = optionalString(json.value(), "platform");
        manifest.arch = optionalString(json.value(), "arch");
        manifest.releaseId = optionalString(json.value(), "releaseId");
        manifest.releaseDate = optionalString(json.value(), "releaseDate");
        manifest.publishedAt = optionalString(json.value(), "publishedAt");
        manifest.expiresAt = optionalString(json.value(), "expiresAt");
        manifest.notes = optionalString(json.value(), "notes");
        manifest.baseUrl = optionalString(json.value(), "baseUrl");
        manifest.mandatory = optionalBool(json.value(), "mandatory", false);
        manifest.allowDowngrade = optionalBool(json.value(), "allowDowngrade", false);

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
                file.localPath = optionalString(item, "localPath");
                file.sha256 = sha.value();
                const auto* size = item.get("size");
                if (!size || !size->isNumber()) {
                    return Result<Manifest>::fail({ErrorCode::ManifestParseFailed, "file size is required"});
                }
                file.size = static_cast<std::uint64_t>(size->asNumber());

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
                manifest.remove.push_back(item.asString());
            }
        }

        return Result<Manifest>::ok(std::move(manifest));
    } catch (...) {
        return Result<Manifest>::fail({ErrorCode::ManifestParseFailed, "Unexpected manifest parse failure"});
    }
}

std::string Manifest::toJson() const {
    util::Json::Object root;
    root.emplace("schemaVersion", static_cast<double>(schemaVersion));
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
        item.emplace("size", static_cast<double>(file.size));
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
    try {
        auto json = util::Json::parse(jsonText);
        if (!json) {
            return Result<IndexManifest>::fail(json.error());
        }
        if (!json.value().isObject()) {
            return Result<IndexManifest>::fail({ErrorCode::ManifestParseFailed, "Index manifest root must be object"});
        }

        IndexManifest manifest;
        const auto* schema = json.value().get("schemaVersion");
        if (!schema || !schema->isNumber()) {
            return Result<IndexManifest>::fail({ErrorCode::ManifestParseFailed, "schemaVersion is required"});
        }
        manifest.schemaVersion = static_cast<int>(schema->asInt());
        if (manifest.schemaVersion != kSupportedManifestSchema) {
            return Result<IndexManifest>::fail({ErrorCode::UnsupportedManifestSchema, "Unsupported index manifest schemaVersion"});
        }
        manifest.appId = optionalString(json.value(), "appId");
        manifest.channel = optionalString(json.value(), "channel");
        manifest.generatedAt = optionalString(json.value(), "generatedAt");

        const auto* targets = json.value().get("targets");
        if (!targets || !targets->isArray()) {
            return Result<IndexManifest>::fail({ErrorCode::ManifestParseFailed, "targets array is required"});
        }
        for (const auto& item : targets->asArray()) {
            if (!item.isObject()) {
                return Result<IndexManifest>::fail({ErrorCode::ManifestParseFailed, "target must be object"});
            }
            IndexTarget target;
            target.platform = optionalString(item, "platform");
            target.arch = optionalString(item, "arch");
            auto url = requiredString(item, "manifestUrl");
            if (!url) {
                return Result<IndexManifest>::fail(url.error());
            }
            target.manifestUrl = url.value();
            manifest.targets.push_back(std::move(target));
        }
        return Result<IndexManifest>::ok(std::move(manifest));
    } catch (...) {
        return Result<IndexManifest>::fail({ErrorCode::ManifestParseFailed, "Unexpected index manifest parse failure"});
    }
}

} // namespace autoupdater

