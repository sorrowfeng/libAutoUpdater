#include "libAutoUpdater/interfaces/IStateStore.h"

#include "default/JsonStateStoreInternal.h"
#include "libAutoUpdater/interfaces/IFileSystem.h"
#include "util/Json.h"
#include "util/PathUtil.h"
#include "util/Sha256.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>

namespace autoupdater {

namespace {

constexpr std::uint64_t kStateSchemaVersion = 1;
constexpr auto kLockRetryDelay = std::chrono::milliseconds(10);
constexpr std::size_t kLockAttempts = 500;

std::mutex& processStateStoreMutex() {
    // The on-disk lock is the cross-process/path-alias authority. A single
    // process mutex also covers separate JsonStateStore instances without a
    // fallible canonical-path registry and the state operations are tiny.
    static std::mutex mutex;
    return mutex;
}

Error stateError(std::string message) {
    return {ErrorCode::StateStoreError, std::move(message)};
}

Error storageError(const Error& error, const std::string& context) {
    if (error.code == ErrorCode::ResourceLimitExceeded || error.code == ErrorCode::PathTraversalRejected ||
        error.code == ErrorCode::SecurityPolicyViolation) {
        return {error.code, context + ": " + error.message};
    }
    return stateError(context + ": " + error.message);
}

bool pendingUpdatesEqual(const PendingUpdate& left, const PendingUpdate& right) {
    return left.version.toString() == right.version.toString() && left.releaseId == right.releaseId &&
           left.backupDir.lexically_normal() == right.backupDir.lexically_normal() &&
           left.applyPlanPath.lexically_normal() == right.applyPlanPath.lexically_normal() &&
           left.applyPlanDigest == right.applyPlanDigest;
}

bool pendingUpdatesEqual(const std::optional<PendingUpdate>& left, const std::optional<PendingUpdate>& right) {
    if (left.has_value() != right.has_value()) {
        return false;
    }
    return !left || pendingUpdatesEqual(*left, *right);
}

Result<void> requireOnlyKeys(const util::Json::Object& object, std::initializer_list<const char*> allowedKeys,
                             const std::string& context) {
    for (const auto& entry : object) {
        const auto allowed =
            std::any_of(allowedKeys.begin(), allowedKeys.end(), [&](const char* key) { return entry.first == key; });
        if (!allowed) {
            return Result<void>::fail(stateError(context + " contains unknown field '" + entry.first + "'"));
        }
    }
    return Result<void>::ok();
}

bool containsControlCharacter(const std::string& value) {
    return std::any_of(value.begin(), value.end(),
                       [](unsigned char character) { return character < 0x20 || character == 0x7f; });
}

Result<std::uint64_t> parseOffset(const util::Json& value) {
    if (!value.isUnsignedInteger()) {
        return Result<std::uint64_t>::fail(stateError("Download resume offset must be an integer"));
    }
    return Result<std::uint64_t>::ok(value.asUInt64());
}

Result<PendingUpdate> parsePendingUpdate(const util::Json& value, bool allowMissingLegacyDigest = false) {
    if (!value.isObject()) {
        return Result<PendingUpdate>::fail(stateError("pendingUpdate must be an object"));
    }
    const auto& object = value.asObject();
    auto keys = requireOnlyKeys(object, {"version", "releaseId", "backupDir", "applyPlanPath", "applyPlanDigest"},
                                "pendingUpdate");
    if (!keys) {
        return Result<PendingUpdate>::fail(keys.error());
    }

    const auto requireString = [&](const char* key) -> Result<std::string> {
        const auto it = object.find(key);
        if (it == object.end() || !it->second.isString()) {
            return Result<std::string>::fail(stateError(std::string("pendingUpdate.") + key + " must be a string"));
        }
        return Result<std::string>::ok(it->second.asString());
    };

    auto versionText = requireString("version");
    auto releaseId = requireString("releaseId");
    auto backupDirText = requireString("backupDir");
    auto applyPlanPathText = requireString("applyPlanPath");
    Result<std::string> applyPlanDigest =
        Result<std::string>::fail(stateError("pendingUpdate.applyPlanDigest is missing"));
    const auto digestIt = object.find("applyPlanDigest");
    if (digestIt == object.end() && allowMissingLegacyDigest) {
        applyPlanDigest = Result<std::string>::ok({});
    } else {
        applyPlanDigest = requireString("applyPlanDigest");
    }
    if (!versionText) {
        return Result<PendingUpdate>::fail(versionText.error());
    }
    if (!releaseId) {
        return Result<PendingUpdate>::fail(releaseId.error());
    }
    if (!backupDirText) {
        return Result<PendingUpdate>::fail(backupDirText.error());
    }
    if (!applyPlanPathText) {
        return Result<PendingUpdate>::fail(applyPlanPathText.error());
    }
    if (!applyPlanDigest) {
        return Result<PendingUpdate>::fail(applyPlanDigest.error());
    }

    auto version = Version::parse(versionText.value());
    if (!version) {
        return Result<PendingUpdate>::fail(stateError("pendingUpdate.version is invalid"));
    }
    if (backupDirText.value().empty() || applyPlanPathText.value().empty()) {
        return Result<PendingUpdate>::fail(stateError("Pending update paths must not be empty"));
    }
    if (containsControlCharacter(backupDirText.value()) || containsControlCharacter(applyPlanPathText.value())) {
        return Result<PendingUpdate>::fail(stateError("Pending update paths contain control characters"));
    }
    auto backupDir = util::pathFromUtf8(backupDirText.value());
    auto applyPlanPath = util::pathFromUtf8(applyPlanPathText.value());
    if (backupDir.empty() || applyPlanPath.empty()) {
        return Result<PendingUpdate>::fail(stateError("Pending update paths are not valid UTF-8 paths"));
    }
    if (!backupDir.is_absolute() || !applyPlanPath.is_absolute()) {
        return Result<PendingUpdate>::fail(stateError("Pending update paths must be absolute"));
    }
    if (applyPlanDigest.value().empty() && !allowMissingLegacyDigest) {
        return Result<PendingUpdate>::fail(
            stateError("pendingUpdate.applyPlanDigest must not be empty in schema version 1"));
    }
    if (!applyPlanDigest.value().empty() && !util::isLowerHexSha256(applyPlanDigest.value())) {
        return Result<PendingUpdate>::fail(stateError("pendingUpdate.applyPlanDigest must be a lowercase SHA-256"));
    }

    PendingUpdate pending;
    pending.version = version.value();
    pending.releaseId = std::move(releaseId.value());
    pending.backupDir = std::move(backupDir);
    pending.applyPlanPath = std::move(applyPlanPath);
    pending.applyPlanDigest = std::move(applyPlanDigest.value());
    return Result<PendingUpdate>::ok(std::move(pending));
}

Result<void> validateRoot(const util::Json::Object& root, const ResourceLimits& limits) {
    const auto schema = root.find("schemaVersion");
    if (schema != root.end() &&
        (!schema->second.isUnsignedInteger() || schema->second.asUInt64() != kStateSchemaVersion)) {
        return Result<void>::fail(stateError("State schemaVersion is unsupported"));
    }
    auto keys = requireOnlyKeys(
        root, {"schemaVersion", "lastAcceptedVersion", "lastAcceptedReleaseId", "pendingUpdate", "downloadResume"},
        "State root");
    if (!keys) {
        return keys;
    }

    const auto acceptedVersion = root.find("lastAcceptedVersion");
    const auto acceptedRelease = root.find("lastAcceptedReleaseId");
    if ((acceptedVersion == root.end()) != (acceptedRelease == root.end())) {
        return Result<void>::fail(stateError("Accepted version and release ID must be stored together"));
    }
    if (acceptedVersion != root.end()) {
        if (!acceptedVersion->second.isString() || !acceptedRelease->second.isString()) {
            return Result<void>::fail(stateError("Accepted version and release ID must be strings"));
        }
        auto parsed = Version::parse(acceptedVersion->second.asString());
        if (!parsed) {
            return Result<void>::fail(stateError("lastAcceptedVersion is invalid"));
        }
    }

    const auto pending = root.find("pendingUpdate");
    if (pending != root.end()) {
        auto parsed = parsePendingUpdate(pending->second, schema == root.end());
        if (!parsed) {
            return Result<void>::fail(parsed.error());
        }
    }

    const auto downloads = root.find("downloadResume");
    if (downloads != root.end()) {
        if (!downloads->second.isObject()) {
            return Result<void>::fail(stateError("downloadResume must be an object"));
        }
        if (downloads->second.asObject().size() > limits.json.maxContainerEntries) {
            return Result<void>::fail({ErrorCode::ResourceLimitExceeded, "Download resume entry limit exceeded"});
        }
        for (const auto& entry : downloads->second.asObject()) {
            if (entry.first.empty() || containsControlCharacter(entry.first) || !entry.second.isObject()) {
                return Result<void>::fail(stateError("Download resume entries must have a non-empty key and object"));
            }
            const auto& record = entry.second.asObject();
            auto recordKeys =
                requireOnlyKeys(record, {"offset", "etag", "lastModified", "sha256"}, "Download resume record");
            if (!recordKeys) {
                return recordKeys;
            }
            const auto offset = record.find("offset");
            const auto etag = record.find("etag");
            const auto lastModified = record.find("lastModified");
            const auto sha256 = record.find("sha256");
            if (offset == record.end() || etag == record.end() || lastModified == record.end() ||
                sha256 == record.end() || !etag->second.isString() || !lastModified->second.isString() ||
                !sha256->second.isString()) {
                return Result<void>::fail(stateError("Download resume record is incomplete or has invalid types"));
            }
            auto parsedOffset = parseOffset(offset->second);
            if (!parsedOffset) {
                return Result<void>::fail(parsedOffset.error());
            }
            if (parsedOffset.value() > limits.maxArtifactBytes) {
                return Result<void>::fail(
                    {ErrorCode::ResourceLimitExceeded, "Resume offset exceeds the artifact byte limit"});
            }
            if (!util::isLowerHexSha256(sha256->second.asString())) {
                return Result<void>::fail(stateError("Download resume SHA-256 is invalid"));
            }
            if (containsControlCharacter(etag->second.asString()) ||
                containsControlCharacter(lastModified->second.asString())) {
                return Result<void>::fail(stateError("Download resume validators contain control characters"));
            }
        }
    }
    return Result<void>::ok();
}

Result<util::Json::Object> parseRoot(const std::string& contents, const ResourceLimits& limits) {
    auto json = util::Json::parse(contents, limits.json);
    if (!json) {
        if (json.error().code == ErrorCode::ResourceLimitExceeded) {
            return Result<util::Json::Object>::fail(json.error());
        }
        return Result<util::Json::Object>::fail(
            stateError("State file contains invalid JSON: " + json.error().message));
    }
    if (!json.value().isObject()) {
        return Result<util::Json::Object>::fail(stateError("State file root must be an object"));
    }
    auto root = json.value().asObject();
    auto valid = validateRoot(root, limits);
    if (!valid) {
        return Result<util::Json::Object>::fail(valid.error());
    }
    return Result<util::Json::Object>::ok(std::move(root));
}

struct StoredFile {
    bool exists = false;
    std::string contents;
    RootedEntryExpectation expectation = RootedEntryExpectation::missing();
};

Result<StoredFile> readStoredFile(IRootedDirectory& root, const std::string& name, std::uint64_t maxBytes,
                                  const std::string& description) {
    auto opened = root.openRegularFile(name, RootedFileOpenMode::ReadOnly);
    if (!opened) {
        return Result<StoredFile>::fail(storageError(opened.error(), "Failed to open " + description));
    }
    if (!opened.value().exists()) {
        return Result<StoredFile>::ok({});
    }
    const auto failOpened = [&](Error error) {
        auto closed = opened.value().file->close();
        if (!closed) {
            error.message += "; failed to close " + description + ": " + closed.error().message;
        }
        return Result<StoredFile>::fail(std::move(error));
    };

    auto metadata = opened.value().file->metadata();
    if (!metadata) {
        return failOpened(storageError(metadata.error(), "Failed to inspect " + description));
    }
    if (metadata.value().size > maxBytes ||
        metadata.value().size > static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max)())) {
        return failOpened({ErrorCode::ResourceLimitExceeded, description + " exceeds its byte limit"});
    }

