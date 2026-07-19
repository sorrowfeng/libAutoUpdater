#include "TestCommon.h"

#include "ApplyExecutor.h"
#include "ApplyPlanWriter.h"
#include "ManifestFetcher.h"
#include "DownloadExecutor.h"
#include "NetworkLimits.h"
#include "NetworkRequest.h"
#include "NetworkTestCommon.h"
#include "UrlPolicy.h"
#include "libAutoUpdater/ApplyPlan.h"
#include "libAutoUpdater/Manifest.h"
#include "libAutoUpdater/Updater.h"
#include "libAutoUpdater/interfaces/IHashProvider.h"
#include "libAutoUpdater/interfaces/ISignatureVerifier.h"
#include "util/Json.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

class MemoryRootedFile final : public autoupdater::IRootedFile {
  public:
    autoupdater::Result<std::size_t> read(void* buffer, std::size_t size) noexcept override {
        const auto available = contents_.size() - std::min(position_, contents_.size());
        const auto count = std::min(size, available);
        if (count > 0) {
            std::memcpy(buffer, contents_.data() + position_, count);
            position_ += count;
        }
        return autoupdater::Result<std::size_t>::ok(count);
    }

    autoupdater::Result<void> write(const void* data, std::size_t size) noexcept override {
        if (position_ > contents_.size()) {
            contents_.resize(position_, '\0');
        }
        if (size > contents_.max_size() - position_) {
            return autoupdater::Result<void>::fail(
                {autoupdater::ErrorCode::FileSystemError, "Test file size overflow"});
        }
        if (position_ + size > contents_.size()) {
            contents_.resize(position_ + size);
        }
        std::memcpy(contents_.data() + position_, data, size);
        position_ += size;
        return autoupdater::Result<void>::ok();
    }

    autoupdater::Result<void> seek(std::uint64_t offset) noexcept override {
        if (offset > std::numeric_limits<std::size_t>::max()) {
            return autoupdater::Result<void>::fail({autoupdater::ErrorCode::FileSystemError, "Test seek overflow"});
        }
        position_ = static_cast<std::size_t>(offset);
        return autoupdater::Result<void>::ok();
    }

    autoupdater::Result<void> truncate(std::uint64_t size) noexcept override {
        if (failNextTruncate) {
            failNextTruncate = false;
            return autoupdater::Result<void>::fail(
                {autoupdater::ErrorCode::FileSystemError, "Injected truncate failure"});
        }
        if (size > std::numeric_limits<std::size_t>::max()) {
            return autoupdater::Result<void>::fail({autoupdater::ErrorCode::FileSystemError, "Test truncate overflow"});
        }
        contents_.resize(static_cast<std::size_t>(size));
        position_ = std::min(position_, contents_.size());
        return autoupdater::Result<void>::ok();
    }

    autoupdater::Result<void> flush() noexcept override {
        return autoupdater::Result<void>::ok();
    }

    autoupdater::Result<autoupdater::RootedFileMetadata> metadata() noexcept override {
        autoupdater::RootedFileMetadata metadata;
        metadata.size = contents_.size();
        metadata.identity = "memory-file";
        return autoupdater::Result<autoupdater::RootedFileMetadata>::ok(metadata);
    }

    autoupdater::Result<void> setPermissions(std::filesystem::perms) noexcept override {
        return autoupdater::Result<void>::ok();
    }

    const std::string& contents() const noexcept {
        return contents_;
    }

    bool failNextTruncate = false;

  private:
    std::string contents_;
    std::size_t position_ = 0;
};

class LogicalLargeFile final : public autoupdater::IRootedFile {
  public:
    explicit LogicalLargeFile(std::uint64_t size) : size_(size), position_(size) {}

