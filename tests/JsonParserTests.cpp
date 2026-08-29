#include "TestCommon.h"

#include "util/Json.h"

#include <string>

using autoupdater::util::Json;

void testJsonParsesUnicodeEscapes() {
    auto json = Json::parse(R"json({"text": "ab\u0041"})json");
    LAU_REQUIRE(json);
    LAU_REQUIRE(json.value().get("text")->asString() == "abA");

    auto emoji = Json::parse("{\"face\": \"\\uD83D\\uDE00\"}");
    LAU_REQUIRE(emoji);
    // U+1F600 as UTF-8
    LAU_REQUIRE(emoji.value().get("face")->asString() == "\xF0\x9F\x98\x80");
}

void testJsonRejectsInvalidUnicodeEscapes() {
    LAU_REQUIRE(!Json::parse("{\"a\": \"\\uzzzz\"}"));
    LAU_REQUIRE(!Json::parse("{\"a\": \"\\uD83D\"}"));        // unpaired high surrogate
    LAU_REQUIRE(!Json::parse("{\"a\": \"\\uDE00\"}"));        // unpaired low surrogate
    LAU_REQUIRE(!Json::parse("{\"a\": \"\\uD83D\\u0041\"}")); // high surrogate + non-low
}

void testJsonRejectsUnescapedControlCharacters() {
    LAU_REQUIRE(!Json::parse("{\"a\": \"x\ty\"}")); // raw tab inside string
    LAU_REQUIRE(!Json::parse("{\"a\": \"x\ny\"}")); // raw newline inside string
    LAU_REQUIRE(Json::parse("{\"a\": \"x\\ty\"}")); // escaped tab is fine
}

void testJsonRejectsDeepNesting() {
    std::string deep;
    for (int i = 0; i < 100000; ++i) {
        deep += '[';
    }
    for (int i = 0; i < 100000; ++i) {
        deep += ']';
    }
    auto json = Json::parse(deep);
    LAU_REQUIRE(!json);

    std::string shallow = "{\"a\":";
    for (int i = 0; i < 100; ++i) {
        shallow += '[';
    }
    for (int i = 0; i < 100; ++i) {
        shallow += ']';
    }
    shallow += '}';
    LAU_REQUIRE(Json::parse(shallow));
}

void testJsonNumbersRejectOutOfRangeConversions() {
    auto json = Json::parse("{\"huge\": 1e300, \"negative\": -5, \"fraction\": 1.5, \"ok\": 42}");
    LAU_REQUIRE(json);
    const auto& root = json.value();

    LAU_REQUIRE(root.get("huge")->asInt(-1) == -1);
    LAU_REQUIRE(root.get("negative")->asInt(-1) == -5);
    LAU_REQUIRE(root.get("fraction")->asInt(-1) == -1); // not integral
    LAU_REQUIRE(root.get("ok")->asInt(-1) == 42);

    std::uint64_t out = 0;
    LAU_REQUIRE(!root.get("huge")->asUInt64(out));
    LAU_REQUIRE(!root.get("negative")->asUInt64(out));
    LAU_REQUIRE(!root.get("fraction")->asUInt64(out));
    LAU_REQUIRE(root.get("ok")->asUInt64(out));
    LAU_REQUIRE(out == 42);

    // Stringifying a non-integral or out-of-range double must not hit
    // undefined behavior and must round-trip as a number.
    auto huge = Json::parse("{\"v\": 1e300}");
    LAU_REQUIRE(huge);
    const auto text = huge.value().stringify();
    LAU_REQUIRE(text.find("1e+300") != std::string::npos);
}

void testJsonStrictWhitespace() {
    LAU_REQUIRE(!Json::parse("{\v\"a\": 1}")); // vertical tab is not JSON whitespace
    LAU_REQUIRE(Json::parse("{\n\t\r \"a\": 1}"));
}
