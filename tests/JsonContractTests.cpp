#include "TestCommon.h"

#include "ApplyJournal.h"
#include "ApplyTransactionReceipt.h"
#include "libAutoUpdater/ApplyPlan.h"
#include "libAutoUpdater/Manifest.h"
#include "libAutoUpdater/interfaces/IFileSystem.h"
#include "libAutoUpdater/interfaces/IStateStore.h"
#include "util/Json.h"
#include "util/Rfc3339.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <locale>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using autoupdater::util::Json;

constexpr std::uint64_t kBeyondDoubleInteger = UINT64_C(9007199254740993);

Json parseJson(const std::string& text) {
    auto parsed = Json::parse(text, autoupdater::JsonResourceLimits{});
    if (!parsed) {
        throw std::runtime_error("Expected valid JSON: " + parsed.error().message);
    }
    return std::move(parsed.value());
}

void requireInvalidJson(const std::string& text) {
    LAU_REQUIRE(!Json::parse(text, autoupdater::JsonResourceLimits{}));
}

std::string bytes(std::initializer_list<unsigned int> values) {
    std::string result;
    result.reserve(values.size());
    for (const auto value : values) {
        result.push_back(static_cast<char>(value));
    }
    return result;
}

unsigned int hexNibble(char value) {
    if (value >= '0' && value <= '9') {
        return static_cast<unsigned int>(value - '0');
    }
    if (value >= 'a' && value <= 'f') {
        return static_cast<unsigned int>(value - 'a' + 10);
    }
    throw std::runtime_error("Invalid hexadecimal byte in JSON conformance corpus");
}

std::string decodeHex(const std::string& encoded) {
    if (encoded.size() % 2 != 0) {
        throw std::runtime_error("Odd-length hexadecimal JSON conformance payload");
    }
    std::string decoded;
    decoded.reserve(encoded.size() / 2);
    for (std::size_t index = 0; index < encoded.size(); index += 2) {
        decoded.push_back(static_cast<char>((hexNibble(encoded[index]) << 4U) | hexNibble(encoded[index + 1])));
    }
    return decoded;
}

void verifySharedJsonCorpus() {
#ifndef LIBAUTOUPDATER_JSON_CORPUS_PATH
#error "LIBAUTOUPDATER_JSON_CORPUS_PATH must name the shared JSON conformance corpus"
#endif
    std::ifstream input(LIBAUTOUPDATER_JSON_CORPUS_PATH, std::ios::binary);
    if (!input) {
        throw std::runtime_error("Cannot open shared JSON conformance corpus");
    }

    std::string line;
    std::size_t lineNumber = 0;
    while (std::getline(input, line)) {
        ++lineNumber;
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty() || line.front() == '#') {
            continue;
        }
        const auto first = line.find('|');
        const auto second = first == std::string::npos ? std::string::npos : line.find('|', first + 1);
        if (first == std::string::npos || second == std::string::npos || line.find('|', second + 1) != std::string::npos) {
            throw std::runtime_error("Malformed JSON conformance corpus line " + std::to_string(lineNumber));
        }
        const auto expectation = line.substr(0, first);
        const auto name = line.substr(first + 1, second - first - 1);
        if ((expectation != "accept" && expectation != "reject") || name.empty()) {
            throw std::runtime_error("Invalid JSON conformance corpus metadata on line " +
                                     std::to_string(lineNumber));
        }

        const auto parsed = Json::parse(decodeHex(line.substr(second + 1)), autoupdater::JsonResourceLimits{});
        const bool expectedValid = expectation == "accept";
        if (static_cast<bool>(parsed) != expectedValid || (parsed && !parsed.value().isObject())) {
            throw std::runtime_error("JSON conformance mismatch for case '" + name + "'");
        }
    }
}

class CommaDecimalPoint final : public std::numpunct<char> {
  protected:
    char do_decimal_point() const override {
        return ',';
    }
};

class GlobalLocaleGuard final {
  public:
    GlobalLocaleGuard() : previous_(std::locale()) {
        std::locale::global(std::locale(previous_, new CommaDecimalPoint));
    }

    GlobalLocaleGuard(const GlobalLocaleGuard&) = delete;
    GlobalLocaleGuard& operator=(const GlobalLocaleGuard&) = delete;

    ~GlobalLocaleGuard() {
        std::locale::global(previous_);
    }

