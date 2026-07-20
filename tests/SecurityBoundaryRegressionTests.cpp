#include "TestCommon.h"

#include "ApplyExecutor.h"
#include "ManifestFetcher.h"
#include "NetworkRequest.h"
#include "NetworkTestCommon.h"
#include "UpdatePlanner.h"
#include "UrlPolicy.h"
#include "libAutoUpdater/interfaces/IHashProvider.h"
#include "libAutoUpdater/interfaces/IRootedFileSystem.h"
#include "libAutoUpdater/interfaces/ISignatureVerifier.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

void writeBoundaryFile(const std::filesystem::path& path, const std::string& contents) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    LAU_REQUIRE(output.good());
    output << contents;
    LAU_REQUIRE(output.good());
}

std::string readBoundaryFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    LAU_REQUIRE(input.good());
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

bool isLowerHex(std::string_view value) {
    return !value.empty() && std::all_of(value.begin(), value.end(), [](char character) {
        return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f');
    });
}

void requireJournalFilesStayInTheirRoot(const std::filesystem::path& installDir) {
    const auto journalDir = installDir / ".autoupdater" / "journal";
    LAU_REQUIRE(std::filesystem::is_directory(journalDir));

    std::size_t journalFiles = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(journalDir)) {
        LAU_REQUIRE(entry.is_regular_file());
        LAU_REQUIRE(entry.path().parent_path() == journalDir);

        const auto name = entry.path().filename().string();
        if (name == "terminal.json") {
            ++journalFiles;
            continue;
        }

        constexpr std::string_view planSuffix = ".plan.json";
        constexpr std::string_view summarySuffix = ".json";
        const auto suffix = name.size() >= planSuffix.size() &&
                                    name.compare(name.size() - planSuffix.size(), planSuffix.size(), planSuffix) == 0
                                ? planSuffix
                                : summarySuffix;
        LAU_REQUIRE(name.size() > suffix.size());
        LAU_REQUIRE(name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0);
        const auto transactionId = name.substr(0, name.size() - suffix.size());
        LAU_REQUIRE(transactionId.size() == 64);
        LAU_REQUIRE(isLowerHex(transactionId));
        ++journalFiles;
    }
    LAU_REQUIRE(journalFiles == 3);
}

void testUntrustedReleaseIdsCannotSelectJournalPaths() {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root =
        std::filesystem::temp_directory_path() / ("libAutoUpdater-security-boundary-journal-" + std::to_string(nonce));
    std::error_code error;
    std::filesystem::remove_all(root, error);

    const auto traversalSentinel = root / "journal-traversal.json";
    const auto windowsTraversalSentinel = root / "journal-windows.json";
    const auto absoluteSentinel = root / "absolute-journal.json";
    writeBoundaryFile(traversalSentinel, "traversal-sentinel");
    writeBoundaryFile(windowsTraversalSentinel, "windows-sentinel");
    writeBoundaryFile(absoluteSentinel, "absolute-sentinel");

    const std::vector<std::string> hostileReleaseIds = {
        "../../../journal-traversal", "..\\..\\..\\journal-windows",    (root / "absolute-journal").string(),
        "nested/separator",           std::string("embedded\0nul", 12), std::string(4096, 'a'),
    };

    for (std::size_t index = 0; index < hostileReleaseIds.size(); ++index) {
        autoupdater::ApplyPlan plan;
        plan.installDir = root / ("install-" + std::to_string(index));
        plan.stagingDir = root / ("staging-" + std::to_string(index));
        plan.backupDir = root / ("backup-" + std::to_string(index));
        plan.toVersion = "2.0.0";
        plan.releaseId = hostileReleaseIds[index];
        plan.manifestSha256 = std::string(64, 'd');

        const auto applied = autoupdater::updater::executeApplyPlan(plan);
        LAU_REQUIRE(applied);
        requireJournalFilesStayInTheirRoot(plan.installDir);
    }

    LAU_REQUIRE(readBoundaryFile(traversalSentinel) == "traversal-sentinel");
    LAU_REQUIRE(readBoundaryFile(windowsTraversalSentinel) == "windows-sentinel");
    LAU_REQUIRE(readBoundaryFile(absoluteSentinel) == "absolute-sentinel");

    std::filesystem::remove_all(root, error);
}

class BoundaryMemoryFile final : public autoupdater::IRootedFile {
  public:
    autoupdater::Result<std::size_t> read(void* buffer, std::size_t size) noexcept override {
        const auto remaining = contents_.size() - position_;
        const auto count = std::min(size, remaining);
        std::copy_n(contents_.data() + position_, count, static_cast<char*>(buffer));
        position_ += count;
        return autoupdater::Result<std::size_t>::ok(count);
    }

    autoupdater::Result<void> write(const void* data, std::size_t size) noexcept override {
        if (size > contents_.max_size() - position_) {
            return autoupdater::Result<void>::fail(
                {autoupdater::ErrorCode::FileSystemError, "Boundary test file size overflow"});
        }
        if (position_ + size > contents_.size()) {
            contents_.resize(position_ + size);
        }
        const auto* bytes = static_cast<const char*>(data);
        std::copy(bytes, bytes + size, contents_.begin() + static_cast<std::ptrdiff_t>(position_));
        position_ += size;
        return autoupdater::Result<void>::ok();
    }