    StoredFile stored;
    stored.exists = true;
    stored.expectation = RootedEntryExpectation::matching(metadata.value());
    stored.contents.reserve(static_cast<std::size_t>(metadata.value().size));
    std::array<char, 64 * 1024> buffer{};
    for (;;) {
        auto read = opened.value().file->read(buffer.data(), buffer.size());
        if (!read) {
            return failOpened(storageError(read.error(), "Failed to read " + description));
        }
        if (read.value() == 0) {
            break;
        }
        if (read.value() > maxBytes - stored.contents.size()) {
            return failOpened({ErrorCode::ResourceLimitExceeded, description + " exceeds its byte limit"});
        }
        stored.contents.append(buffer.data(), read.value());
    }

    auto finalMetadata = opened.value().file->metadata();
    if (!finalMetadata) {
        return failOpened(storageError(finalMetadata.error(), "Failed to re-inspect " + description));
    }
    if (finalMetadata.value().identity != metadata.value().identity ||
        finalMetadata.value().size != metadata.value().size || stored.contents.size() != metadata.value().size) {
        return failOpened(stateError(description + " changed while it was being read"));
    }
    auto closed = opened.value().file->close();
    if (!closed) {
        return Result<StoredFile>::fail(storageError(closed.error(), "Failed to close " + description));
    }
    return Result<StoredFile>::ok(std::move(stored));
}

