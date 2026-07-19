#include "TestCommon.h"

#include "libAutoUpdater/Version.h"

#include <clocale>
#include <string>

void testVersionParsingAndOrdering() {
    auto stable = autoupdater::Version::parse("1.2.3");
    auto alpha = autoupdater::Version::parse("1.2.3-alpha.1");
    auto beta = autoupdater::Version::parse("1.2.3-beta");
    auto build = autoupdater::Version::parse("1.2.3+build.7");
    auto numericBuild = autoupdater::Version::parse("1.2.3+build.007");

    LAU_REQUIRE(stable);
    LAU_REQUIRE(alpha);
    LAU_REQUIRE(beta);
    LAU_REQUIRE(build);
    LAU_REQUIRE(numericBuild);
    LAU_REQUIRE(!autoupdater::Version::parse("1.2.3-alpha.01"));
    LAU_REQUIRE(alpha.value() < beta.value());
    LAU_REQUIRE(beta.value() < stable.value());
    LAU_REQUIRE(stable.value() == build.value());
    LAU_REQUIRE(stable.value().toString() == "1.2.3");

    const std::string nonAsciiIdentifier = "\xc2\xaa";
    LAU_REQUIRE(!autoupdater::Version::parse("1.2.3-" + nonAsciiIdentifier));
    LAU_REQUIRE(!autoupdater::Version::parse("1.2.3+" + nonAsciiIdentifier));
    LAU_REQUIRE(!autoupdater::Version::parse(nonAsciiIdentifier + ".2.3"));

#ifdef _WIN32
    const char* currentLocale = std::setlocale(LC_CTYPE, nullptr);
    const std::string previousLocale = currentLocale != nullptr ? currentLocale : "C";
    bool rejectsUnderSingleByteLocale = true;
    if (std::setlocale(LC_CTYPE, ".1252") != nullptr) {
        rejectsUnderSingleByteLocale = !autoupdater::Version::parse("1.2.3-" + nonAsciiIdentifier);
    }
    LAU_REQUIRE(std::setlocale(LC_CTYPE, previousLocale.c_str()) != nullptr);
    LAU_REQUIRE(rejectsUnderSingleByteLocale);
#endif
}
