#include "TestCommon.h"

#include "libAutoUpdater/ApplyPlan.h"
#include "libAutoUpdater/Manifest.h"
#include "libAutoUpdater/Version.h"
#include "util/PathUtil.h"
#include "util/UrlUtil.h"

#include <filesystem>
#include <random>
#include <string>

namespace {

std::string randomText(std::mt19937& rng) {
    static constexpr char alphabet[] = "abcdefghijklmnopqrstuvwxyz"
                                       "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                                       "0123456789"
                                       ".-_/{}[],:\"\\ \t\n";

    std::uniform_int_distribution<int> lengthDist(0, 256);
    std::uniform_int_distribution<int> charDist(0, static_cast<int>(sizeof(alphabet) - 2));

    std::string value;
    const auto length = lengthDist(rng);
    value.reserve(static_cast<std::size_t>(length));
    for (int i = 0; i < length; ++i) {
        value.push_back(alphabet[static_cast<std::size_t>(charDist(rng))]);
    }
    return value;
}

} // namespace

void testFuzzSmokeParsersAndPaths() {
    std::mt19937 rng(0xA070D37u);
    const std::filesystem::path root = "install-root";

    for (int i = 0; i < 500; ++i) {
        const auto input = randomText(rng);
        (void)autoupdater::Version::parse(input);
        (void)autoupdater::Manifest::parse(input);
        (void)autoupdater::IndexManifest::parse(input);
        (void)autoupdater::ApplyPlan::parse(input);
        (void)autoupdater::util::safeJoin(root, input);
    }

    LAU_REQUIRE(autoupdater::Version::parse("1.2.3").hasValue());
    LAU_REQUIRE(!autoupdater::util::safeJoin(root, "../escape").hasValue());
    const auto decodedPath =
        autoupdater::util::pathToUtf8(autoupdater::util::fileUrlToPath("file:///tmp/%E4%B8%AD%E6%96%87/file.txt"));
    LAU_REQUIRE(decodedPath.find(u8"中文") != std::string::npos);
}