Result<void> verifyTemporaryFile(IRootedFile& file, const std::string& contents, const std::string& description) {
    auto metadata = file.metadata();
    if (!metadata) {
        return Result<void>::fail(storageError(metadata.error(), "Failed to inspect " + description));
    }
    if (metadata.value().size != contents.size()) {
        return Result<void>::fail(stateError(description + " has an unexpected size after writing"));
    }
    auto flushed = file.flush();
    if (!flushed) {
        return Result<void>::fail(storageError(flushed.error(), "Failed to flush " + description));
    }
    auto rewound = file.seek(0);
    if (!rewound) {
        return Result<void>::fail(storageError(rewound.error(), "Failed to rewind " + description));
    }

    std::string verified;
    verified.reserve(contents.size());
    std::array<char, 64 * 1024> buffer{};
    for (;;) {
        auto read = file.read(buffer.data(), buffer.size());
        if (!read) {
            return Result<void>::fail(storageError(read.error(), "Failed to verify " + description));
        }
        if (read.value() == 0) {
            break;
        }
        if (read.value() > contents.size() - verified.size()) {
            return Result<void>::fail(stateError(description + " grew during verification"));
        }
        verified.append(buffer.data(), read.value());
    }
    if (verified != contents) {
        return Result<void>::fail(stateError(description + " did not preserve the complete state document"));
    }
    return Result<void>::ok();
}

