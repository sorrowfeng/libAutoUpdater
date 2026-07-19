#include "TestCommon.h"

#include "NetworkRequest.h"
#include "NetworkTestCommon.h"
#include "UrlPolicy.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace {

class MemoryRootedFile final : public autoupdater::IRootedFile {
  public:
    explicit MemoryRootedFile(std::string contents = {}) : contents_(std::move(contents)), offset_(contents_.size()) {}

    autoupdater::Result<std::size_t> read(void* buffer, std::size_t size) noexcept override {
        const auto available = contents_.size() - std::min(offset_, contents_.size());
        const auto count = std::min(size, available);
        if (count != 0) {
            std::memcpy(buffer, contents_.data() + offset_, count);
            offset_ += count;
        }
        return autoupdater::Result<std::size_t>::ok(count);
    }

    autoupdater::Result<void> write(const void* data, std::size_t size) noexcept override {
        try {
            if (offset_ > contents_.size()) {
                contents_.resize(offset_, '\0');
            }
            if (size > contents_.max_size() - offset_) {
                return autoupdater::Result<void>::fail(
                    {autoupdater::ErrorCode::FileSystemError, "In-memory file size overflow"});
            }
            if (offset_ + size > contents_.size()) {
                contents_.resize(offset_ + size);
            }
            if (size != 0) {
                std::memcpy(contents_.data() + offset_, data, size);
                offset_ += size;
            }
            return autoupdater::Result<void>::ok();
        } catch (...) {
            return autoupdater::Result<void>::fail(
                {autoupdater::ErrorCode::FileSystemError, "In-memory file write failed"});
        }
    }

    autoupdater::Result<void> seek(std::uint64_t offset) noexcept override {
        if (offset > contents_.max_size()) {
            return autoupdater::Result<void>::fail(
                {autoupdater::ErrorCode::FileSystemError, "In-memory file seek overflow"});
        }
        offset_ = static_cast<std::size_t>(offset);
        return autoupdater::Result<void>::ok();
    }

    autoupdater::Result<void> truncate(std::uint64_t size) noexcept override {
        if (size > contents_.max_size()) {
            return autoupdater::Result<void>::fail(
                {autoupdater::ErrorCode::FileSystemError, "In-memory file truncate overflow"});
        }
        try {
            contents_.resize(static_cast<std::size_t>(size));
            offset_ = std::min(offset_, contents_.size());
            return autoupdater::Result<void>::ok();
        } catch (...) {
            return autoupdater::Result<void>::fail(
                {autoupdater::ErrorCode::FileSystemError, "In-memory file truncate failed"});
        }
    }

    autoupdater::Result<void> flush() noexcept override {
        return autoupdater::Result<void>::ok();
    }

    autoupdater::Result<autoupdater::RootedFileMetadata> metadata() noexcept override {
        autoupdater::RootedFileMetadata metadata;
        metadata.size = contents_.size();
        metadata.permissions = permissions_;
        metadata.identity = "memory-file";
        return autoupdater::Result<autoupdater::RootedFileMetadata>::ok(std::move(metadata));
    }

    autoupdater::Result<void> setPermissions(std::filesystem::perms permissions) noexcept override {
        permissions_ = permissions;
        return autoupdater::Result<void>::ok();
    }

    const std::string& contents() const noexcept {
        return contents_;
    }

  private:
    std::string contents_;
    std::size_t offset_ = 0;
    std::filesystem::perms permissions_ = std::filesystem::perms::owner_read;
};

autoupdater::UrlPolicy makePolicy(std::vector<std::string> scopes) {
    autoupdater::Config config;
    config.manifestUrl = "https://updates.example.test/releases/start.json";
    config.security.allowedBaseUrls = std::move(scopes);
    auto policy = autoupdater::UrlPolicy::fromConfig(config);
    LAU_REQUIRE(policy);
    return std::move(policy.value());
}

autoupdater::UrlPolicy sameOriginPolicy() {
    return makePolicy({"https://updates.example.test/releases/"});
}

autoupdater::test::ScriptedResponse downloadResponse(std::string bytes, int statusCode = 200) {
    autoupdater::test::ScriptedResponse response;
    response.statusCode = statusCode;
    response.downloadedBytes = std::move(bytes);
    return response;
}

} // namespace

