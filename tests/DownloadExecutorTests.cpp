#include "TestCommon.h"

#include "DownloadExecutor.h"
#include "libAutoUpdater/interfaces/IFileSystem.h"
#include "libAutoUpdater/interfaces/IHashProvider.h"
#include "util/PathUtil.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

namespace {

std::filesystem::path uniqueTempDir(const char* tag) {
    const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
    auto path = std::filesystem::temp_directory_path() / "libAutoUpdater-download-test" / (tag + std::to_string(tick));
    std::filesystem::create_directories(path);
    return path;
}

std::string readFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

// Serves a fixed payload and can fail a configurable number of times first.
class FakeDownloadNetwork final : public autoupdater::INetworkClient {
  public:
    std::string payload;
    int failuresBeforeSuccess = 0;
    int attempts = 0;
    std::optional<std::uint64_t> lastResumeOffset;

    autoupdater::Result<std::string> getText(const std::string&, const autoupdater::NetworkOptions&,
                                             autoupdater::CancellationToken&) noexcept override {
        return autoupdater::Result<std::string>::fail({autoupdater::ErrorCode::NetworkError, "not used"});
    }

    autoupdater::Result<autoupdater::DownloadResult>
    downloadToFile(const std::string&, const std::filesystem::path& target, const autoupdater::NetworkOptions&,
                   const std::optional<autoupdater::DownloadResumeInfo>& resume, autoupdater::ProgressCallback,
                   autoupdater::CancellationToken& cancel) noexcept override {
        try {
            ++attempts;
            lastResumeOffset = resume && resume->offset > 0 ? std::optional<std::uint64_t>(resume->offset)
                                                            : std::nullopt;
            if (failuresBeforeSuccess > 0) {
                --failuresBeforeSuccess;
                return autoupdater::Result<autoupdater::DownloadResult>::fail(
                    {autoupdater::ErrorCode::DownloadFailed, "simulated failure"});
            }
            if (cancel.isCancelled()) {
                return autoupdater::Result<autoupdater::DownloadResult>::fail(
                    {autoupdater::ErrorCode::Cancelled, "Operation cancelled"});
            }

            const std::size_t offset = resume ? static_cast<std::size_t>(resume->offset) : 0;
            std::ofstream output(target, std::ios::binary | (offset > 0 ? std::ios::app : std::ios::trunc));
            if (!output) {
                return autoupdater::Result<autoupdater::DownloadResult>::fail(
                    {autoupdater::ErrorCode::DownloadFailed, "open failed"});
            }
            if (offset < payload.size()) {
                output.write(payload.data() + offset, static_cast<std::streamsize>(payload.size() - offset));
            }
            output.close();

            autoupdater::DownloadResult result;
            std::error_code ec;
            result.bytesWritten = static_cast<std::uint64_t>(std::filesystem::file_size(target, ec));
            result.etag = "fake-etag";
            return autoupdater::Result<autoupdater::DownloadResult>::ok(result);
        } catch (...) {
            return autoupdater::Result<autoupdater::DownloadResult>::fail(
                {autoupdater::ErrorCode::DownloadFailed, "unexpected failure"});
        }
    }
};

autoupdater::UpdateDecision makeDecision(const std::filesystem::path& stagingRoot, const std::string& path,
                                         const std::string& payload, const std::string& sha256) {
    autoupdater::UpdateDecision decision;
    autoupdater::PlannedDownload download;
    download.file.path = path;
    download.file.sha256 = sha256;
    download.file.size = payload.size();
    download.url = "mock://server/" + path;
    download.stagingPath = stagingRoot / autoupdater::util::pathFromUtf8(path);
    decision.downloads.push_back(std::move(download));
    return decision;
}

} // namespace