Result<void> writeAtomic(IRootedDirectory& root, const std::string& name, const std::string& contents,
                         const RootedEntryExpectation& expectation, std::uint64_t maxBytes,
                         const std::string& description) {
    auto temporary = root.createAtomicReplacement(name);
    if (!temporary) {
        return Result<void>::fail(storageError(temporary.error(), "Failed to create " + description));
    }
    auto written = temporary.value()->file().write(contents.data(), contents.size());
    if (!written) {
        auto error = storageError(written.error(), "Failed to write " + description);
        auto discarded = temporary.value()->discard();
        if (!discarded) {
            error.message += "; failed to discard incomplete " + description + ": " + discarded.error().message;
        }
        return Result<void>::fail(std::move(error));
    }
    auto verified = verifyTemporaryFile(temporary.value()->file(), contents, description);
    if (!verified) {
        auto error = verified.error();
        auto discarded = temporary.value()->discard();
        if (!discarded) {
            error.message += "; failed to discard invalid " + description + ": " + discarded.error().message;
        }
        return Result<void>::fail(std::move(error));
    }
    auto preparedMetadata = temporary.value()->file().metadata();
    if (!preparedMetadata) {
        auto error = storageError(preparedMetadata.error(), "Failed to inspect prepared " + description);
        auto discarded = temporary.value()->discard();
        if (!discarded) {
            error.message += "; failed to discard invalid " + description + ": " + discarded.error().message;
        }
        return Result<void>::fail(std::move(error));
    }
    auto committed = temporary.value()->commit(expectation);
    // commit() may report a post-rename durability failure. Close the bound
    // temporary handle before reopening the target so the visible outcome can
    // be reconciled instead of being mistaken for a definitely-unapplied write.
    auto discarded = temporary.value()->discard();
    const auto publication = temporary.value()->publishStatus();
    temporary.value().reset();
    if (!discarded) {
        auto error = !committed ? storageError(committed.error(), "Failed to commit " + description)
                                : storageError(discarded.error(), "Failed to finish " + description + " cleanup");
        if (!committed) {
            error.message += "; cleanup also failed: " + discarded.error().message;
        }
        return Result<void>::fail(std::move(error));
    }
    if (!committed) {
        auto commitError = storageError(committed.error(), "Failed to commit " + description);
        if (publication.publication == RootedPublication::NotPublished || !publication.namespaceDurable ||
            !publication.failureCanBeReconciled) {
            return Result<void>::fail(std::move(commitError));
        }
        auto observed = readStoredFile(root, name, maxBytes, description);
        if (!observed) {
            commitError.message += "; failed to reconcile publication: " + observed.error().message;
            return Result<void>::fail(std::move(commitError));
        }
        if (observed.value().exists && observed.value().contents == contents &&
            observed.value().expectation.kind == RootedEntryExpectationKind::Identity &&
            observed.value().expectation.identity == preparedMetadata.value().identity) {
            return Result<void>::ok();
        }
        return Result<void>::fail(std::move(commitError));
    }

    auto installed = readStoredFile(root, name, maxBytes, description);
    if (!installed) {
        return Result<void>::fail(installed.error());
    }
    if (!installed.value().exists || installed.value().contents != contents) {
        return Result<void>::fail(stateError(description + " failed post-commit verification"));
    }
    return Result<void>::ok();
}