    autoupdater::Result<std::size_t> read(void*, std::size_t) noexcept override {
        return autoupdater::Result<std::size_t>::ok(0);
    }
    autoupdater::Result<void> write(const void*, std::size_t size) noexcept override {
        ++writeCalls;
        if (size > std::numeric_limits<std::uint64_t>::max() - position_) {
            return autoupdater::Result<void>::fail({autoupdater::ErrorCode::FileSystemError, "Logical file overflow"});
        }
        position_ += size;
        size_ = std::max(size_, position_);
        return autoupdater::Result<void>::ok();
    }
    autoupdater::Result<void> seek(std::uint64_t offset) noexcept override {
        position_ = offset;
        return autoupdater::Result<void>::ok();
    }
    autoupdater::Result<void> truncate(std::uint64_t size) noexcept override {
        size_ = size;
        position_ = std::min(position_, size_);
        return autoupdater::Result<void>::ok();
    }
    autoupdater::Result<void> flush() noexcept override {
        return autoupdater::Result<void>::ok();
    }
    autoupdater::Result<autoupdater::RootedFileMetadata> metadata() noexcept override {
        autoupdater::RootedFileMetadata metadata;
        metadata.size = size_;
        metadata.identity = "logical-large-file";
        return autoupdater::Result<autoupdater::RootedFileMetadata>::ok(metadata);
    }
    autoupdater::Result<void> setPermissions(std::filesystem::perms) noexcept override {
        return autoupdater::Result<void>::ok();
    }

    int writeCalls = 0;

  private:
    std::uint64_t size_ = 0;
    std::uint64_t position_ = 0;
};

class SplitOverflowNetwork final : public autoupdater::INetworkClient {
  public:
    autoupdater::Result<autoupdater::TextResponse> getText(const std::string&, const autoupdater::NetworkOptions&,
                                                           std::uint64_t,
                                                           autoupdater::CancellationToken&) noexcept override {
        return autoupdater::Result<autoupdater::TextResponse>::fail(
            {autoupdater::ErrorCode::NetworkError, "Text requests are not supported"});
    }

    autoupdater::Result<autoupdater::DownloadResult>
    downloadToFile(const std::string&, autoupdater::IRootedFile& target, const autoupdater::NetworkOptions&,
                   std::uint64_t, const std::optional<autoupdater::DownloadResumeInfo>&, autoupdater::ProgressCallback,
                   autoupdater::CancellationToken&) noexcept override {
        ++calls;
        auto prefix = target.write("1234", 4);
        if (!prefix) {
            return autoupdater::Result<autoupdater::DownloadResult>::fail(prefix.error());
        }
        auto overflow = target.write("5", 1);
        if (!overflow) {
            // Deliberately mask the file error to verify that the coordinator's
            // bounded wrapper preserves the resource-limit classification.
            return autoupdater::Result<autoupdater::DownloadResult>::fail(
                {autoupdater::ErrorCode::DownloadFailed, "Adapter masked the write failure"});
        }
        return autoupdater::Result<autoupdater::DownloadResult>::fail(
            {autoupdater::ErrorCode::DownloadFailed, "Expected bounded write failure"});
    }

    int calls = 0;
};

class CountingVerifier final : public autoupdater::ISignatureVerifier {
  public:
    autoupdater::Result<void> verify(std::string_view, std::string_view, std::string_view) noexcept override {
        ++calls;
        return autoupdater::Result<void>::ok();
    }

    int calls = 0;
};

autoupdater::UrlPolicy networkPolicy() {
    autoupdater::Config config;
    config.manifestUrl = "https://updates.example.test/releases/manifest.json";
    config.security.allowedBaseUrls = {"https://updates.example.test/releases/"};
    auto policy = autoupdater::UrlPolicy::fromConfig(config);
    LAU_REQUIRE(policy);
    return std::move(policy.value());
}

std::string smallManifest(std::uint64_t size = 0) {
    return std::string(
               R"json({"schemaVersion":1,"version":"1.0.0","files":[{"path":"app.bin","sha256":"00","size":)json") +
           std::to_string(size) + "}]}";
}

} // namespace

