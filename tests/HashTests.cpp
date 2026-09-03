#include "TestCommon.h"

#include "libAutoUpdater/interfaces/IHashProvider.h"
#include "libAutoUpdater/interfaces/IRootedFileSystem.h"
#include "util/Sha256.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <ios>
#include <sstream>
#include <streambuf>
#include <string>
#include <vector>

namespace {

class FailingReadBuffer final : public std::streambuf {
  public:
    explicit FailingReadBuffer(std::string prefix) : prefix_(std::move(prefix)) {
        setg(prefix_.data(), prefix_.data(), prefix_.data() + prefix_.size());
    }

  protected:
    int_type underflow() override {
        throw std::ios_base::failure("injected read failure");
    }

  private:
    std::string prefix_;
};

class FailingRootedFile final : public autoupdater::IRootedFile {
  public:
    autoupdater::Result<std::size_t> read(void* output, std::size_t size) noexcept override {
        if (!prefixReturned_) {
            constexpr char prefix[] = "abc";
            const auto count = std::min(size, sizeof(prefix) - 1);
            std::memcpy(output, prefix, count);
            prefixReturned_ = true;
            return autoupdater::Result<std::size_t>::ok(count);
        }
        return autoupdater::Result<std::size_t>::fail(
            {autoupdater::ErrorCode::FileSystemError, "Injected rooted read failure"});
    }
    autoupdater::Result<void> write(const void*, std::size_t) noexcept override {
        return autoupdater::Result<void>::fail({autoupdater::ErrorCode::FileSystemError, "Unexpected rooted write"});
    }
    autoupdater::Result<void> seek(std::uint64_t) noexcept override {
        prefixReturned_ = false;
        return autoupdater::Result<void>::ok();
    }
    autoupdater::Result<void> truncate(std::uint64_t) noexcept override {
        return autoupdater::Result<void>::fail({autoupdater::ErrorCode::FileSystemError, "Unexpected rooted truncate"});
    }
    autoupdater::Result<void> flush() noexcept override {
        return autoupdater::Result<void>::ok();
    }
    autoupdater::Result<autoupdater::RootedFileMetadata> metadata() noexcept override {
        return autoupdater::Result<autoupdater::RootedFileMetadata>::ok({});
    }
    autoupdater::Result<void> setPermissions(std::filesystem::perms) noexcept override {
        return autoupdater::Result<void>::ok();
    }
    autoupdater::Result<void> close() noexcept override {
        return autoupdater::Result<void>::ok();
    }

  private:
    bool prefixReturned_ = false;
};

class ChunkedRootedFile final : public autoupdater::IRootedFile {
  public:
    ChunkedRootedFile(std::string contents, std::vector<std::size_t> chunks)
        : contents_(std::move(contents)), chunks_(std::move(chunks)) {}

    autoupdater::Result<std::size_t> read(void* output, std::size_t size) noexcept override {
        if (offset_ == contents_.size()) {
            return autoupdater::Result<std::size_t>::ok(0);
        }
        const auto chunk = chunks_[chunkIndex_++ % chunks_.size()];
        const auto count = std::min({size, chunk, contents_.size() - offset_});
        std::memcpy(output, contents_.data() + offset_, count);
        offset_ += count;
        return autoupdater::Result<std::size_t>::ok(count);
    }
    autoupdater::Result<void> write(const void*, std::size_t) noexcept override {
        return autoupdater::Result<void>::fail({autoupdater::ErrorCode::FileSystemError, "Unexpected rooted write"});
    }
    autoupdater::Result<void> seek(std::uint64_t offset) noexcept override {
        if (offset > contents_.size()) {
            return autoupdater::Result<void>::fail({autoupdater::ErrorCode::FileSystemError, "Invalid rooted seek"});
        }
        offset_ = static_cast<std::size_t>(offset);
        chunkIndex_ = 0;
        return autoupdater::Result<void>::ok();
    }
    autoupdater::Result<void> truncate(std::uint64_t) noexcept override {
        return autoupdater::Result<void>::fail({autoupdater::ErrorCode::FileSystemError, "Unexpected rooted truncate"});
    }
    autoupdater::Result<void> flush() noexcept override {
        return autoupdater::Result<void>::ok();
    }
    autoupdater::Result<autoupdater::RootedFileMetadata> metadata() noexcept override {
        autoupdater::RootedFileMetadata metadata;
        metadata.size = contents_.size();
        return autoupdater::Result<autoupdater::RootedFileMetadata>::ok(metadata);
    }
    autoupdater::Result<void> setPermissions(std::filesystem::perms) noexcept override {
        return autoupdater::Result<void>::ok();
    }
    autoupdater::Result<void> close() noexcept override {
        return autoupdater::Result<void>::ok();
    }