class JsonStateStore final : public IStateStore {
  public:
    JsonStateStore(std::filesystem::path path, ResourceLimits limits, std::shared_ptr<IFileSystem> fileSystem,
                   detail::JsonStateStoreHooks hooks)
        : limits_(std::move(limits)), fileSystem_(std::move(fileSystem)), hooks_(std::move(hooks)) {
        try {
            std::error_code error;
            path_ = path.is_absolute() ? path.lexically_normal()
                                       : std::filesystem::absolute(path, error).lexically_normal();
            if (error || path_.empty() || path_.filename().empty() || path_.parent_path().empty()) {
                configurationError_ = stateError("State file path is invalid");
                return;
            }
            parentPath_ = path_.parent_path();
            fileName_ = util::pathToUtf8(path_.filename());
            if (fileName_.empty() || !util::validateManagedPath(fileName_)) {
                configurationError_ = stateError("State file name is not a safe managed path");
                return;
            }
            backupName_ = fileName_ + ".lkg";
            lockName_ = fileName_ + ".lock";
            if (!util::validateManagedPath(backupName_) || !util::validateManagedPath(lockName_)) {
                configurationError_ = stateError("State companion file names are invalid");
            }
        } catch (...) {
            configurationError_ = stateError("Failed to normalize state file path");
        }
    }

    Result<void> saveLastAcceptedVersion(const Version& version, const std::string& releaseId) noexcept override {
        try {
            auto state = lockAndLoad();
            if (!state) {
                return Result<void>::fail(state.error());
            }
            state.value().document["lastAcceptedVersion"] = util::Json(version.toString());
            state.value().document["lastAcceptedReleaseId"] = util::Json(releaseId);
            return saveLocked(state.value());
        } catch (...) {
            return Result<void>::fail(stateError("Unexpected accepted-version save failure"));
        }
    }

    Result<std::optional<Version>> loadLastAcceptedVersion() noexcept override {
        try {
            auto state = lockAndLoad();
            if (!state) {
                return Result<std::optional<Version>>::fail(state.error());
            }
            const auto it = state.value().document.find("lastAcceptedVersion");
            if (it == state.value().document.end()) {
                return Result<std::optional<Version>>::ok(std::nullopt);
            }
            auto version = Version::parse(it->second.asString());
            if (!version) {
                return Result<std::optional<Version>>::fail(stateError("lastAcceptedVersion is invalid"));
            }
            return Result<std::optional<Version>>::ok(version.value());
        } catch (...) {
            return Result<std::optional<Version>>::fail(stateError("Unexpected accepted-version load failure"));
        }
    }

    Result<std::string> loadLastAcceptedReleaseId() noexcept override {
        try {
            auto state = lockAndLoad();
            if (!state) {
                return Result<std::string>::fail(state.error());
            }
            const auto it = state.value().document.find("lastAcceptedReleaseId");
            return Result<std::string>::ok(it == state.value().document.end() ? std::string{} : it->second.asString());
        } catch (...) {
            return Result<std::string>::fail(stateError("Unexpected accepted-release load failure"));
        }
    }

    Result<void> commitHealthyVersion(const Version& version, const std::string& releaseId,
                                      const std::optional<PendingUpdate>& expectedPending) noexcept override {
        try {
            auto state = lockAndLoad();
            if (!state) {
                return Result<void>::fail(state.error());
            }
            auto actualPending = pendingFromRoot(state.value().document);
            if (!actualPending) {
                return Result<void>::fail(actualPending.error());
            }
            if (!pendingUpdatesEqual(actualPending.value(), expectedPending)) {
                const auto acceptedVersion = state.value().document.find("lastAcceptedVersion");
                const auto acceptedRelease = state.value().document.find("lastAcceptedReleaseId");
                if (!actualPending.value() && acceptedVersion != state.value().document.end() &&
                    acceptedRelease != state.value().document.end() &&
                    acceptedVersion->second.asString() == version.toString() &&
                    acceptedRelease->second.asString() == releaseId) {
                    // A prior attempt can become visible before its final
                    // durability acknowledgement. Recommit the complete
                    // terminal state so success still means a durability
                    // barrier was positively acknowledged.
                    return saveLocked(state.value());
                }
                return Result<void>::fail(stateError("Persisted pending update changed before healthy-version commit"));
            }
            state.value().document["lastAcceptedVersion"] = util::Json(version.toString());
            state.value().document["lastAcceptedReleaseId"] = util::Json(releaseId);
            state.value().document.erase("pendingUpdate");
            return saveLocked(state.value());
        } catch (...) {
            return Result<void>::fail(stateError("Unexpected healthy-version commit failure"));
        }
    }