void testNetworkRequestRejectsInitialUrlBeforeTransport() {
    auto policy = sameOriginPolicy();
    autoupdater::test::ScriptedNetworkClient network;
    autoupdater::NetworkOptions options;
    autoupdater::CancellationToken cancel;

    auto result = autoupdater::fetchTextWithRedirects("https://evil.example.test/releases/start.json", options, policy,
                                                      network, cancel);
    LAU_REQUIRE(!result);
    LAU_REQUIRE(result.error().code == autoupdater::ErrorCode::SecurityPolicyViolation);
    LAU_REQUIRE(network.textRequests.empty());
}

void testNetworkRequestFollowsAllAllowedRedirectStatuses() {
    const std::string start = "https://updates.example.test/releases/channel/latest.json";
    const std::string final = "https://updates.example.test/releases/stable/manifest.json";
    for (const int statusCode : {301, 302, 303, 307, 308}) {
        auto policy = sameOriginPolicy();
        autoupdater::test::ScriptedNetworkClient network;
        auto redirect = autoupdater::test::redirectResponse(statusCode, "  ../stable/manifest.json\t");
        network.queueText(start, std::move(redirect));
        network.queueText(final, autoupdater::test::textResponse("manifest-body"));
        autoupdater::NetworkOptions options;
        autoupdater::CancellationToken cancel;

        auto result = autoupdater::fetchTextWithRedirects(start, options, policy, network, cancel);
        LAU_REQUIRE(result);
        LAU_REQUIRE(result.value().body == "manifest-body");
        LAU_REQUIRE(result.value().effectiveUrl == final);
        LAU_REQUIRE(network.textRequests == std::vector<std::string>({start, final}));
    }
}

void testNetworkRequestEnforcesRedirectOriginsAndProtocols() {
    const std::string start = "https://updates.example.test/releases/start.json";
    const std::string crossOrigin = "https://cdn.example.test/releases/final.json";

    {
        auto policy = makePolicy({"https://updates.example.test/releases/", "https://cdn.example.test/releases/"});
        autoupdater::test::ScriptedNetworkClient network;
        network.queueText(start, autoupdater::test::redirectResponse(302, crossOrigin));
        network.queueText(crossOrigin, autoupdater::test::textResponse("allowed-cross-origin"));
        autoupdater::NetworkOptions options;
        autoupdater::CancellationToken cancel;
        auto result = autoupdater::fetchTextWithRedirects(start, options, policy, network, cancel);
        LAU_REQUIRE(result);
        LAU_REQUIRE(result.value().body == "allowed-cross-origin");
        LAU_REQUIRE(network.textRequests == std::vector<std::string>({start, crossOrigin}));
    }

    {
        auto policy = sameOriginPolicy();
        autoupdater::test::ScriptedNetworkClient network;
        network.queueText(start, autoupdater::test::redirectResponse(302, crossOrigin));
        autoupdater::NetworkOptions options;
        autoupdater::CancellationToken cancel;
        auto result = autoupdater::fetchTextWithRedirects(start, options, policy, network, cancel);
        LAU_REQUIRE(!result);
        LAU_REQUIRE(result.error().code == autoupdater::ErrorCode::SecurityPolicyViolation);
        LAU_REQUIRE(network.textRequests == std::vector<std::string>({start}));
    }

    {
        const std::string middle = "https://updates.example.test/releases/middle.json";
        auto policy = sameOriginPolicy();
        autoupdater::test::ScriptedNetworkClient network;
        network.queueText(start, autoupdater::test::redirectResponse(302, middle));
        network.queueText(middle, autoupdater::test::redirectResponse(307, crossOrigin));
        autoupdater::NetworkOptions options;
        autoupdater::CancellationToken cancel;
        auto result = autoupdater::fetchTextWithRedirects(start, options, policy, network, cancel);
        LAU_REQUIRE(!result);
        LAU_REQUIRE(result.error().code == autoupdater::ErrorCode::SecurityPolicyViolation);
        LAU_REQUIRE(network.textRequests == std::vector<std::string>({start, middle}));
    }

    {
        auto policy = makePolicy({"https://updates.example.test/releases/", "http://updates.example.test/releases/"});
        autoupdater::test::ScriptedNetworkClient network;
        network.queueText(start,
                          autoupdater::test::redirectResponse(302, "http://updates.example.test/releases/final.json"));
        autoupdater::NetworkOptions options;
        autoupdater::CancellationToken cancel;
        auto result = autoupdater::fetchTextWithRedirects(start, options, policy, network, cancel);
        LAU_REQUIRE(!result);
        LAU_REQUIRE(result.error().code == autoupdater::ErrorCode::SecurityPolicyViolation);
        LAU_REQUIRE(network.textRequests == std::vector<std::string>({start}));
    }

    {
        auto policy = sameOriginPolicy();
        autoupdater::test::ScriptedNetworkClient network;
#ifdef _WIN32
        const std::string localTarget = "file:///C:/updates/manifest.json";
#else
        const std::string localTarget = "file:///tmp/updates/manifest.json";
#endif
        network.queueText(start, autoupdater::test::redirectResponse(302, localTarget));
        autoupdater::NetworkOptions options;
        autoupdater::CancellationToken cancel;
        auto result = autoupdater::fetchTextWithRedirects(start, options, policy, network, cancel);
        LAU_REQUIRE(!result);
        LAU_REQUIRE(result.error().code == autoupdater::ErrorCode::SecurityPolicyViolation);
        LAU_REQUIRE(network.textRequests == std::vector<std::string>({start}));
    }
}

