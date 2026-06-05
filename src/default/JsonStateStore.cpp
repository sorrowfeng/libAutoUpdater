#include "libAutoUpdater/interfaces/IStateStore.h"

#include "util/Json.h"
#include "util/PathUtil.h"

#include <fstream>
#include <sstream>

namespace autoupdater {

namespace {

class JsonStateStore final : public IStateStore {
  public:
    explicit JsonStateStore(std::filesystem::path path) : path_(std::move(path)) {}

    Result<void> saveLastAcceptedVersion(const Version& version, const std::string& releaseId) noexcept override {
        auto root = loadRoot();
        if (!root) {
            return Result<void>::fail(root.error());
        }
        root.value()["lastAcceptedVersion"] = util::Json(version.toString());
        root.value()["lastAcceptedReleaseId"] = util::Json(releaseId);
        return saveRoot(root.value());
    }

    Result<std::optional<Version>> loadLastAcceptedVersion() noexcept override {
        auto root = loadRoot();
        if (!root) {
            return Result<std::optional<Version>>::fail(root.error());
        }
        const auto it = root.value().find("lastAcceptedVersion");
        if (it == root.value().end() || !it->second.isString()) {
            return Result<std::optional<Version>>::ok(std::nullopt);
        }
        auto version = Version::parse(it->second.asString());
        if (!version) {
            return Result<std::optional<Version>>::fail(version.error());
        }
        return Result<std::optional<Version>>::ok(version.value());
    }

    Result<std::string> loadLastAcceptedReleaseId() noexcept override {
        auto root = loadRoot();
        if (!root) {
            return Result<std::string>::fail(root.error());
        }
        const auto it = root.value().find("lastAcceptedReleaseId");
        if (it == root.value().end() || !it->second.isString()) {
            return Result<std::string>::ok({});
        }
        return Result<std::string>::ok(it->second.asString());
    }

    Result<void> savePendingUpdate(const PendingUpdate& pending) noexcept override {
        auto root = loadRoot();
        if (!root) {
            return Result<void>::fail(root.error());
        }
        util::Json::Object object;
        object.emplace("version", pending.version.toString());
        object.emplace("releaseId", pending.releaseId);
        object.emplace("backupDir", util::pathToUtf8(pending.backupDir));
        object.emplace("applyPlanPath", util::pathToUtf8(pending.applyPlanPath));
        root.value()["pendingUpdate"] = util::Json(std::move(object));
        return saveRoot(root.value());
    }

    Result<std::optional<PendingUpdate>> loadPendingUpdate() noexcept override {
        auto root = loadRoot();
        if (!root) {
            return Result<std::optional<PendingUpdate>>::fail(root.error());
        }
        const auto it = root.value().find("pendingUpdate");
        if (it == root.value().end() || !it->second.isObject()) {
            return Result<std::optional<PendingUpdate>>::ok(std::nullopt);
        }
        const auto& object = it->second;
        const auto* versionJson = object.get("version");
        if (!versionJson || !versionJson->isString()) {
            return Result<std::optional<PendingUpdate>>::ok(std::nullopt);
        }
        auto version = Version::parse(versionJson->asString());
        if (!version) {
            return Result<std::optional<PendingUpdate>>::fail(version.error());
        }
        PendingUpdate pending;
        pending.version = version.value();
        const auto* releaseId = object.get("releaseId");
        if (releaseId && releaseId->isString()) {
            pending.releaseId = releaseId->asString();
        }
        const auto* backupDir = object.get("backupDir");
        if (backupDir && backupDir->isString()) {
            pending.backupDir = util::pathFromUtf8(backupDir->asString());
        }
        const auto* applyPlanPath = object.get("applyPlanPath");
        if (applyPlanPath && applyPlanPath->isString()) {
            pending.applyPlanPath = util::pathFromUtf8(applyPlanPath->asString());
        }
        return Result<std::optional<PendingUpdate>>::ok(pending);
    }

    Result<void> clearPendingUpdate() noexcept override {
        auto root = loadRoot();
        if (!root) {
            return Result<void>::fail(root.error());
        }
        root.value().erase("pendingUpdate");
        return saveRoot(root.value());
    }