    Result<void> savePendingUpdate(const PendingUpdate& pending) noexcept override {
        try {
            auto validated = pendingToJson(pending);
            if (!validated) {
                return Result<void>::fail(validated.error());
            }
            auto state = lockAndLoad();
            if (!state) {
                return Result<void>::fail(state.error());
            }
            state.value().document["pendingUpdate"] = std::move(validated.value());
            return saveLocked(state.value());
        } catch (...) {
            return Result<void>::fail(stateError("Unexpected pending-update save failure"));
        }
    }

    Result<std::optional<PendingUpdate>> loadPendingUpdate() noexcept override {
        try {
            auto state = lockAndLoad();
            if (!state) {
                return Result<std::optional<PendingUpdate>>::fail(state.error());
            }
            return pendingFromRoot(state.value().document);
        } catch (...) {
            return Result<std::optional<PendingUpdate>>::fail(stateError("Unexpected pending-update load failure"));
        }
    }

    Result<void> clearPendingUpdate() noexcept override {
        try {
            auto state = lockAndLoad();
            if (!state) {
                return Result<void>::fail(state.error());
            }
            state.value().document.erase("pendingUpdate");
            return saveLocked(state.value());
        } catch (...) {
            return Result<void>::fail(stateError("Unexpected pending-update clear failure"));
        }
    }

    Result<void> saveDownloadResume(const DownloadResumeState& resume) noexcept override {
        try {
            if (resume.key.empty() || containsControlCharacter(resume.key) || !util::isLowerHexSha256(resume.sha256)) {
                return Result<void>::fail(stateError("Download resume key or SHA-256 is invalid"));
            }
            if (containsControlCharacter(resume.etag) || containsControlCharacter(resume.lastModified)) {
                return Result<void>::fail(stateError("Download resume validators contain control characters"));
            }
            if (resume.offset > limits_.maxArtifactBytes) {
                return Result<void>::fail(
                    {ErrorCode::ResourceLimitExceeded, "Resume offset exceeds the artifact byte limit"});
            }
            auto state = lockAndLoad();
            if (!state) {
                return Result<void>::fail(state.error());
            }
            util::Json::Object downloads;
            const auto downloadsIt = state.value().document.find("downloadResume");
            if (downloadsIt != state.value().document.end()) {
                downloads = downloadsIt->second.asObject();
            }
            if (downloads.find(resume.key) == downloads.end() && downloads.size() >= limits_.json.maxContainerEntries) {
                return Result<void>::fail({ErrorCode::ResourceLimitExceeded, "Download resume entry limit exceeded"});
            }
            util::Json::Object record;
            record.emplace("offset", resume.offset);
            record.emplace("etag", resume.etag);
            record.emplace("lastModified", resume.lastModified);
            record.emplace("sha256", resume.sha256);
            downloads[resume.key] = util::Json(std::move(record));
            state.value().document["downloadResume"] = util::Json(std::move(downloads));
            return saveLocked(state.value());
        } catch (...) {
            return Result<void>::fail(stateError("Unexpected download-resume save failure"));
        }
    }

    Result<std::optional<DownloadResumeState>> loadDownloadResume(const std::string& key) noexcept override {
        try {
            auto state = lockAndLoad();
            if (!state) {
                return Result<std::optional<DownloadResumeState>>::fail(state.error());
            }
            const auto downloads = state.value().document.find("downloadResume");
            if (downloads == state.value().document.end()) {
                return Result<std::optional<DownloadResumeState>>::ok(std::nullopt);
            }
            const auto record = downloads->second.asObject().find(key);
            if (record == downloads->second.asObject().end()) {
                return Result<std::optional<DownloadResumeState>>::ok(std::nullopt);
            }
            DownloadResumeState resume;
            resume.key = key;
            auto offset = parseOffset(*record->second.get("offset"));
            if (!offset) {
                return Result<std::optional<DownloadResumeState>>::fail(offset.error());
            }
            resume.offset = offset.value();
            resume.etag = record->second.get("etag")->asString();
            resume.lastModified = record->second.get("lastModified")->asString();
            resume.sha256 = record->second.get("sha256")->asString();
            return Result<std::optional<DownloadResumeState>>::ok(std::move(resume));
        } catch (...) {
            return Result<std::optional<DownloadResumeState>>::fail(
                stateError("Unexpected download-resume load failure"));
        }
    }