void testNetworkRequestRequiresOneLocationHeader() {
    const std::string start = "https://updates.example.test/releases/start.json";
    std::vector<autoupdater::test::ScriptedResponse> invalidRedirects;

    autoupdater::test::ScriptedResponse missing;
    missing.statusCode = 302;
    invalidRedirects.push_back(std::move(missing));

    autoupdater::test::ScriptedResponse empty;
    empty.statusCode = 302;
    empty.headers.push_back({"Location", " \t "});
    invalidRedirects.push_back(std::move(empty));

    autoupdater::test::ScriptedResponse duplicate;
    duplicate.statusCode = 302;
    duplicate.headers.push_back({"Location", "one.json"});
    duplicate.headers.push_back({"lOcAtIoN", "two.json"});
    invalidRedirects.push_back(std::move(duplicate));

    for (auto& response : invalidRedirects) {
        auto policy = sameOriginPolicy();
        autoupdater::test::ScriptedNetworkClient network;
        network.queueText(start, std::move(response));
        autoupdater::NetworkOptions options;
        autoupdater::CancellationToken cancel;
        auto result = autoupdater::fetchTextWithRedirects(start, options, policy, network, cancel);
        LAU_REQUIRE(!result);
        LAU_REQUIRE(result.error().code == autoupdater::ErrorCode::SecurityPolicyViolation);
        LAU_REQUIRE(network.textRequests == std::vector<std::string>({start}));
    }
}

void testNetworkRequestDetectsLoopsAndRedirectLimit() {
    const std::string start = "https://updates.example.test/releases/start.json";
    const std::string middle = "https://updates.example.test/releases/middle.json";

    {
        auto policy = sameOriginPolicy();
        autoupdater::test::ScriptedNetworkClient network;
        network.queueText(start, autoupdater::test::redirectResponse(302, middle));
        network.queueText(middle, autoupdater::test::redirectResponse(302, start));
        autoupdater::NetworkOptions options;
        autoupdater::CancellationToken cancel;
        auto result = autoupdater::fetchTextWithRedirects(start, options, policy, network, cancel);
        LAU_REQUIRE(!result);
        LAU_REQUIRE(result.error().code == autoupdater::ErrorCode::SecurityPolicyViolation);
        LAU_REQUIRE(network.textRequests == std::vector<std::string>({start, middle}));
    }

    {
        auto policy = sameOriginPolicy();
        autoupdater::test::ScriptedNetworkClient network;
        network.queueText(
            start, autoupdater::test::redirectResponse(302, "HTTPS://UPDATES.EXAMPLE.TEST:443/releases/./start.json"));
        autoupdater::NetworkOptions options;
        autoupdater::CancellationToken cancel;
        auto result = autoupdater::fetchTextWithRedirects(start, options, policy, network, cancel);
        LAU_REQUIRE(!result);
        LAU_REQUIRE(result.error().code == autoupdater::ErrorCode::SecurityPolicyViolation);
        LAU_REQUIRE(network.textRequests == std::vector<std::string>({start}));
    }

    {
        const std::string final = "https://updates.example.test/releases/final.json";
        auto policy = sameOriginPolicy();
        autoupdater::test::ScriptedNetworkClient network;
        network.queueText(start, autoupdater::test::redirectResponse(302, middle));
        network.queueText(middle, autoupdater::test::redirectResponse(302, final));
        autoupdater::NetworkOptions options;
        options.maxRedirects = 1;
        autoupdater::CancellationToken cancel;
        auto result = autoupdater::fetchTextWithRedirects(start, options, policy, network, cancel);
        LAU_REQUIRE(!result);
        LAU_REQUIRE(result.error().code == autoupdater::ErrorCode::SecurityPolicyViolation);
        LAU_REQUIRE(network.textRequests == std::vector<std::string>({start, middle}));
    }
}