    autoupdater::Result<void> seek(std::uint64_t offset) noexcept override {
        if (offset > std::numeric_limits<std::size_t>::max()) {
            return autoupdater::Result<void>::fail(
                {autoupdater::ErrorCode::FileSystemError, "Boundary test seek overflow"});
        }
        position_ = static_cast<std::size_t>(offset);
        return autoupdater::Result<void>::ok();
    }

    autoupdater::Result<void> truncate(std::uint64_t size) noexcept override {
        if (size > std::numeric_limits<std::size_t>::max()) {
            return autoupdater::Result<void>::fail(
                {autoupdater::ErrorCode::FileSystemError, "Boundary test truncate overflow"});
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
        metadata.identity = "security-boundary-memory-file";
        return autoupdater::Result<autoupdater::RootedFileMetadata>::ok(std::move(metadata));
    }

    autoupdater::Result<void> setPermissions(std::filesystem::perms) noexcept override {
        return autoupdater::Result<void>::ok();
    }

    autoupdater::Result<void> close() noexcept override {
        return autoupdater::Result<void>::ok();
    }

    const std::string& contents() const noexcept {
        return contents_;
    }

  private:
    std::string contents_;
    std::size_t position_ = 0;
};

autoupdater::UrlPolicy downloadBoundaryPolicy() {
    autoupdater::Config config;
    config.manifestUrl = "https://updates.example.test/releases/manifest.json";
    config.security.allowedBaseUrls = {
        "https://updates.example.test/releases/",
        "http://updates.example.test/releases/",
    };
    auto policy = autoupdater::UrlPolicy::fromConfig(config);
    LAU_REQUIRE(policy);
    return std::move(policy.value());
}

void testArtifactDownloadRejectsRedirectDowngradeAndRestoresTheTarget() {
    const std::string initial = "https://updates.example.test/releases/artifact.bin";
    const std::string downgraded = "http://updates.example.test/releases/artifact.bin";
    autoupdater::test::ScriptedNetworkClient network;
    auto redirect = autoupdater::test::redirectResponse(302, downgraded);
    redirect.downloadedBytes = "untrusted-redirect-body";
    network.queueDownload(initial, std::move(redirect));

    BoundaryMemoryFile target;
    autoupdater::NetworkOptions options;
    autoupdater::CancellationToken cancel;
    auto policy = downloadBoundaryPolicy();
    const auto downloaded =
        autoupdater::downloadWithRedirects(initial, target, options, 128, policy, network, std::nullopt, {}, cancel);

    LAU_REQUIRE(!downloaded);
    LAU_REQUIRE(downloaded.error().code == autoupdater::ErrorCode::SecurityPolicyViolation);
    LAU_REQUIRE(network.downloadRequests == std::vector<std::string>({initial}));
    LAU_REQUIRE(target.contents().empty());
}

class BoundarySignatureVerifier final : public autoupdater::ISignatureVerifier {
  public:
    autoupdater::Result<void> verify(std::string_view, std::string_view, std::string_view) noexcept override {
        ++calls;
        return autoupdater::Result<void>::ok();
    }

    int calls = 0;
};

void testFetchedUnsignedManifestCannotAuthorizeDowngrade() {
    const std::string manifestUrl = "https://updates.example.test/releases/manifest.json";
    autoupdater::Config config;
    config.appId = "com.example.security-boundary";
    config.channel = "stable";
    config.platform = "windows";
    config.arch = "x64";
    config.currentVersion = autoupdater::Version::parse("2.0.0").value();
    config.manifestUrl = manifestUrl;
    config.security.allowedBaseUrls = {"https://updates.example.test/releases/"};
    config.security.rejectDowngrade = false;
    config.security.requireManifestSignature = false;

    autoupdater::test::ScriptedNetworkClient network;
    network.queueText(manifestUrl, autoupdater::test::textResponse(R"json({
      "schemaVersion": 1,
      "appId": "com.example.security-boundary",
      "channel": "stable",
      "platform": "windows",
      "arch": "x64",
      "version": "1.5.0",
      "allowDowngrade": true,
      "files": []
    })json"));

    auto hash = autoupdater::createDefaultHashProvider();
    BoundarySignatureVerifier verifier;
    autoupdater::CancellationToken cancel;
    const auto envelope = autoupdater::fetchAndVerifyManifest(config, network, *hash, verifier, cancel);
    LAU_REQUIRE(envelope);
    LAU_REQUIRE(!envelope.value().releaseManifestSignatureVerified);
    LAU_REQUIRE(verifier.calls == 0);

    const auto decision = autoupdater::planUpdate(config, envelope.value(), {}, std::nullopt, {});
    LAU_REQUIRE(!decision);
    LAU_REQUIRE(decision.error().code == autoupdater::ErrorCode::SecurityPolicyViolation);
}

} // namespace

void testSecurityBoundaryRegressions() {
    testUntrustedReleaseIdsCannotSelectJournalPaths();
    testArtifactDownloadRejectsRedirectDowngradeAndRestoresTheTarget();
    testFetchedUnsignedManifestCannotAuthorizeDowngrade();
}
