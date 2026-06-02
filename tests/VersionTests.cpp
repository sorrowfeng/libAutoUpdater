#include "TestCommon.h"

#include "libAutoUpdater/Version.h"

void testVersionParsingAndOrdering() {
    auto stable = autoupdater::Version::parse("1.2.3");
    auto alpha = autoupdater::Version::parse("1.2.3-alpha.1");
    auto beta = autoupdater::Version::parse("1.2.3-beta");
    auto build = autoupdater::Version::parse("1.2.3+build.7");

    LAU_REQUIRE(stable);
    LAU_REQUIRE(alpha);
    LAU_REQUIRE(beta);
    LAU_REQUIRE(build);
    LAU_REQUIRE(alpha.value() < beta.value());
    LAU_REQUIRE(beta.value() < stable.value());
    LAU_REQUIRE(stable.value() == build.value());
    LAU_REQUIRE(stable.value().toString() == "1.2.3");
}