void testNetworkRequestRejectsForgedEffectiveUrl() {
    const std::string start = "https://updates.example.test/releases/start.json";
    const std::string forged = "https://updates.example.test/releases/forged.json";

    {
        auto policy = sameOriginPolicy();
        autoupdater::test::ScriptedNetworkClient network;
        auto response = autoupdater::test::textResponse("forged");
        response.effectiveUrl = forged;
        network.queueText(start, std::move(response));
        autoupdater::NetworkOptions options;
        autoupdater::CancellationToken cancel;
        auto result = autoupdater::fetchTextWithRedirects(start, options, policy, network, cancel);
        LAU_REQUIRE(!result);
        LAU_REQUIRE(result.error().code == autoupdater::ErrorCode::SecurityPolicyViolation);
        LAU_REQUIRE(network.textRequests == std::vector<std::string>({start}));
    }

    {
        auto policy = sameOriginPolicy();
        autoupdater::test::ScriptedNetworkClient network;
        auto response = autoupdater::test::textResponse("equivalent");
        response.effectiveUrl = "HTTPS://UPDATES.EXAMPLE.TEST:443/releases/./start.json";
        network.queueText(start, std::move(response));
        autoupdater::NetworkOptions options;
        autoupdater::CancellationToken cancel;
        auto result = autoupdater::fetchTextWithRedirects(start, options, policy, network, cancel);
        LAU_REQUIRE(result);
        LAU_REQUIRE(result.value().body == "equivalent");
    }
}

void testNetworkDownloadRestoresBodiesWrittenByRedirects() {
    const std::string start = "https://updates.example.test/releases/start.bin";
    const std::string final = "https://updates.example.test/releases/final.bin";

    {
        auto policy = sameOriginPolicy();
        autoupdater::test::ScriptedNetworkClient network;
        network.queueDownload(start, autoupdater::test::redirectResponse(302, "final.bin"));
        network.queueDownload(final, downloadResponse("payload"));
        MemoryRootedFile target;
        autoupdater::NetworkOptions options;
        autoupdater::CancellationToken cancel;
        auto result =
            autoupdater::downloadWithRedirects(start, target, options, policy, network, std::nullopt, {}, cancel);
        LAU_REQUIRE(result);
        LAU_REQUIRE(target.contents() == "payload");
        LAU_REQUIRE(network.downloadRequests == std::vector<std::string>({start, final}));
        LAU_REQUIRE(network.downloadTargetSizesAfterResponse == std::vector<std::uint64_t>({0, 7}));
    }

    {
        auto policy = sameOriginPolicy();
        autoupdater::test::ScriptedNetworkClient network;
        auto redirect = autoupdater::test::redirectResponse(302, "final.bin");
        redirect.downloadedBytes = "malicious-redirect-body";
        network.queueDownload(start, std::move(redirect));
        network.queueDownload(final, downloadResponse("payload"));
        MemoryRootedFile target;
        autoupdater::NetworkOptions options;
        autoupdater::CancellationToken cancel;
        auto result =
            autoupdater::downloadWithRedirects(start, target, options, policy, network, std::nullopt, {}, cancel);
        LAU_REQUIRE(result);
        LAU_REQUIRE(target.contents() == "payload");
    }

    {
        auto policy = sameOriginPolicy();
        autoupdater::test::ScriptedNetworkClient network;
        auto forged = downloadResponse("malicious-success-body");
        forged.effectiveUrl = final;
        network.queueDownload(start, std::move(forged));
        MemoryRootedFile target("seed");
        autoupdater::NetworkOptions options;
        autoupdater::CancellationToken cancel;
        auto result =
            autoupdater::downloadWithRedirects(start, target, options, policy, network, std::nullopt, {}, cancel);
        LAU_REQUIRE(!result);
        LAU_REQUIRE(result.error().code == autoupdater::ErrorCode::SecurityPolicyViolation);
        LAU_REQUIRE(target.contents() == "seed");
    }
}