  private:
    std::locale previous_;
};

std::string sha(char value) {
    return std::string(64, value);
}

autoupdater::ResourceLimits largeArtifactLimits() {
    autoupdater::ResourceLimits limits;
    limits.maxArtifactBytes = std::numeric_limits<std::uint64_t>::max();
    limits.maxTotalArtifactBytes = std::numeric_limits<std::uint64_t>::max();
    return limits;
}

std::string validManifest(std::uint64_t size = kBeyondDoubleInteger) {
    return std::string(R"json({"schemaVersion":1,"version":"1.2.3","files":[{"path":"app.bin","sha256":")json") +
           sha('a') + R"json(","size":)json" + std::to_string(size) + "}]}";
}

std::string validApplyPlan(std::uint64_t size = kBeyondDoubleInteger) {
    return std::string(
               R"json({"schemaVersion":2,"intent":"install","fromVersion":"1.0.0","toVersion":"1.2.3","manifestSha256":")json") +
           sha('b') +
           R"json(","installDir":"/install","stagingDir":"/staging","backupDir":"/backup","restartCommand":[],"operations":[{"type":"replace","source":"app.bin","target":"app.bin","sha256":")json" +
           sha('a') + R"json(","size":)json" + std::to_string(size) + "}]}";
}

std::string addField(const std::string& text, const std::string& key, Json value) {
    auto root = parseJson(text).asObject();
    root.emplace(key, std::move(value));
    return Json(std::move(root)).stringify();
}

std::string replaceField(const std::string& text, const std::string& key, Json value) {
    auto root = parseJson(text).asObject();
    root[key] = std::move(value);
    return Json(std::move(root)).stringify();
}

class TemporaryDirectory final {
  public:
    TemporaryDirectory() {
        static std::atomic<std::uint64_t> sequence{0};
        path_ = std::filesystem::temp_directory_path() / "libAutoUpdater-json-contract" /
                std::to_string(sequence.fetch_add(1));
        std::error_code error;
        std::filesystem::remove_all(path_, error);
        error.clear();
        std::filesystem::create_directories(path_, error);
        if (error) {
            throw std::runtime_error("Failed to create JSON contract test directory");
        }
    }

    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    const std::filesystem::path& path() const noexcept {
        return path_;
    }

  private:
    std::filesystem::path path_;
};

void writeFile(const std::filesystem::path& path, const std::string& contents) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    LAU_REQUIRE(output);
    output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    LAU_REQUIRE(output);
}

} // namespace

void testJsonRfc8259SyntaxAndUnicode() {
    verifySharedJsonCorpus();

    const auto unicode = parseJson(R"json("\u0041\u00df\u6771\ud834\udd1e")json");
    LAU_REQUIRE(unicode.isString());
    LAU_REQUIRE(unicode.asString() == bytes({0x41, 0xc3, 0x9f, 0xe6, 0x9d, 0xb1, 0xf0, 0x9d, 0x84, 0x9e}));

    const auto nul = parseJson(R"json("\u0000")json");
    LAU_REQUIRE(nul.asString().size() == 1);
    LAU_REQUIRE(nul.asString().front() == '\0');
    LAU_REQUIRE(nul.stringify() == R"json("\u0000")json");

    for (const auto& text : std::vector<std::string>{
             R"json({"a":0,"a":1})json",
             R"json({"a":0,"\u0061":1})json",
             R"json("\ud800")json",
             R"json("\udc00")json",
             R"json("\ud800\u0041")json",
             R"json("\udc00\ud800")json",
             std::string("\"bad\0value\"", 11),
             std::string("\f0", 2),
             std::string("\v0", 2),
             std::string("\"line\nfeed\"")}) {
        requireInvalidJson(text);
    }

    for (const auto& malformed : std::vector<std::string>{
             bytes({0x80}),
             bytes({0xc0, 0x80}),
             bytes({0xe2, 0x82}),
             bytes({0xed, 0xa0, 0x80}),
             bytes({0xf4, 0x90, 0x80, 0x80}),
             bytes({0xf8, 0x88, 0x80, 0x80, 0x80})}) {
        requireInvalidJson(std::string("\"") + malformed + "\"");
    }

    const auto rawUtf8 = std::string("\"") + bytes({0xe6, 0x9d, 0xb1}) + "\"";
    LAU_REQUIRE(parseJson(rawUtf8).asString() == bytes({0xe6, 0x9d, 0xb1}));
}