void testDownloadExecutorSucceedsAndStagesFile() {
    const auto root = uniqueTempDir("ok");
    FakeDownloadNetwork network;
    network.payload = "hello world";
    auto hash = autoupdater::createDefaultHashProvider();
    const auto payloadHash = hash->sha256Bytes(network.payload).value();

    autoupdater::Config config;
    config.retry.maxRetries = 0;
    config.retry.retryDelay = std::chrono::milliseconds(1);

    auto fileSystem = autoupdater::createDefaultFileSystem();
    auto decision = makeDecision(root, "bin/app.txt", network.payload, payloadHash);
    autoupdater::CancellationToken cancel;
    auto result = autoupdater::executeDownloads(config, decision, network, *fileSystem, *hash, nullptr, {}, cancel);
    LAU_REQUIRE(result);
    LAU_REQUIRE(network.attempts == 1);
    LAU_REQUIRE(readFile(decision.downloads[0].stagingPath) == network.payload);
    // The temporary download file must not be left behind.
    LAU_REQUIRE(!std::filesystem::exists(decision.downloads[0].stagingPath.string() + ".download"));

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

void testDownloadExecutorRetriesAndReportsAttempts() {
    const auto root = uniqueTempDir("retry");
    FakeDownloadNetwork network;
    network.payload = "retry me";
    network.failuresBeforeSuccess = 2;
    auto hash = autoupdater::createDefaultHashProvider();
    const auto payloadHash = hash->sha256Bytes(network.payload).value();

    autoupdater::Config config;
    config.retry.maxRetries = 3;
    config.retry.retryDelay = std::chrono::milliseconds(1);

    auto fileSystem = autoupdater::createDefaultFileSystem();
    auto decision = makeDecision(root, "bin/app.txt", network.payload, payloadHash);
    autoupdater::CancellationToken cancel;
    auto result = autoupdater::executeDownloads(config, decision, network, *fileSystem, *hash, nullptr, {}, cancel);
    LAU_REQUIRE(result);
    LAU_REQUIRE(network.attempts == 3);

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

void testDownloadExecutorHashMismatchCleansUpAndFails() {
    const auto root = uniqueTempDir("hash");
    FakeDownloadNetwork network;
    network.payload = "tampered content";
    auto hash = autoupdater::createDefaultHashProvider();
    const auto wrongHash = hash->sha256Bytes("different content").value();

    autoupdater::Config config;
    config.retry.maxRetries = 1;
    config.retry.retryDelay = std::chrono::milliseconds(1);

    auto fileSystem = autoupdater::createDefaultFileSystem();
    auto decision = makeDecision(root, "bin/app.txt", network.payload, wrongHash);
    autoupdater::CancellationToken cancel;
    auto result = autoupdater::executeDownloads(config, decision, network, *fileSystem, *hash, nullptr, {}, cancel);
    LAU_REQUIRE(!result);
    LAU_REQUIRE(result.error().code == autoupdater::ErrorCode::HashMismatch);
    LAU_REQUIRE(network.attempts == 2);  // initial + one retry
    // Both the staged file and the temp download file must be gone.
    LAU_REQUIRE(!std::filesystem::exists(decision.downloads[0].stagingPath));
    LAU_REQUIRE(!std::filesystem::exists(decision.downloads[0].stagingPath.string() + ".download"));

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

void testDownloadExecutorCancelStopsDownload() {
    const auto root = uniqueTempDir("cancel");
    FakeDownloadNetwork network;
    network.payload = "cancel me";
    auto hash = autoupdater::createDefaultHashProvider();
    const auto payloadHash = hash->sha256Bytes(network.payload).value();

    autoupdater::Config config;
    config.retry.maxRetries = 5;
    config.retry.retryDelay = std::chrono::seconds(30);  // cancel must cut this short

    auto fileSystem = autoupdater::createDefaultFileSystem();
    auto decision = makeDecision(root, "bin/a.txt", network.payload, payloadHash);
    auto second = makeDecision(root, "bin/b.txt", network.payload, payloadHash);
    decision.downloads.push_back(second.downloads[0]);

    autoupdater::CancellationToken cancel;
    autoupdater::ProgressCallback cancelOnProgress = [&](const autoupdater::Progress&) {
        cancel.cancel();
    };
    const auto start = std::chrono::steady_clock::now();
    auto result =
        autoupdater::executeDownloads(config, decision, network, *fileSystem, *hash, nullptr, cancelOnProgress, cancel);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    LAU_REQUIRE(!result);
    LAU_REQUIRE(result.error().code == autoupdater::ErrorCode::Cancelled);
    LAU_REQUIRE(elapsed < std::chrono::seconds(10));

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

void testDownloadExecutorResumesPartialDownload() {
    const auto root = uniqueTempDir("resume");
    FakeDownloadNetwork network;
    network.payload = "0123456789";
    auto hash = autoupdater::createDefaultHashProvider();
    const auto payloadHash = hash->sha256Bytes(network.payload).value();

    auto decision = makeDecision(root, "bin/app.txt", network.payload, payloadHash);
    // Pre-seed a partial .download file with the first 4 bytes.
    std::filesystem::create_directories(decision.downloads[0].stagingPath.parent_path());
    {
        std::ofstream partial(decision.downloads[0].stagingPath.string() + ".download", std::ios::binary | std::ios::trunc);
        partial << "0123";
    }

    autoupdater::Config config;
    config.retry.maxRetries = 0;
    config.network.enableResume = true;

    auto fileSystem = autoupdater::createDefaultFileSystem();
    autoupdater::CancellationToken cancel;
    auto result = autoupdater::executeDownloads(config, decision, network, *fileSystem, *hash, nullptr, {}, cancel);
    LAU_REQUIRE(result);
    LAU_REQUIRE(network.lastResumeOffset.has_value());
    LAU_REQUIRE(network.lastResumeOffset.value() == 4);
    LAU_REQUIRE(readFile(decision.downloads[0].stagingPath) == network.payload);

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

void testDownloadExecutorRejectsNegativeRetryCount() {
    const auto root = uniqueTempDir("negretry");
    FakeDownloadNetwork network;
    network.payload = "x";
    auto hash = autoupdater::createDefaultHashProvider();

    autoupdater::Config config;
    config.retry.maxRetries = -1;

    auto fileSystem = autoupdater::createDefaultFileSystem();
    auto decision = makeDecision(root, "bin/app.txt", network.payload, hash->sha256Bytes("x").value());
    autoupdater::CancellationToken cancel;
    auto result = autoupdater::executeDownloads(config, decision, network, *fileSystem, *hash, nullptr, {}, cancel);
    LAU_REQUIRE(!result);
    LAU_REQUIRE(result.error().code == autoupdater::ErrorCode::InvalidConfig);
    LAU_REQUIRE(network.attempts == 0);

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}