void testNetworkDownloadResetsResumeAndRejectsUnexpectedSuccessStatus() {
    const std::string start = "https://updates.example.test/releases/start.bin";
    const std::string final = "https://updates.example.test/releases/final.bin";

    {
        auto policy = sameOriginPolicy();
        autoupdater::test::ScriptedNetworkClient network;
        network.queueDownload(start, autoupdater::test::redirectResponse(302, "final.bin"));
        network.queueDownload(final, downloadResponse("complete-payload"));
        MemoryRootedFile target("partial");
        autoupdater::NetworkOptions options;
        autoupdater::DownloadResumeInfo resume;
        resume.offset = 7;
        resume.etag = "old-validator";
        autoupdater::CancellationToken cancel;

        auto result = autoupdater::downloadWithRedirects(start, target, options, policy, network, resume, {}, cancel);
        LAU_REQUIRE(result);
        LAU_REQUIRE(target.contents() == "complete-payload");
        LAU_REQUIRE(network.downloadRequests == std::vector<std::string>({start, final}));
        LAU_REQUIRE(network.downloadResumes.size() == 2);
        LAU_REQUIRE(network.downloadResumes[0]);
        LAU_REQUIRE(network.downloadResumes[0]->offset == 7);
        LAU_REQUIRE(network.downloadResumes[0]->etag == "old-validator");
        LAU_REQUIRE(!network.downloadResumes[1]);
    }

    for (const int statusCode : {201, 204}) {
        auto policy = sameOriginPolicy();
        autoupdater::test::ScriptedNetworkClient network;
        network.queueDownload(start, downloadResponse("unexpected-body", statusCode));
        MemoryRootedFile target("seed");
        autoupdater::NetworkOptions options;
        autoupdater::CancellationToken cancel;

        auto result =
            autoupdater::downloadWithRedirects(start, target, options, policy, network, std::nullopt, {}, cancel);
        LAU_REQUIRE(!result);
        LAU_REQUIRE(result.error().code == autoupdater::ErrorCode::DownloadFailed);
        LAU_REQUIRE(target.contents() == "seed");
    }
}

void testNetworkDownloadRestartsIgnoredHttpResume() {
    const std::string url = "https://updates.example.test/releases/payload.bin";
    auto policy = sameOriginPolicy();
    autoupdater::test::ScriptedNetworkClient network;
    network.queueDownload(url, downloadResponse({}, 200));
    network.queueDownload(url, downloadResponse("complete-payload", 200));
    MemoryRootedFile target("partial");
    autoupdater::NetworkOptions options;
    autoupdater::DownloadResumeInfo resume;
    resume.offset = 7;
    resume.etag = "old-validator";
    autoupdater::CancellationToken cancel;

    auto result = autoupdater::downloadWithRedirects(url, target, options, policy, network, resume, {}, cancel);
    LAU_REQUIRE(result);
    LAU_REQUIRE(target.contents() == "complete-payload");
    LAU_REQUIRE(network.downloadRequests == std::vector<std::string>({url, url}));
    LAU_REQUIRE(network.downloadResumes.size() == 2);
    LAU_REQUIRE(network.downloadResumes[0]);
    LAU_REQUIRE(network.downloadResumes[0]->offset == 7);
    LAU_REQUIRE(network.downloadResumes[0]->etag == "old-validator");
    LAU_REQUIRE(!network.downloadResumes[1]);
    LAU_REQUIRE(network.downloadTargetSizesAfterResponse == std::vector<std::uint64_t>({7, 16}));
}

void testNetworkDownloadPreservesLocalFileResume() {
#ifdef _WIN32
    const std::string manifestUrl = "file:///C:/updates/manifest.json";
    const std::string artifactUrl = "file:///C:/updates/payload.bin";
#else
    const std::string manifestUrl = "file:///tmp/updates/manifest.json";
    const std::string artifactUrl = "file:///tmp/updates/payload.bin";
#endif
    autoupdater::Config config;
    config.manifestUrl = manifestUrl;
    config.security.allowLocalFileUrls = true;
    auto policy = autoupdater::UrlPolicy::fromConfig(config);
    LAU_REQUIRE(policy);

    autoupdater::test::ScriptedNetworkClient network;
    network.queueDownload(artifactUrl, downloadResponse("-complete"));
    MemoryRootedFile target("partial");
    autoupdater::NetworkOptions options;
    autoupdater::DownloadResumeInfo resume;
    resume.offset = 7;
    autoupdater::CancellationToken cancel;

    auto result =
        autoupdater::downloadWithRedirects(artifactUrl, target, options, policy.value(), network, resume, {}, cancel);
    LAU_REQUIRE(result);
    LAU_REQUIRE(target.contents() == "partial-complete");
    LAU_REQUIRE(network.downloadRequests == std::vector<std::string>({artifactUrl}));
    LAU_REQUIRE(network.downloadResumes.size() == 1);
    LAU_REQUIRE(network.downloadResumes[0]);
    LAU_REQUIRE(network.downloadResumes[0]->offset == 7);
}