void testJsonExactNumericContract() {
    const auto minimum = parseJson("-9223372036854775808");
    LAU_REQUIRE(minimum.isSignedInteger());
    LAU_REQUIRE(minimum.asInt64() == std::numeric_limits<std::int64_t>::min());
    LAU_REQUIRE(minimum.stringify() == "-9223372036854775808");

    const auto maximum = parseJson("18446744073709551615");
    LAU_REQUIRE(maximum.isUnsignedInteger());
    LAU_REQUIRE(maximum.asUInt64() == std::numeric_limits<std::uint64_t>::max());
    LAU_REQUIRE(maximum.stringify() == "18446744073709551615");

    const auto exact = parseJson(std::to_string(kBeyondDoubleInteger));
    LAU_REQUIRE(exact.isUnsignedInteger());
    LAU_REQUIRE(exact.asUInt64() == kBeyondDoubleInteger);
    LAU_REQUIRE(exact.asNumber(-1.0) == -1.0);
    LAU_REQUIRE(exact.stringify() == std::to_string(kBeyondDoubleInteger));

    const auto floatingPoint = parseJson("1.0");
    LAU_REQUIRE(floatingPoint.isFloatingPoint());
    LAU_REQUIRE(!floatingPoint.isInteger());
    LAU_REQUIRE(floatingPoint.asDouble() == 1.0);
    LAU_REQUIRE(floatingPoint.asInt64(-1) == -1);

    const auto zeroWithLargeExponent = parseJson("0e9999");
    LAU_REQUIRE(zeroWithLargeExponent.isFloatingPoint());
    LAU_REQUIRE(zeroWithLargeExponent.asDouble() == 0.0);
    const auto negativeZeroWithLargeExponent = parseJson("-0.0e-9999");
    LAU_REQUIRE(negativeZeroWithLargeExponent.isFloatingPoint());
    LAU_REQUIRE(std::signbit(negativeZeroWithLargeExponent.asDouble()));

    const Json smallestSubnormal(std::numeric_limits<double>::denorm_min());
    const auto smallestSubnormalText = smallestSubnormal.stringify();
    const auto smallestSubnormalRoundTrip = parseJson(smallestSubnormalText);
    LAU_REQUIRE(smallestSubnormalRoundTrip.isFloatingPoint());
    LAU_REQUIRE(smallestSubnormalRoundTrip.asDouble() == std::numeric_limits<double>::denorm_min());

    for (const auto& text : {"+1", ".1", "01", "-01", "1.", "1e", "1e+", "--1",
                             "-9223372036854775809", "18446744073709551616", "1e309", "1e-9999",
                             "NaN", "Infinity"}) {
        requireInvalidJson(text);
    }

    {
        GlobalLocaleGuard locale;
        const auto parsed = parseJson("1234.5");
        LAU_REQUIRE(parsed.asDouble() == 1234.5);
        LAU_REQUIRE(parsed.stringify() == "1234.5");
        LAU_REQUIRE(Json(UINT64_C(1234)).stringify() == "1234");
    }

    const Json nonFinite(std::numeric_limits<double>::infinity());
    LAU_REQUIRE(!Json::validateResourceUsage(nonFinite, autoupdater::JsonResourceLimits{}));
    bool threw = false;
    try {
        (void)nonFinite.stringify();
    } catch (const std::domain_error&) {
        threw = true;
    }
    LAU_REQUIRE(threw);

    const Json invalidUtf8(bytes({0x80}));
    LAU_REQUIRE(!Json::validateResourceUsage(invalidUtf8, autoupdater::JsonResourceLimits{}));
    threw = false;
    try {
        (void)invalidUtf8.stringify();
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    LAU_REQUIRE(threw);
}

void testMetadataSchemasRequireExactTypesAndRanges() {
    auto limits = largeArtifactLimits();
    auto manifest = autoupdater::Manifest::parse(validManifest(), limits);
    LAU_REQUIRE(manifest);
    LAU_REQUIRE(manifest.value().files.size() == 1);
    LAU_REQUIRE(manifest.value().files.front().size == kBeyondDoubleInteger);
    auto manifestRoundTrip = autoupdater::Manifest::parse(manifest.value().toJson(), limits);
    LAU_REQUIRE(manifestRoundTrip);
    LAU_REQUIRE(manifestRoundTrip.value().files.front().size == kBeyondDoubleInteger);
    const auto futureManifest =
        autoupdater::Manifest::parse(R"json({"schemaVersion":2,"futureField":true})json", limits);
    LAU_REQUIRE(!futureManifest);
    LAU_REQUIRE(futureManifest.error().code == autoupdater::ErrorCode::UnsupportedManifestSchema);

    for (const auto& text : std::vector<std::string>{
             R"json({"schemaVersion":1.0,"version":"1.2.3"})json",
             R"json({"schemaVersion":1,"version":"1.2.3","unknown":true})json",
             R"json({"schemaVersion":1,"version":"1.2.3","expiresAt":false})json",
             std::string(R"json({"schemaVersion":1,"version":"1.2.3-)json") + bytes({0xc2, 0xaa}) +
                 R"json("})json",
             std::string(R"json({"schemaVersion":1,"version":")json") + std::string(100000, '9') +
                 R"json(.2.3"})json",
             std::string(R"json({"schemaVersion":1,"version":"1.2.3","files":[{"path":"app.bin","sha256":")json") +
                 sha('A') + R"json(","size":1}]})json",
             std::string(R"json({"schemaVersion":1,"version":"1.2.3","files":[{"path":"app.bin","sha256":")json") +
                 sha('a') + R"json(","size":1.5}]})json",
             std::string(R"json({"schemaVersion":1,"version":"1.2.3","files":[{"path":"app.bin","sha256":")json") +
                 sha('a') + R"json(","size":-1}]})json",
             std::string(R"json({"schemaVersion":1,"version":"1.2.3","files":[{"path":"shared.bin","localPath":"a.bin","sha256":")json") +
                 sha('a') + R"json(","size":1},{"path":"shared.bin","localPath":"b.bin","sha256":")json" +
                 sha('b') + R"json(","size":1}]})json"}) {
        LAU_REQUIRE(!autoupdater::Manifest::parse(text, limits));
    }

    const auto requireManifestSourceConflict = [&](std::vector<autoupdater::ManifestFile> files) {
        autoupdater::Manifest candidate;
        candidate.version = autoupdater::Version::parse("1.2.3").value();
        candidate.files = std::move(files);
        LAU_REQUIRE(!autoupdater::Manifest::parse(candidate.toJson(), limits));
    };
    const std::vector<autoupdater::ManifestFile> conflictingManifestSources = {
        {"a", "bin/first", sha('a'), 1},
        {"a-", "bin/sibling", sha('a'), 1},
        {"a/child", "bin/child", sha('a'), 1},
    };
    requireManifestSourceConflict(conflictingManifestSources);
    requireManifestSourceConflict(std::vector<autoupdater::ManifestFile>(conflictingManifestSources.rbegin(),
                                                                          conflictingManifestSources.rend()));

    const auto indexWrongType =
        R"json({"schemaVersion":1,"targets":[{"platform":false,"manifestUrl":"manifest.json"}]})json";
    LAU_REQUIRE(!autoupdater::IndexManifest::parse(indexWrongType, limits));
    const auto futureIndex =
        autoupdater::IndexManifest::parse(R"json({"schemaVersion":2,"futureField":true})json", limits);
    LAU_REQUIRE(!futureIndex);
    LAU_REQUIRE(futureIndex.error().code == autoupdater::ErrorCode::UnsupportedManifestSchema);

    auto plan = autoupdater::ApplyPlan::parse(validApplyPlan(), limits);
    LAU_REQUIRE(plan);
    LAU_REQUIRE(plan.value().operations.front().size == kBeyondDoubleInteger);
    auto planRoundTrip = autoupdater::ApplyPlan::parse(plan.value().toJson(), limits);
    LAU_REQUIRE(planRoundTrip);
    LAU_REQUIRE(planRoundTrip.value().operations.front().size == kBeyondDoubleInteger);

    for (const auto& text : std::vector<std::string>{
             R"json({"schemaVersion":2.0,"intent":"install","installDir":"/i","stagingDir":"/s","backupDir":"/b","operations":[]})json",
             addField(validApplyPlan(1), "unknown", Json(true)),
             replaceField(validApplyPlan(1), "fromVersion", Json(false)),
             replaceField(validApplyPlan(1), "restartCommand", Json("not-an-array")),
             std::string(
                 R"json({"schemaVersion":2,"intent":"install","installDir":"/i","stagingDir":"/s","backupDir":"/b","restartCommand":["app",1],"operations":[]})json"),
             std::string(
                 R"json({"schemaVersion":2,"intent":"install","installDir":"/i","stagingDir":"/s","backupDir":"/b","operations":[{"type":"replace","source":"app","target":"app","sha256":")json") +
                 sha('a') + R"json(","size":1.5}]})json",
             R"json({"schemaVersion":2,"intent":"install","installDir":"/i","stagingDir":"/s","backupDir":"/b","operations":[{"type":"remove","target":"old","size":1}]})json",
             R"json({"schemaVersion":2,"intent":"install","installDir":"/i","stagingDir":"/s","backupDir":"/b","operations":[{"type":"remove","target":"old","precondition":{"exists":false,"size":0}}]})json"}) {
        LAU_REQUIRE(!autoupdater::ApplyPlan::parse(text, limits));
    }

    const auto requireApplySourceConflict = [&](std::vector<autoupdater::ApplyOperation> operations) {
        autoupdater::ApplyPlan candidate;
        candidate.installDir = "/install";
        candidate.stagingDir = "/staging";
        candidate.backupDir = "/backup";
        candidate.operations = std::move(operations);
        LAU_REQUIRE(!autoupdater::ApplyPlan::parse(candidate.toJson(), limits));
    };
    const std::vector<autoupdater::ApplyOperation> conflictingApplySources = {
        {autoupdater::ApplyOperationType::Replace, "a", "bin/first", sha('a'), 1},
        {autoupdater::ApplyOperationType::Replace, "a-", "bin/sibling", sha('a'), 1},
        {autoupdater::ApplyOperationType::Replace, "a/child", "bin/child", sha('a'), 1},
    };
    requireApplySourceConflict(conflictingApplySources);
    requireApplySourceConflict(std::vector<autoupdater::ApplyOperation>(conflictingApplySources.rbegin(),
                                                                         conflictingApplySources.rend()));
}