void testNetworkResourceLimits() {
    const std::string url = "https://updates.example.test/releases/manifest.json";
    autoupdater::NetworkOptions options;
    autoupdater::CancellationToken cancel;

    {
        auto policy = networkPolicy();
        autoupdater::test::ScriptedNetworkClient network;
        network.queueText(url, autoupdater::test::textResponse("1234"));
        auto result = autoupdater::fetchTextWithRedirects(url, options, 4, policy, network, cancel);
        LAU_REQUIRE(result);
    }
    {
        auto policy = networkPolicy();
        autoupdater::test::ScriptedNetworkClient network;
        autoupdater::test::ScriptedResponse response;
        response.headers.push_back(
            {"x", std::string(static_cast<std::size_t>(autoupdater::detail::kMaxNetworkResponseHeaderBytes - 5), 'a')});
        network.queueText(url, std::move(response));
        auto result = autoupdater::fetchTextWithRedirects(url, options, 4, policy, network, cancel);
        LAU_REQUIRE(result);
    }
    {
        auto policy = networkPolicy();
        autoupdater::test::ScriptedNetworkClient network;
        autoupdater::test::ScriptedResponse response;
        response.headers.push_back(
            {"x", std::string(static_cast<std::size_t>(autoupdater::detail::kMaxNetworkResponseHeaderBytes - 4), 'a')});
        network.queueText(url, std::move(response));
        auto result = autoupdater::fetchTextWithRedirects(url, options, 4, policy, network, cancel);
        LAU_REQUIRE(!result);
        LAU_REQUIRE(result.error().code == autoupdater::ErrorCode::ResourceLimitExceeded);
    }
    for (auto response :
         {autoupdater::test::textResponse("12345"),
          autoupdater::test::ScriptedResponse{200, {{"Content-Length", "1"}}, "12345", std::nullopt, {}},
          autoupdater::test::ScriptedResponse{200, {{"Transfer-Encoding", "chunked"}}, "12345", std::nullopt, {}},
          autoupdater::test::ScriptedResponse{200, {{"Content-Length", "5"}}, {}, std::nullopt, {}}}) {
        auto policy = networkPolicy();
        autoupdater::test::ScriptedNetworkClient network;
        network.queueText(url, std::move(response));
        auto result = autoupdater::fetchTextWithRedirects(url, options, 4, policy, network, cancel);
        LAU_REQUIRE(!result);
        LAU_REQUIRE(result.error().code == autoupdater::ErrorCode::ResourceLimitExceeded);
    }

    {
        auto policy = networkPolicy();
        SplitOverflowNetwork network;
        MemoryRootedFile target;
        auto result =
            autoupdater::downloadWithRedirects(url, target, options, 4, policy, network, std::nullopt, {}, cancel);
        LAU_REQUIRE(!result);
        LAU_REQUIRE(result.error().code == autoupdater::ErrorCode::ResourceLimitExceeded);
        LAU_REQUIRE(target.contents().empty());
        LAU_REQUIRE(network.calls == 1);
    }

    {
        auto policy = networkPolicy();
        SplitOverflowNetwork network;
        MemoryRootedFile target;
        target.failNextTruncate = true;
        auto result =
            autoupdater::downloadWithRedirects(url, target, options, 4, policy, network, std::nullopt, {}, cancel);
        LAU_REQUIRE(!result);
        LAU_REQUIRE(result.error().code == autoupdater::ErrorCode::ResourceLimitExceeded);
        LAU_REQUIRE(result.error().message.find("failed to restore") != std::string::npos);
        LAU_REQUIRE(target.contents() == "1234");
        LAU_REQUIRE(network.calls == 1);
    }

    {
        const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto root = std::filesystem::temp_directory_path() /
                          ("libAutoUpdater-invalid-resource-config-" + std::to_string(nonce));
        autoupdater::Config config;
        config.installDir = root / "install";
        config.resources.maxStateBytes = autoupdater::ResourceLimits{}.maxStateBytes + 1;
        autoupdater::Updater updater(config);
        auto result = updater.markCurrentVersionHealthy();
        LAU_REQUIRE(!result);
        LAU_REQUIRE(result.error().code == autoupdater::ErrorCode::InvalidConfig);
        LAU_REQUIRE(!std::filesystem::exists(root));
    }
    {
        const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto tempDir =
            std::filesystem::temp_directory_path() / ("libAutoUpdater-resource-limit-" + std::to_string(nonce));
        std::filesystem::create_directories(tempDir);

        autoupdater::Config config;
        config.manifestUrl = url;
        config.security.allowedBaseUrls = {"https://updates.example.test/releases/"};
        config.tempDir = tempDir;
        config.retry.maxRetries = 3;

        autoupdater::UpdateDecision decision;
        autoupdater::PlannedDownload download;
        download.file.path = "app.bin";
        download.file.size = 4;
        auto hash = autoupdater::createDefaultHashProvider();
        auto expectedHash = hash->sha256Bytes("1234");
        LAU_REQUIRE(expectedHash);
        download.file.sha256 = expectedHash.value();
        download.url = url;
        decision.downloads.push_back(std::move(download));

        SplitOverflowNetwork network;
        auto fileSystem = autoupdater::createDefaultFileSystem();
        auto result = autoupdater::executeDownloads(config, decision, network, *fileSystem, *hash, nullptr, {}, cancel);
        LAU_REQUIRE(!result);
        LAU_REQUIRE(result.error().code == autoupdater::ErrorCode::ResourceLimitExceeded);
        LAU_REQUIRE(network.calls == 1);
        LAU_REQUIRE(!std::filesystem::exists(tempDir / "app.bin.download"));
        std::error_code error;
        std::filesystem::remove_all(tempDir, error);
    }

    {
        auto policy = networkPolicy();
        autoupdater::test::ScriptedNetworkClient network;
        autoupdater::test::ScriptedResponse response;
        response.downloadedBytes = "x";
        network.queueDownload(url, std::move(response));
        LogicalLargeFile target(std::numeric_limits<std::uint64_t>::max());
        autoupdater::DownloadResumeInfo resume;
        resume.offset = std::numeric_limits<std::uint64_t>::max();
        auto result = autoupdater::downloadWithRedirects(
            url, target, options, std::numeric_limits<std::uint64_t>::max(), policy, network, resume, {}, cancel);
        LAU_REQUIRE(!result);
        LAU_REQUIRE(result.error().code == autoupdater::ErrorCode::ResourceLimitExceeded);
        LAU_REQUIRE(target.writeCalls == 0);
    }
}