    Result<void> saveDownloadResume(const DownloadResumeState& state) noexcept override {
        auto root = loadRoot();
        if (!root) {
            return Result<void>::fail(root.error());
        }
        util::Json::Object downloads;
        const auto downloadsIt = root.value().find("downloadResume");
        if (downloadsIt != root.value().end() && downloadsIt->second.isObject()) {
            downloads = downloadsIt->second.asObject();
        }

        util::Json::Object record;
        record.emplace("offset", static_cast<double>(state.offset));
        record.emplace("etag", state.etag);
        record.emplace("lastModified", state.lastModified);
        record.emplace("sha256", state.sha256);
        downloads[state.key] = util::Json(std::move(record));
        root.value()["downloadResume"] = util::Json(std::move(downloads));
        return saveRoot(root.value());
    }

    Result<std::optional<DownloadResumeState>> loadDownloadResume(const std::string& key) noexcept override {
        auto root = loadRoot();
        if (!root) {
            return Result<std::optional<DownloadResumeState>>::fail(root.error());
        }
        const auto downloadsIt = root.value().find("downloadResume");
        if (downloadsIt == root.value().end() || !downloadsIt->second.isObject()) {
            return Result<std::optional<DownloadResumeState>>::ok(std::nullopt);
        }
        const auto recordIt = downloadsIt->second.asObject().find(key);
        if (recordIt == downloadsIt->second.asObject().end() || !recordIt->second.isObject()) {
            return Result<std::optional<DownloadResumeState>>::ok(std::nullopt);
        }

        DownloadResumeState state;
        state.key = key;
        const auto& object = recordIt->second;
        const auto* offset = object.get("offset");
        if (offset && offset->isNumber()) {
            state.offset = static_cast<std::uint64_t>(offset->asNumber());
        }
        const auto* etag = object.get("etag");
        if (etag && etag->isString()) {
            state.etag = etag->asString();
        }
        const auto* lastModified = object.get("lastModified");
        if (lastModified && lastModified->isString()) {
            state.lastModified = lastModified->asString();
        }
        const auto* sha256 = object.get("sha256");
        if (sha256 && sha256->isString()) {
            state.sha256 = sha256->asString();
        }
        return Result<std::optional<DownloadResumeState>>::ok(state);
    }

    Result<void> clearDownloadResume(const std::string& key) noexcept override {
        auto root = loadRoot();
        if (!root) {
            return Result<void>::fail(root.error());
        }
        const auto downloadsIt = root.value().find("downloadResume");
        if (downloadsIt != root.value().end() && downloadsIt->second.isObject()) {
            auto downloads = downloadsIt->second.asObject();
            downloads.erase(key);
            root.value()["downloadResume"] = util::Json(std::move(downloads));
        }
        return saveRoot(root.value());
    }

  private:
    Result<util::Json::Object> loadRoot() noexcept {
        try {
            if (!std::filesystem::exists(path_)) {
                return Result<util::Json::Object>::ok({});
            }
            std::ifstream input(path_, std::ios::binary);
            if (!input) {
                return Result<util::Json::Object>::fail({ErrorCode::StateStoreError, "Failed to open state file"});
            }
            std::ostringstream stream;
            stream << input.rdbuf();
            auto json = util::Json::parse(stream.str());
            if (!json) {
                return Result<util::Json::Object>::fail(json.error());
            }
            if (!json.value().isObject()) {
                return Result<util::Json::Object>::fail({ErrorCode::StateStoreError, "State file root must be object"});
            }
            return Result<util::Json::Object>::ok(json.value().asObject());
        } catch (...) {
            return Result<util::Json::Object>::fail({ErrorCode::StateStoreError, "Failed to read state file"});
        }
    }

    Result<void> saveRoot(const util::Json::Object& root) noexcept {
        try {
            std::error_code ec;
            std::filesystem::create_directories(path_.parent_path(), ec);
            if (ec) {
                return Result<void>::fail({ErrorCode::StateStoreError, ec.message()});
            }
            std::ofstream output(path_, std::ios::binary | std::ios::trunc);
            if (!output) {
                return Result<void>::fail({ErrorCode::StateStoreError, "Failed to write state file"});
            }
            output << util::Json(root).stringify(2);
            return Result<void>::ok();
        } catch (...) {
            return Result<void>::fail({ErrorCode::StateStoreError, "Failed to write state file"});
        }
    }

    std::filesystem::path path_;
};

} // namespace

std::shared_ptr<IStateStore> createJsonStateStore(const std::filesystem::path& path) {
    return std::make_shared<JsonStateStore>(path);
}

} // namespace autoupdater
