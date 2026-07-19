#include "TestCommon.h"

#include "libAutoUpdater/Manifest.h"
#include "util/Rfc3339.h"

#include <fstream>
#include <string>

#ifndef LIBAUTOUPDATER_RFC3339_CORPUS_PATH
#error "LIBAUTOUPDATER_RFC3339_CORPUS_PATH must identify the shared timestamp corpus"
#endif

void testRfc3339ProfileAndNormalization() {
    std::ifstream corpus(LIBAUTOUPDATER_RFC3339_CORPUS_PATH, std::ios::binary);
    LAU_REQUIRE(corpus.good());
    std::string line;
    std::size_t cases = 0;
    while (std::getline(corpus, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty() || line.front() == '#') {
            continue;
        }
        const auto first = line.find('|');
        const auto second = first == std::string::npos ? std::string::npos : line.find('|', first + 1);
        LAU_REQUIRE(first != std::string::npos);
        LAU_REQUIRE(second != std::string::npos);
        const auto expectation = line.substr(0, first);
        LAU_REQUIRE(expectation == "accept" || expectation == "reject");
        const auto timestamp = line.substr(second + 1);
        const auto parsed = autoupdater::util::parseRfc3339(timestamp);
        LAU_REQUIRE((expectation == "accept") == static_cast<bool>(parsed));
        ++cases;
    }
    LAU_REQUIRE(cases >= 30);

    const auto epoch = autoupdater::util::parseRfc3339("1970-01-01T00:00:00Z");
    LAU_REQUIRE(epoch);
    LAU_REQUIRE(epoch.value().unixSeconds == 0);
    LAU_REQUIRE(epoch.value().nanoseconds == 0);

    const auto beforeEpoch = autoupdater::util::parseRfc3339("1969-12-31T23:59:59.5Z");
    LAU_REQUIRE(beforeEpoch);
    LAU_REQUIRE(beforeEpoch.value().unixSeconds == -1);
    LAU_REQUIRE(beforeEpoch.value().nanoseconds == 500000000U);

    const auto offset = autoupdater::util::parseRfc3339("2026-07-01T01:00:00.123456789+02:00");
    const auto utc = autoupdater::util::parseRfc3339("2026-06-30T23:00:00.123456789Z");
    LAU_REQUIRE(offset);
    LAU_REQUIRE(utc);
    LAU_REQUIRE(offset.value() == utc.value());

    const auto negativeOffsetAcrossDay = autoupdater::util::parseRfc3339("2026-07-19T23:30:00-01:00");
    const auto nextUtcDay = autoupdater::util::parseRfc3339("2026-07-20T00:30:00Z");
    LAU_REQUIRE(negativeOffsetAcrossDay);
    LAU_REQUIRE(nextUtcDay);
    LAU_REQUIRE(negativeOffsetAcrossDay.value() == nextUtcDay.value());

    const auto current = autoupdater::util::currentUtcInstant();
    LAU_REQUIRE(current);
    LAU_REQUIRE(current.value().nanoseconds < 1000000000U);
}

void testMetadataTimestampsUseStrictProfile() {
    const auto manifest = autoupdater::Manifest::parse(R"json({
      "schemaVersion": 1,
      "version": "1.2.3",
      "releaseDate": "2026-07-19T12:34:56Z",
      "publishedAt": "2026-07-19T14:34:56+02:00",
      "expiresAt": "2026-08-19T12:34:56.123456789Z"
    })json");
    LAU_REQUIRE(manifest);

    const auto index = autoupdater::IndexManifest::parse(R"json({
      "schemaVersion": 1,
      "generatedAt": "2026-07-19T12:34:56-04:00",
      "targets": []
    })json");
    LAU_REQUIRE(index);

    for (const auto* field : {"releaseDate", "publishedAt", "expiresAt"}) {
        const std::string invalid =
            std::string("{\"schemaVersion\":1,\"version\":\"1.2.3\",\"") + field + "\":\"2026-02-30T00:00:00Z\"}";
        const auto rejected = autoupdater::Manifest::parse(invalid);
        LAU_REQUIRE(!rejected);
        LAU_REQUIRE(rejected.error().code == autoupdater::ErrorCode::ManifestParseFailed);
    }

    LAU_REQUIRE(!autoupdater::Manifest::parse(R"json({"schemaVersion":1,"version":"1.2.3","expiresAt":""})json"));
    LAU_REQUIRE(
        !autoupdater::IndexManifest::parse(R"json({"schemaVersion":1,"generatedAt":"not-a-time","targets":[]})json"));
}