    Result<void> clearDownloadResume(const std::string& key) noexcept override {
        try {
            auto state = lockAndLoad();
            if (!state) {
                return Result<void>::fail(state.error());
            }
            const auto downloadsIt = state.value().document.find("downloadResume");
            if (downloadsIt != state.value().document.end()) {
                auto downloads = downloadsIt->second.asObject();
                downloads.erase(key);
                state.value().document["downloadResume"] = util::Json(std::move(downloads));
            }
            return saveLocked(state.value());
        } catch (...) {
            return Result<void>::fail(stateError("Unexpected download-resume clear failure"));
        }
    }

  private:
    struct LockedState {
        std::unique_lock<std::mutex> processLock;
        std::unique_ptr<IRootedDirectory> root;
        std::unique_ptr<IRootedLock> diskLock;
        StoredFile primary;
        util::Json::Object document;
    };

    Result<LockedState> lockAndLoad() {
        if (configurationError_) {
            return Result<LockedState>::fail(*configurationError_);
        }
        if (!fileSystem_) {
            return Result<LockedState>::fail(stateError("State file system dependency is unavailable"));
        }

        LockedState state;
        state.processLock = std::unique_lock<std::mutex>(processStateStoreMutex());
        auto root =
            fileSystem_->openRoot(parentPath_, RootAccess::ReadWrite, true, RootedDirectoryCreationMode::Private);
        if (!root) {
            return Result<LockedState>::fail(storageError(root.error(), "Failed to open state directory"));
        }
        state.root = std::move(root.value());

        Error lockFailure = stateError("Timed out waiting for the state file lock");
        for (std::size_t attempt = 0; attempt < kLockAttempts; ++attempt) {
            auto locked = state.root->acquireExclusiveLock(lockName_);
            if (locked) {
                state.diskLock = std::move(locked.value());
                break;
            }
            if (locked.error().code != ErrorCode::ApplyFailed) {
                return Result<LockedState>::fail(storageError(locked.error(), "Failed to acquire state file lock"));
            }
            lockFailure = storageError(locked.error(), "State file is busy");
            if (attempt + 1 < kLockAttempts) {
                std::this_thread::sleep_for(kLockRetryDelay);
            }
        }
        if (!state.diskLock) {
            return Result<LockedState>::fail(lockFailure);
        }

        auto primary = readStoredFile(*state.root, fileName_, limits_.maxStateBytes, "state file");
        if (!primary) {
            return Result<LockedState>::fail(primary.error());
        }
        state.primary = std::move(primary.value());
        if (!state.primary.exists) {
            auto backup = readStoredFile(*state.root, backupName_, limits_.maxStateBytes, "last-known-good state file");
            if (!backup) {
                return Result<LockedState>::fail(backup.error());
            }
            if (backup.value().exists) {
                return Result<LockedState>::fail(
                    stateError("Primary state file is missing while a last-known-good snapshot exists"));
            }
            return Result<LockedState>::ok(std::move(state));
        }

        auto parsed = parseRoot(state.primary.contents, limits_);
        if (!parsed) {
            return Result<LockedState>::fail(parsed.error());
        }
        state.document = std::move(parsed.value());
        return Result<LockedState>::ok(std::move(state));
    }