void testJsonResourceLimits() {
    autoupdater::JsonResourceLimits limits;
    limits.maxDepth = 3;
    limits.maxNodes = 16;
    limits.maxStringBytes = 4;
    limits.maxNumberBytes = 4;
    limits.maxContainerEntries = 2;

    LAU_REQUIRE(autoupdater::util::Json::parse(R"([[0]])", limits));
    const auto depth = autoupdater::util::Json::parse(R"([[[0]]])", limits);
    LAU_REQUIRE(!depth);
    LAU_REQUIRE(depth.error().code == autoupdater::ErrorCode::ResourceLimitExceeded);

    const auto string = autoupdater::util::Json::parse(R"("12345")", limits);
    LAU_REQUIRE(!string);
    LAU_REQUIRE(string.error().code == autoupdater::ErrorCode::ResourceLimitExceeded);

    const auto number = autoupdater::util::Json::parse("12345", limits);
    LAU_REQUIRE(!number);
    LAU_REQUIRE(number.error().code == autoupdater::ErrorCode::ResourceLimitExceeded);

    const auto array = autoupdater::util::Json::parse("[0,1,2]", limits);
    LAU_REQUIRE(!array);
    LAU_REQUIRE(array.error().code == autoupdater::ErrorCode::ResourceLimitExceeded);

    limits.maxContainerEntries = 1;
    const auto duplicateObject = autoupdater::util::Json::parse(R"({"a":0,"a":1})", limits);
    LAU_REQUIRE(!duplicateObject);
    LAU_REQUIRE(duplicateObject.error().code == autoupdater::ErrorCode::ResourceLimitExceeded);
}