void testStateAndJournalSchemasFailClosed() {
    TemporaryDirectory temporary;
    auto limits = largeArtifactLimits();
    const auto statePath = temporary.path() / "state.json";
    auto stateStore = autoupdater::createJsonStateStore(statePath, limits);

    autoupdater::DownloadResumeState resume;
    resume.key = "https://updates.example.test/app.bin";
    resume.offset = kBeyondDoubleInteger;
    resume.etag = "\"exact\"";
    resume.lastModified = "Sun, 19 Jul 2026 00:00:00 GMT";
    resume.sha256 = sha('a');
    LAU_REQUIRE(stateStore->saveDownloadResume(resume));
    auto loaded = stateStore->loadDownloadResume(resume.key);
    LAU_REQUIRE(loaded);
    LAU_REQUIRE(loaded.value().has_value());
    LAU_REQUIRE(loaded.value()->offset == kBeyondDoubleInteger);

    resume.etag = "safe\r\nInjected: true";
    LAU_REQUIRE(!stateStore->saveDownloadResume(resume));

    autoupdater::PendingUpdate pending;
    pending.version = autoupdater::Version::parse("2.0.0").value();
    pending.releaseId = "release-2";
    pending.backupDir = temporary.path() / std::filesystem::path(std::string("bad\0path", 8));
    pending.applyPlanPath = temporary.path() / "apply-plan.json";
    pending.applyPlanDigest = sha('b');
    LAU_REQUIRE(!stateStore->savePendingUpdate(pending));

    const std::string validatorRecord =
        std::string(R"json({"schemaVersion":1,"downloadResume":{"artifact":{"offset":1,"etag":"ok\r\nInjected: true","lastModified":"","sha256":")json") +
        sha('a') + R"json("}}})json";
    const auto invalidValidatorPath = temporary.path() / "invalid-validator.json";
    writeFile(invalidValidatorPath, validatorRecord);
    LAU_REQUIRE(!autoupdater::createJsonStateStore(invalidValidatorPath, limits)->loadDownloadResume("artifact"));

    const std::string validSidecarRecord =
        std::string(R"json({"offset":1,"etag":"","lastModified":"","sha256":")json") + sha('a') +
        R"json(","releaseKey":")json" + sha('b') + R"json(","updatedAt":1})json";
    const std::vector<std::string> invalidResumeSidecars = {
        R"json({"schemaVersion":2,"entries":{}})json",
        R"json({"schemaVersion":1,"entries":{},"unknown":true})json",
        std::string(R"json({"schemaVersion":1,"entries":{"https://updates.example.test/?token=secret":)json") +
            validSidecarRecord + "}}",
        std::string(R"json({"schemaVersion":1,"entries":{")json") + sha('c') +
            R"json(":{"offset":1,"etag":"","lastModified":"","sha256":")json" + sha('a') +
            R"json(","releaseKey":")json" + sha('b') + R"json("}}})json",
        std::string(R"json({"schemaVersion":1,"entries":{")json") + sha('c') +
            R"json(":{"offset":1,"etag":"","lastModified":"","sha256":")json" + sha('a') +
            R"json(","releaseKey":")json" + sha('b') + R"json(","updatedAt":1.5}}})json",
    };
    for (std::size_t index = 0; index < invalidResumeSidecars.size(); ++index) {
        const auto invalidSidecarState =
            temporary.path() / ("invalid-resume-sidecar-" + std::to_string(index) + ".json");
        writeFile(std::filesystem::path(invalidSidecarState.string() + ".resume"), invalidResumeSidecars[index]);
        LAU_REQUIRE(!autoupdater::createJsonStateStore(invalidSidecarState, limits)->loadDownloadResume("artifact"));
    }

    for (std::size_t index = 0; index < 3; ++index) {
        const auto invalidPath = temporary.path() / ("invalid-" + std::to_string(index) + ".json");
        const std::vector<std::string> invalidStates = {
            R"json({"schemaVersion":1.0})json",
            R"json({"schemaVersion":1,"unknown":true})json",
            std::string(R"json({"schemaVersion":1,"downloadResume":{"artifact":{"offset":1.5,"etag":"","lastModified":"","sha256":")json") +
                sha('a') + R"json("}}})json"};
        writeFile(invalidPath, invalidStates[index]);
        LAU_REQUIRE(!autoupdater::createJsonStateStore(invalidPath, limits)->loadLastAcceptedVersion());
    }
    const auto futureStatePath = temporary.path() / "future-state.json";
    writeFile(futureStatePath, R"json({"schemaVersion":2,"futureField":true})json");
    const auto futureState =
        autoupdater::createJsonStateStore(futureStatePath, limits)->loadLastAcceptedVersion();
    LAU_REQUIRE(!futureState);
    LAU_REQUIRE(futureState.error().message.find("unsupported") != std::string::npos);

    const auto transactionId = sha('a');
    const auto planDigest = sha('b');
    auto receipt = autoupdater::serializeApplyTransactionReceipt({transactionId, planDigest});
    LAU_REQUIRE(receipt);
    const auto legacyReceipt = autoupdater::parseApplyTransactionReceipt(receipt.value());
    LAU_REQUIRE(legacyReceipt);
    LAU_REQUIRE(!legacyReceipt.value().completedAt);
    LAU_REQUIRE(parseJson(receipt.value()).get("schemaVersion")->asUInt64() == 2);
    LAU_REQUIRE(!autoupdater::parseApplyTransactionReceipt(addField(receipt.value(), "unknown", Json(true))));
    LAU_REQUIRE(!autoupdater::parseApplyTransactionReceipt(
        replaceField(receipt.value(), "schemaVersion", Json(2.0))));
    const auto futureReceipt = autoupdater::parseApplyTransactionReceipt(
        R"json({"schemaVersion":4,"transactionId":"ignored","futureField":true})json");
    LAU_REQUIRE(!futureReceipt);
    LAU_REQUIRE(futureReceipt.error().message.find("Unsupported") != std::string::npos);

    const auto completedAt = autoupdater::util::parseRfc3339("2026-07-20T12:34:56.123456789Z");
    LAU_REQUIRE(completedAt);
    auto terminalReceiptJson =
        autoupdater::serializeApplyTransactionReceipt({transactionId, planDigest, completedAt.value()});
    LAU_REQUIRE(terminalReceiptJson);
    LAU_REQUIRE(parseJson(terminalReceiptJson.value()).get("schemaVersion")->asUInt64() == 3);
    const auto terminalReceipt = autoupdater::parseApplyTransactionReceipt(terminalReceiptJson.value());
    LAU_REQUIRE(terminalReceipt);
    LAU_REQUIRE(terminalReceipt.value().completedAt == completedAt.value());
    LAU_REQUIRE(!autoupdater::parseApplyTransactionReceipt(
        replaceField(receipt.value(), "schemaVersion", Json(UINT64_C(3)))));
    LAU_REQUIRE(!autoupdater::parseApplyTransactionReceipt(
        replaceField(terminalReceiptJson.value(), "schemaVersion", Json(UINT64_C(2)))));
    LAU_REQUIRE(!autoupdater::parseApplyTransactionReceipt(
        replaceField(terminalReceiptJson.value(), "completedAt", Json("0001-01-01T00:00:00+23:59"))));
    LAU_REQUIRE(!autoupdater::parseApplyTransactionReceipt(
        replaceField(terminalReceiptJson.value(), "completedAt", Json(true))));
    LAU_REQUIRE(!autoupdater::serializeApplyTransactionReceipt(
        {transactionId, planDigest, autoupdater::util::UtcInstant{0, 1000000000U}}));
    LAU_REQUIRE(!autoupdater::serializeApplyTransactionReceipt(
        {transactionId, planDigest, autoupdater::util::UtcInstant{INT64_C(-62135596801), 0}}));

    auto snapshotPlan = autoupdater::ApplyPlan::parse(validApplyPlan(1), limits);
    LAU_REQUIRE(snapshotPlan);
    const auto snapshotJson = snapshotPlan.value().toJson();
    const auto snapshotDigest = autoupdater::updater::applyPlanDigest(snapshotPlan.value());
    LAU_REQUIRE(snapshotDigest);
    const autoupdater::ApplyTransactionReceipt snapshotReceipt{transactionId, snapshotDigest.value(),
                                                               completedAt.value()};
    const auto snapshotPath = temporary.path() / "terminal-plan-install" / ".autoupdater" / "journal" /
                              (transactionId + ".plan.json");
    writeFile(snapshotPath, snapshotJson);
    auto fileSystem = autoupdater::createDefaultFileSystem();
    auto loadedSnapshot = autoupdater::detail::loadTerminalApplyPlan(
        *fileSystem, temporary.path() / "terminal-plan-install", snapshotReceipt, limits);
    LAU_REQUIRE(loadedSnapshot);
    LAU_REQUIRE(loadedSnapshot.value().toVersion == snapshotPlan.value().toVersion);
    writeFile(snapshotPath, "{}");
    LAU_REQUIRE(!autoupdater::detail::loadTerminalApplyPlan(
        *fileSystem, temporary.path() / "terminal-plan-install", snapshotReceipt, limits));
    std::error_code snapshotError;
    LAU_REQUIRE(std::filesystem::remove(snapshotPath, snapshotError));
    LAU_REQUIRE(!snapshotError);
    LAU_REQUIRE(!autoupdater::detail::loadTerminalApplyPlan(
        *fileSystem, temporary.path() / "terminal-plan-install", snapshotReceipt, limits));

    autoupdater::updater::ApplyJournalSummary summary;
    summary.transactionId = transactionId;
    summary.planDigest = planDigest;
    summary.operationCount = 0;
    auto summaryJson = autoupdater::updater::serializeApplyJournalSummary(summary);
    LAU_REQUIRE(summaryJson);
    LAU_REQUIRE(parseJson(summaryJson.value()).get("schemaVersion")->asUInt64() == 2);
    auto invalidSummary = summary;
    invalidSummary.applyError = {"ApplyFailed", ""};
    LAU_REQUIRE(!autoupdater::updater::serializeApplyJournalSummary(invalidSummary));
    invalidSummary = summary;
    invalidSummary.fileState = static_cast<autoupdater::updater::JournalFileState>(255);
    LAU_REQUIRE(!autoupdater::updater::serializeApplyJournalSummary(invalidSummary));
    LAU_REQUIRE(autoupdater::updater::parseApplyJournalSummary(summaryJson.value()));
    LAU_REQUIRE(!autoupdater::updater::parseApplyJournalSummary(
        addField(summaryJson.value(), "unknown", Json(true))));
    LAU_REQUIRE(!autoupdater::updater::parseApplyJournalSummary(
        replaceField(summaryJson.value(), "operationCount", Json("00"))));

    auto completeSummary = summary;
    completeSummary.fileState = autoupdater::updater::JournalFileState::Complete;
    completeSummary.restartState = autoupdater::updater::JournalRestartState::NotRequested;
    completeSummary.completedAt = completedAt.value();
    const auto completeSummaryJson = autoupdater::updater::serializeApplyJournalSummary(completeSummary);
    LAU_REQUIRE(completeSummaryJson);
    const auto parsedCompleteSummary =
        autoupdater::updater::parseApplyJournalSummary(completeSummaryJson.value());
    LAU_REQUIRE(parsedCompleteSummary);
    LAU_REQUIRE(parsedCompleteSummary.value().completedAt == completedAt.value());
    LAU_REQUIRE(!autoupdater::updater::parseApplyJournalSummary(
        replaceField(replaceField(replaceField(summaryJson.value(), "schemaVersion", Json(UINT64_C(3))),
                                  "fileState", Json("complete")),
                     "restartState", Json("not_requested"))));
    auto legacyCompleteJson = replaceField(summaryJson.value(), "schemaVersion", Json(UINT64_C(2)));
    legacyCompleteJson = replaceField(legacyCompleteJson, "fileState", Json("complete"));
    legacyCompleteJson = replaceField(legacyCompleteJson, "restartState", Json("not_requested"));
    const auto parsedLegacyComplete = autoupdater::updater::parseApplyJournalSummary(legacyCompleteJson);
    LAU_REQUIRE(parsedLegacyComplete);
    LAU_REQUIRE(parsedLegacyComplete.value().schemaVersion == 2);
    LAU_REQUIRE(!parsedLegacyComplete.value().completedAt);
    LAU_REQUIRE(!autoupdater::updater::parseApplyJournalSummary(
        replaceField(completeSummaryJson.value(), "schemaVersion", Json(UINT64_C(2)))));
    LAU_REQUIRE(!autoupdater::updater::parseApplyJournalSummary(
        addField(summaryJson.value(), "completedAt", Json("2026-07-20T12:34:56Z"))));
    LAU_REQUIRE(!autoupdater::updater::parseApplyJournalSummary(
        replaceField(completeSummaryJson.value(), "completedAt", Json("9999-12-31T23:59:59-23:59"))));
    LAU_REQUIRE(!autoupdater::updater::parseApplyJournalSummary(
        replaceField(summaryJson.value(), "schemaVersion", Json(UINT64_C(4)))));
    auto invalidTimestampSummary = completeSummary;
    invalidTimestampSummary.completedAt = autoupdater::util::UtcInstant{INT64_C(253402300800), 0};
    LAU_REQUIRE(!autoupdater::updater::serializeApplyJournalSummary(invalidTimestampSummary));

    auto summaryRoot = parseJson(summaryJson.value()).asObject();
    auto applyError = summaryRoot.at("applyError").asObject();
    applyError.emplace("unknown", true);
    summaryRoot["applyError"] = Json(std::move(applyError));
    LAU_REQUIRE(!autoupdater::updater::parseApplyJournalSummary(Json(std::move(summaryRoot)).stringify()));

    autoupdater::updater::ApplyJournalOperation operation;
    operation.transactionId = transactionId;
    operation.operationId = sha('c');
    operation.intent = "remove";
    auto operationJson = autoupdater::updater::serializeApplyJournalOperation(operation);
    LAU_REQUIRE(operationJson);
    auto invalidOperation = operation;
    invalidOperation.error = {"ApplyFailed", ""};
    LAU_REQUIRE(!autoupdater::updater::serializeApplyJournalOperation(invalidOperation));
    invalidOperation = operation;
    invalidOperation.applyState = static_cast<autoupdater::updater::JournalApplyState>(255);
    LAU_REQUIRE(!autoupdater::updater::serializeApplyJournalOperation(invalidOperation));
    LAU_REQUIRE(autoupdater::updater::parseApplyJournalOperation(operationJson.value()));
    LAU_REQUIRE(!autoupdater::updater::parseApplyJournalOperation(
        addField(operationJson.value(), "unknown", Json(true))));
    LAU_REQUIRE(!autoupdater::updater::parseApplyJournalOperation(
        replaceField(operationJson.value(), "index", Json("00"))));
}
