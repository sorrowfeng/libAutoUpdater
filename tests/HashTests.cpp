#include "TestCommon.h"

#include "libAutoUpdater/interfaces/IHashProvider.h"
#include "libAutoUpdater/interfaces/IRootedFileSystem.h"
#include "util/Sha256.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <ios>
#include <sstream>
#include <streambuf>
#include <string>

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

} // namespace

void testSha256Provider() {
    auto provider = autoupdater::createDefaultHashProvider();
    auto hash = provider->sha256Bytes("abc");
    LAU_REQUIRE(hash);
    LAU_REQUIRE(hash.value() == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
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