void testManifestAndSignatureResourceLimits() {
    {
        autoupdater::ResourceLimits limits;
        const auto document = smallManifest();
        limits.maxManifestBytes = document.size() - 1;
        auto manifest = autoupdater::Manifest::parse(document, limits);
        LAU_REQUIRE(!manifest);
        LAU_REQUIRE(manifest.error().code == autoupdater::ErrorCode::ResourceLimitExceeded);
    }
    {
        autoupdater::ResourceLimits limits;
        limits.maxArtifactBytes = 4;
        limits.maxTotalArtifactBytes = 8;
        auto manifest = autoupdater::Manifest::parse(smallManifest(5), limits);
        LAU_REQUIRE(!manifest);
        LAU_REQUIRE(manifest.error().code == autoupdater::ErrorCode::ResourceLimitExceeded);
    }
    {
        autoupdater::ResourceLimits limits;
        limits.maxApplyPlanBytes = 4;
        auto plan = autoupdater::ApplyPlan::parse("12345", limits);
        LAU_REQUIRE(!plan);
        LAU_REQUIRE(plan.error().code == autoupdater::ErrorCode::ResourceLimitExceeded);
    }
    {
        const std::string url = "https://updates.example.test/releases/manifest.json";
        autoupdater::Config config;
        config.manifestUrl = url;
        config.security.allowedBaseUrls = {"https://updates.example.test/releases/"};
        config.security.requireManifestSignature = true;
        config.resources.maxIndexBytes = 1024;
        config.resources.maxManifestBytes = 1024;
        config.resources.maxSignatureBytes = 4;

        autoupdater::test::ScriptedNetworkClient network;
        network.queueText(url, autoupdater::test::textResponse(smallManifest()));
        network.queueText(url + ".sig", autoupdater::test::textResponse("12345"));
        CountingVerifier verifier;
        auto hash = autoupdater::createDefaultHashProvider();
        autoupdater::CancellationToken cancel;
        auto result = autoupdater::fetchAndVerifyManifest(config, network, *hash, verifier, cancel);
        LAU_REQUIRE(!result);
        LAU_REQUIRE(result.error().code == autoupdater::ErrorCode::ResourceLimitExceeded);
        LAU_REQUIRE(verifier.calls == 0);
        LAU_REQUIRE(network.textLimits == std::vector<std::uint64_t>({1024, 4}));
    }
    {
        const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto root =
            std::filesystem::temp_directory_path() / ("libAutoUpdater-plan-producer-limit-" + std::to_string(nonce));

        autoupdater::Config config;
        config.appId = "example";
        config.installDir = root / "install";
        config.tempDir = root / "staging";
        config.resources.json.maxContainerEntries = 1;
        auto currentVersion = autoupdater::Version::parse("1.0.0");
        auto nextVersion = autoupdater::Version::parse("2.0.0");
        LAU_REQUIRE(currentVersion);
        LAU_REQUIRE(nextVersion);
        config.currentVersion = currentVersion.value();

        autoupdater::ManifestEnvelope envelope;
        envelope.manifest.version = nextVersion.value();
        envelope.sha256 = "00";

        autoupdater::UpdateDecision decision;
        autoupdater::ApplyOperation first;
        first.type = autoupdater::ApplyOperationType::Remove;
        first.target = "first.bin";
        decision.operations.push_back(first);
        autoupdater::ApplyOperation second = first;
        second.target = "second.bin";
        decision.operations.push_back(std::move(second));

        auto fileSystem = autoupdater::createDefaultFileSystem();
        auto result = autoupdater::writeApplyPlan(config, envelope, decision, *fileSystem);
        LAU_REQUIRE(!result);
        LAU_REQUIRE(result.error().code == autoupdater::ErrorCode::ResourceLimitExceeded);
        LAU_REQUIRE(!std::filesystem::exists(root));
    }
    {
        const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto root =
            std::filesystem::temp_directory_path() / ("libAutoUpdater-oversized-plan-" + std::to_string(nonce));
        autoupdater::ApplyPlan plan;
        plan.installDir = root / "install";
        plan.stagingDir = root / "staging";
        plan.backupDir = root / "backup";
        autoupdater::ApplyOperation operation;
        operation.type = autoupdater::ApplyOperationType::Replace;
        operation.source = "app.bin";
        operation.target = "app.bin";
        operation.size = autoupdater::ResourceLimits{}.maxArtifactBytes + 1;
        plan.operations.push_back(std::move(operation));

        auto result = autoupdater::updater::executeApplyPlan(plan);
        LAU_REQUIRE(!result);
        LAU_REQUIRE(result.error().code == autoupdater::ErrorCode::ResourceLimitExceeded);
        LAU_REQUIRE(!std::filesystem::exists(root));
    }
}