  private:
    std::string contents_;
    std::vector<std::size_t> chunks_;
    std::size_t offset_ = 0;
    std::size_t chunkIndex_ = 0;
};

} // namespace

void testSha256Provider() {
    auto provider = autoupdater::createDefaultHashProvider();
    auto hash = provider->sha256Bytes("abc");
    LAU_REQUIRE(hash);
    LAU_REQUIRE(hash.value() == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

    struct KnownDigest {
        std::size_t length;
        const char* digest;
    };
    constexpr std::array<KnownDigest, 9> boundaryDigests{{
        {0, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
        {55, "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318"},
        {56, "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a"},
        {63, "7d3e74a05d7db15bce4ad9ec0658ea98e3f06eeecf16b4c6fff2da457ddc2f34"},
        {64, "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb"},
        {65, "635361c48bb9eab14198e76ea8ab7f1a41685d6ad62aa9146d301d4f17eb0ae0"},
        {127, "c57e9278af78fa3cab38667bef4ce29d783787a2f731d4e12200270f0c32320a"},
        {128, "6836cf13bac400e9105071cd6af47084dfacad4e5e302c94bfed24e013afb73e"},
        {129, "c12cb024a2e5551cca0e08fce8f1c5e314555cc3fef6329ee994a3db752166ae"},
    }};
    for (const auto& known : boundaryDigests) {
        LAU_REQUIRE(autoupdater::util::sha256Bytes(std::string(known.length, 'a')) == known.digest);
    }

    LAU_REQUIRE(autoupdater::util::sha256Bytes(std::string_view{}) == boundaryDigests.front().digest);

    ChunkedRootedFile chunked(std::string(129, 'a'), {1, 63, 64, 1});
    const auto chunkedHash = autoupdater::util::sha256RootedFile(chunked);
    LAU_REQUIRE(chunkedHash);
    LAU_REQUIRE(chunkedHash.value() == boundaryDigests.back().digest);

    LAU_REQUIRE(autoupdater::util::sha256Bytes(std::string(1'000'000, 'a')) ==
                "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

void testSha256DistinguishesReadFailureFromEof() {
    std::istringstream complete("abc");
    const auto completeHash = autoupdater::util::sha256Stream(complete);
    LAU_REQUIRE(completeHash);
    LAU_REQUIRE(completeHash.value() == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

    FailingReadBuffer buffer("abc");
    std::istream failing(&buffer);
    const auto failedHash = autoupdater::util::sha256Stream(failing);
    LAU_REQUIRE(!failedHash);
    LAU_REQUIRE(failedHash.error().code == autoupdater::ErrorCode::FileSystemError);

    FailingRootedFile rooted;
    const auto rootedFailure = autoupdater::util::sha256RootedFile(rooted);
    LAU_REQUIRE(!rootedFailure);
    LAU_REQUIRE(rootedFailure.error().code == autoupdater::ErrorCode::FileSystemError);
    LAU_REQUIRE(rootedFailure.error().message.find("Injected rooted read failure") != std::string::npos);

    static std::atomic<std::uint64_t> sequence{0};
    const auto name = "libAutoUpdater-hash-test-" +
                      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
                      std::to_string(sequence.fetch_add(1));
    const auto path = std::filesystem::temp_directory_path() / name;
    const std::string contents(70 * 1024, 'x');
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        LAU_REQUIRE(output.good());
        output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        LAU_REQUIRE(output.good());
    }
    const auto fileHash = autoupdater::util::sha256File(path);
    LAU_REQUIRE(fileHash);
    LAU_REQUIRE(fileHash.value() == autoupdater::util::sha256Bytes(contents));
    std::error_code removeError;
    LAU_REQUIRE(std::filesystem::remove(path, removeError));
    LAU_REQUIRE(!removeError);

    const auto missing = autoupdater::util::sha256File(path);
    LAU_REQUIRE(!missing);
    LAU_REQUIRE(missing.error().code == autoupdater::ErrorCode::FileSystemError);
}