    Result<void> saveLocked(LockedState& state) {
        const auto pending = state.document.find("pendingUpdate");
        bool preserveLegacySchema = false;
        if (state.document.find("schemaVersion") == state.document.end() && pending != state.document.end() &&
            pending->second.isObject()) {
            const auto* digest = pending->second.get("applyPlanDigest");
            preserveLegacySchema = !digest || (digest->isString() && digest->asString().empty());
        }
        if (!preserveLegacySchema) {
            state.document["schemaVersion"] = util::Json(kStateSchemaVersion);
        }
        auto valid = validateRoot(state.document, limits_);
        if (!valid) {
            return valid;
        }
        const util::Json json(state.document);
        auto usage = util::Json::validateResourceUsage(json, limits_.json);
        if (!usage) {
            return usage;
        }
        const auto contents = json.stringify(2);
        if (contents.size() > limits_.maxStateBytes) {
            return Result<void>::fail({ErrorCode::ResourceLimitExceeded, "State file exceeds its byte limit"});
        }

        if (state.primary.exists) {
            auto backup = readStoredFile(*state.root, backupName_, limits_.maxStateBytes, "last-known-good state file");
            if (!backup) {
                return Result<void>::fail(backup.error());
            }
            auto savedBackup = writeAtomic(*state.root, backupName_, state.primary.contents, backup.value().expectation,
                                           limits_.maxStateBytes, "last-known-good state file");
            if (!savedBackup) {
                return savedBackup;
            }
            checkpoint(detail::JsonStateStoreCheckpoint::BackupCommitted);

            auto savedPrimary = writeAtomic(*state.root, fileName_, contents, state.primary.expectation,
                                            limits_.maxStateBytes, "state file");
            if (!savedPrimary) {
                return savedPrimary;
            }
            checkpoint(detail::JsonStateStoreCheckpoint::PrimaryCommitted);
            return Result<void>::ok();
        }

        auto savedPrimary = writeAtomic(*state.root, fileName_, contents, RootedEntryExpectation::missing(),
                                        limits_.maxStateBytes, "state file");
        if (!savedPrimary) {
            return savedPrimary;
        }
        checkpoint(detail::JsonStateStoreCheckpoint::PrimaryCommitted);
        // There is no prior snapshot on a first write. The next mutation will
        // preserve this primary as the first last-known-good snapshot before
        // replacing it.
        return Result<void>::ok();
    }

    void checkpoint(detail::JsonStateStoreCheckpoint value) {
        if (hooks_.checkpoint) {
            hooks_.checkpoint(value);
        }
    }

    Result<util::Json> pendingToJson(const PendingUpdate& pending) const {
        if (pending.backupDir.empty() || pending.applyPlanPath.empty() ||
            !util::isLowerHexSha256(pending.applyPlanDigest)) {
            return Result<util::Json>::fail(stateError("Pending update metadata is incomplete or invalid"));
        }
        util::Json::Object object;
        object.emplace("version", pending.version.toString());
        object.emplace("releaseId", pending.releaseId);
        object.emplace("backupDir", util::pathToUtf8(pending.backupDir));
        object.emplace("applyPlanPath", util::pathToUtf8(pending.applyPlanPath));
        object.emplace("applyPlanDigest", pending.applyPlanDigest);
        util::Json value(std::move(object));
        auto parsed = parsePendingUpdate(value);
        if (!parsed) {
            return Result<util::Json>::fail(parsed.error());
        }
        return Result<util::Json>::ok(std::move(value));
    }

    Result<std::optional<PendingUpdate>> pendingFromRoot(const util::Json::Object& root) const {
        const auto pending = root.find("pendingUpdate");
        if (pending == root.end()) {
            return Result<std::optional<PendingUpdate>>::ok(std::nullopt);
        }
        const bool legacy = root.find("schemaVersion") == root.end();
        auto parsed = parsePendingUpdate(pending->second, legacy);
        if (!parsed) {
            return Result<std::optional<PendingUpdate>>::fail(parsed.error());
        }
        return Result<std::optional<PendingUpdate>>::ok(std::move(parsed.value()));
    }

    ResourceLimits limits_;
    std::shared_ptr<IFileSystem> fileSystem_;
    detail::JsonStateStoreHooks hooks_;
    std::filesystem::path path_;
    std::filesystem::path parentPath_;
    std::string fileName_;
    std::string backupName_;
    std::string lockName_;
    std::optional<Error> configurationError_;
};

} // namespace

std::shared_ptr<IStateStore> createJsonStateStore(const std::filesystem::path& path) {
    return createJsonStateStore(path, ResourceLimits{});
}

std::shared_ptr<IStateStore> createJsonStateStore(const std::filesystem::path& path, const ResourceLimits& limits) {
    return std::make_shared<JsonStateStore>(path, limits, createDefaultFileSystem(), detail::JsonStateStoreHooks{});
}

namespace detail {

std::shared_ptr<IStateStore> createJsonStateStoreForTesting(const std::filesystem::path& path,
                                                            const ResourceLimits& limits,
                                                            std::shared_ptr<IFileSystem> fileSystem,
                                                            JsonStateStoreHooks hooks) {
    return std::make_shared<JsonStateStore>(path, limits, std::move(fileSystem), std::move(hooks));
}

} // namespace detail

} // namespace autoupdater
