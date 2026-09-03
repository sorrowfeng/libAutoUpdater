#include "util/Json.h"

#include <charconv>
#include <cmath>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <system_error>

namespace autoupdater::util {

namespace {

const std::string kEmptyString;
const Json::Object kEmptyObject;
const Json::Array kEmptyArray;

bool isContinuationByte(unsigned char value) noexcept {
    return value >= 0x80U && value <= 0xBFU;
}

bool isValidUtf8(const std::string& text) noexcept {
    std::size_t index = 0;
    while (index < text.size()) {
        const auto first = static_cast<unsigned char>(text[index]);
        if (first <= 0x7FU) {
            ++index;
            continue;
        }

        if (first >= 0xC2U && first <= 0xDFU) {
            if (index + 1 >= text.size() || !isContinuationByte(static_cast<unsigned char>(text[index + 1]))) {
                return false;
            }
            index += 2;
            continue;
        }

        if (first >= 0xE0U && first <= 0xEFU) {
            if (index + 2 >= text.size()) {
                return false;
            }
            const auto second = static_cast<unsigned char>(text[index + 1]);
            const auto third = static_cast<unsigned char>(text[index + 2]);
            if (!isContinuationByte(third)) {
                return false;
            }
            if (first == 0xE0U) {
                if (second < 0xA0U || second > 0xBFU) {
                    return false;
                }
            } else if (first == 0xEDU) {
                if (second < 0x80U || second > 0x9FU) {
                    return false;
                }
            } else if (!isContinuationByte(second)) {
                return false;
            }
            index += 3;
            continue;
        }

        if (first >= 0xF0U && first <= 0xF4U) {
            if (index + 3 >= text.size()) {
                return false;
            }
            const auto second = static_cast<unsigned char>(text[index + 1]);
            const auto third = static_cast<unsigned char>(text[index + 2]);
            const auto fourth = static_cast<unsigned char>(text[index + 3]);
            if (!isContinuationByte(third) || !isContinuationByte(fourth)) {
                return false;
            }
            if (first == 0xF0U) {
                if (second < 0x90U || second > 0xBFU) {
                    return false;
                }
            } else if (first == 0xF4U) {
                if (second < 0x80U || second > 0x8FU) {
                    return false;
                }
            } else if (!isContinuationByte(second)) {
                return false;
            }
            index += 4;
            continue;
        }

        return false;
    }
    return true;
}

bool isJsonWhitespace(char value) noexcept {
    return value == ' ' || value == '\t' || value == '\n' || value == '\r';
}

bool isDecimalDigit(char value) noexcept {
    return value >= '0' && value <= '9';
}

bool isExactlyRepresentableAsDouble(std::uint64_t magnitude) noexcept {
    if (magnitude == 0) {
        return true;
    }

    unsigned int bitCount = 0;
    for (auto remaining = magnitude; remaining != 0; remaining >>= 1U) {
        ++bitCount;
    }
    constexpr auto precision = static_cast<unsigned int>(std::numeric_limits<double>::digits);
    if (bitCount <= precision) {
        return true;
    }
    const auto discardedBits = bitCount - precision;
    const auto discardedMask = (UINT64_C(1) << discardedBits) - UINT64_C(1);
    return (magnitude & discardedMask) == 0;
}

std::uint64_t unsignedMagnitude(std::int64_t value) noexcept {
    if (value >= 0) {
        return static_cast<std::uint64_t>(value);
    }
    return static_cast<std::uint64_t>(-(value + 1)) + UINT64_C(1);
}

std::string integerText(std::int64_t value) {
    char buffer[32]{};
    const auto converted = std::to_chars(std::begin(buffer), std::end(buffer), value);
    if (converted.ec != std::errc{}) {
        throw std::runtime_error("Failed to serialize signed JSON integer");
    }
    return std::string(buffer, converted.ptr);
}

std::string integerText(std::uint64_t value) {
    char buffer[32]{};
    const auto converted = std::to_chars(std::begin(buffer), std::end(buffer), value);
    if (converted.ec != std::errc{}) {
        throw std::runtime_error("Failed to serialize unsigned JSON integer");
    }
    return std::string(buffer, converted.ptr);
}

std::string floatingPointText(double value) {
    if (!std::isfinite(value)) {
        throw std::domain_error("JSON cannot serialize a non-finite number");
    }

    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream << std::setprecision(std::numeric_limits<double>::max_digits10) << value;
    if (!stream) {
        throw std::runtime_error("Failed to serialize JSON floating-point number");
    }
    auto text = stream.str();
    if (text.find_first_of(".eE") == std::string::npos) {
        text += ".0";
    }
    return text;
}

std::string numberText(const Json& value) {
    if (value.isSignedInteger()) {
        return integerText(value.asInt64());
    }
    if (value.isUnsignedInteger()) {
        return integerText(value.asUInt64());
    }
    return floatingPointText(value.asDouble());
}

class Parser {
  public:
    Parser(const std::string& input, const JsonResourceLimits& limits) : input_(input), limits_(limits) {}

    Result<Json> parse() {
        skipWhitespace();
        auto value = parseValue(1);
        if (!value) {
            return value;
        }
        skipWhitespace();
        if (pos_ != input_.size()) {
            return fail("Unexpected trailing JSON content");
        }
        return value;
    }

  private:
    Result<Json> parseValue(std::size_t depth) {
        skipWhitespace();
        if (depth > limits_.maxDepth) {
            return limit("JSON depth limit exceeded");
        }
        if (nodeCount_ >= limits_.maxNodes) {
            return limit("JSON node limit exceeded");
        }
        ++nodeCount_;
        if (pos_ >= input_.size()) {
            return fail("Unexpected end of JSON");
        }

        const char value = input_[pos_];
        if (value == '"') {
            auto parsed = parseString();
            if (!parsed) {
                return Result<Json>::fail(parsed.error());
            }
            return Result<Json>::ok(Json(std::move(parsed.value())));
        }
        if (value == '{') {
            return parseObject(depth);
        }
        if (value == '[') {
            return parseArray(depth);
        }
        if (value == '-' || isDecimalDigit(value)) {
            return parseNumber();
        }
        if (match("true")) {
            return Result<Json>::ok(Json(true));
        }
        if (match("false")) {
            return Result<Json>::ok(Json(false));
        }
        if (match("null")) {
            return Result<Json>::ok(Json(nullptr));
        }
        return fail("Invalid JSON value");
    }

    Result<Json> parseObject(std::size_t depth) {
        ++pos_;
        Json::Object object;
        std::size_t entryCount = 0;
        skipWhitespace();
        if (consume('}')) {
            return Result<Json>::ok(Json(std::move(object)));
        }

        while (true) {
            if (entryCount >= limits_.maxContainerEntries) {
                return limit("JSON object entry limit exceeded");
            }
            ++entryCount;
            skipWhitespace();
            if (pos_ >= input_.size() || input_[pos_] != '"') {
                return fail("Expected object key");
            }
            auto key = parseString();
            if (!key) {
                return Result<Json>::fail(key.error());
            }
            if (object.find(key.value()) != object.end()) {
                return fail("Duplicate JSON object key");
            }
            skipWhitespace();
            if (!consume(':')) {
                return fail("Expected ':' after object key");
            }
            if (depth >= limits_.maxDepth) {
                return limit("JSON depth limit exceeded");
            }
            auto value = parseValue(depth + 1);
            if (!value) {
                return value;
            }
            object.emplace(std::move(key.value()), std::move(value.value()));
            skipWhitespace();
            if (consume('}')) {
                break;
            }
            if (!consume(',')) {
                return fail("Expected ',' or '}' in object");
            }
        }
        return Result<Json>::ok(Json(std::move(object)));
    }

    Result<Json> parseArray(std::size_t depth) {
        ++pos_;
        Json::Array array;
        skipWhitespace();
        if (consume(']')) {
            return Result<Json>::ok(Json(std::move(array)));
        }

        while (true) {
            if (array.size() >= limits_.maxContainerEntries) {
                return limit("JSON array entry limit exceeded");
            }
            if (depth >= limits_.maxDepth) {
                return limit("JSON depth limit exceeded");
            }
            auto value = parseValue(depth + 1);
            if (!value) {
                return value;
            }
            array.push_back(std::move(value.value()));
            skipWhitespace();
            if (consume(']')) {
                break;
            }
            if (!consume(',')) {
                return fail("Expected ',' or ']' in array");
            }
        }
        return Result<Json>::ok(Json(std::move(array)));
    }

    Result<Json> parseNumber() {
        const auto start = pos_;
        bool floatingPoint = false;

        if (input_[pos_] == '-') {
            if (!consumeNumberByte(start)) {
                return limit("JSON number length limit exceeded");
            }
        }
        if (pos_ >= input_.size()) {
            return fail("Invalid number");
        }

        if (input_[pos_] == '0') {
            if (!consumeNumberByte(start)) {
                return limit("JSON number length limit exceeded");
            }
            if (pos_ < input_.size() && isDecimalDigit(input_[pos_])) {
                return fail("JSON numbers cannot contain leading zeroes");
            }
        } else if (input_[pos_] >= '1' && input_[pos_] <= '9') {
            do {
                if (!consumeNumberByte(start)) {
                    return limit("JSON number length limit exceeded");
                }
            } while (pos_ < input_.size() && isDecimalDigit(input_[pos_]));
        } else {
            return fail("Invalid number");
        }

        if (pos_ < input_.size() && input_[pos_] == '.') {
            floatingPoint = true;
            if (!consumeNumberByte(start)) {
                return limit("JSON number length limit exceeded");
            }
            if (pos_ >= input_.size() || !isDecimalDigit(input_[pos_])) {
                return fail("Invalid number fraction");
            }
            do {
                if (!consumeNumberByte(start)) {
                    return limit("JSON number length limit exceeded");
                }
            } while (pos_ < input_.size() && isDecimalDigit(input_[pos_]));
        }

        if (pos_ < input_.size() && (input_[pos_] == 'e' || input_[pos_] == 'E')) {
            floatingPoint = true;
            if (!consumeNumberByte(start)) {
                return limit("JSON number length limit exceeded");
            }
            if (pos_ < input_.size() && (input_[pos_] == '+' || input_[pos_] == '-')) {
                if (!consumeNumberByte(start)) {
                    return limit("JSON number length limit exceeded");
                }
            }
            if (pos_ >= input_.size() || !isDecimalDigit(input_[pos_])) {
                return fail("Invalid number exponent");
            }
            do {
                if (!consumeNumberByte(start)) {
                    return limit("JSON number length limit exceeded");
                }
            } while (pos_ < input_.size() && isDecimalDigit(input_[pos_]));
        }

        const std::string_view token(input_.data() + start, pos_ - start);
        if (floatingPoint) {
            return parseFloatingPoint(token);
        }
        return parseInteger(token);
    }

    Result<Json> parseInteger(std::string_view token) const {
        if (!token.empty() && token.front() == '-') {
            std::uint64_t magnitude = 0;
            const auto converted = std::from_chars(token.data() + 1, token.data() + token.size(), magnitude, 10);
            if (converted.ec != std::errc{} || converted.ptr != token.data() + token.size() ||
                magnitude > (UINT64_C(1) << 63U)) {
                return fail("Signed JSON integer is out of range");
            }
            if (magnitude == (UINT64_C(1) << 63U)) {
                return Result<Json>::ok(Json(std::numeric_limits<std::int64_t>::min()));
            }
            return Result<Json>::ok(Json(-static_cast<std::int64_t>(magnitude)));
        }

        std::uint64_t value = 0;
        const auto converted = std::from_chars(token.data(), token.data() + token.size(), value, 10);
        if (converted.ec != std::errc{} || converted.ptr != token.data() + token.size()) {
            return fail("Unsigned JSON integer is out of range");
        }
        return Result<Json>::ok(Json(value));
    }

    Result<Json> parseFloatingPoint(std::string_view token) const {
        const auto exponent = token.find_first_of("eE");
        const auto significandEnd = exponent == std::string_view::npos ? token.size() : exponent;
        bool nonZeroSignificand = false;
        for (std::size_t index = 0; index < significandEnd; ++index) {
            if (token[index] >= '1' && token[index] <= '9') {
                nonZeroSignificand = true;
                break;
            }
        }
        if (!nonZeroSignificand) {
            return Result<Json>::ok(Json(!token.empty() && token.front() == '-' ? -0.0 : 0.0));
        }

        // std::from_chars is locale-independent and performs exact decimal to
        // binary conversion, so it accepts subnormal values such as
        // 4.9406564584124654e-324. iostream extraction would reject them on
        // platforms whose libc++ treats a raised underflow flag as a parse
        // failure, breaking the shared JSON conformance corpus.
        double value = 0.0;
        const auto converted = std::from_chars(token.data(), token.data() + token.size(), value);
        if (converted.ec == std::errc::result_out_of_range || converted.ptr != token.data() + token.size()) {
            return fail("JSON floating-point number is out of range");
        }
        if (converted.ec != std::errc{} || value == 0.0) {
            return fail("JSON floating-point number underflows");
        }
        return Result<Json>::ok(Json(value));
    }

    Result<std::string> parseString() {
        if (!consume('"')) {
            return stringFail("Expected string");
        }

        std::string output;
        while (pos_ < input_.size()) {
            const char value = input_[pos_++];
            if (value == '"') {
                return Result<std::string>::ok(std::move(output));
            }
            if (static_cast<unsigned char>(value) < 0x20U) {
                return stringFail("Unescaped control character in JSON string");
            }
            if (value != '\\') {
                if (!appendByte(output, value)) {
                    return stringLimit();
                }
                continue;
            }

            if (pos_ >= input_.size()) {
                return stringFail("Invalid string escape");
            }
            const char escaped = input_[pos_++];
            switch (escaped) {
            case '"':
                if (!appendByte(output, '"')) {
                    return stringLimit();
                }
                break;
            case '\\':
                if (!appendByte(output, '\\')) {
                    return stringLimit();
                }
                break;
            case '/':
                if (!appendByte(output, '/')) {
                    return stringLimit();
                }
                break;
            case 'b':
                if (!appendByte(output, '\b')) {
                    return stringLimit();
                }
                break;
            case 'f':
                if (!appendByte(output, '\f')) {
                    return stringLimit();
                }
                break;
            case 'n':
                if (!appendByte(output, '\n')) {
                    return stringLimit();
                }
                break;
            case 'r':
                if (!appendByte(output, '\r')) {
                    return stringLimit();
                }
                break;
            case 't':
                if (!appendByte(output, '\t')) {
                    return stringLimit();
                }
                break;
            case 'u': {
                std::uint16_t first = 0;
                if (!parseHexCodeUnit(first)) {
                    return stringFail("Invalid unicode escape");
                }
                std::uint32_t codePoint = first;
                if (first >= 0xD800U && first <= 0xDBFFU) {
                    if (pos_ + 2 > input_.size() || input_[pos_] != '\\' || input_[pos_ + 1] != 'u') {
                        return stringFail("Unpaired high surrogate in JSON string");
                    }
                    pos_ += 2;
                    std::uint16_t second = 0;
                    if (!parseHexCodeUnit(second) || second < 0xDC00U || second > 0xDFFFU) {
                        return stringFail("Invalid low surrogate in JSON string");
                    }
                    codePoint = 0x10000U + ((static_cast<std::uint32_t>(first) - 0xD800U) << 10U) +
                                (static_cast<std::uint32_t>(second) - 0xDC00U);
                } else if (first >= 0xDC00U && first <= 0xDFFFU) {
                    return stringFail("Unpaired low surrogate in JSON string");
                }
                if (!appendCodePoint(output, codePoint)) {
                    return stringLimit();
                }
                break;
            }
            default:
                return stringFail("Invalid string escape");
            }
        }
        return stringFail("Unterminated string");
    }

    bool parseHexCodeUnit(std::uint16_t& value) {
        if (input_.size() - pos_ < 4) {
            return false;
        }
        value = 0;
        for (unsigned int index = 0; index < 4; ++index) {
            const char digit = input_[pos_++];
            value = static_cast<std::uint16_t>(value << 4U);
            if (digit >= '0' && digit <= '9') {
                value = static_cast<std::uint16_t>(value | static_cast<std::uint16_t>(digit - '0'));
            } else if (digit >= 'a' && digit <= 'f') {
                value = static_cast<std::uint16_t>(value | static_cast<std::uint16_t>(digit - 'a' + 10));
            } else if (digit >= 'A' && digit <= 'F') {
                value = static_cast<std::uint16_t>(value | static_cast<std::uint16_t>(digit - 'A' + 10));
            } else {
                return false;
            }
        }
        return true;
    }

    bool appendCodePoint(std::string& output, std::uint32_t codePoint) const {
        char encoded[4]{};
        std::size_t length = 0;
        if (codePoint <= 0x7FU) {
            encoded[0] = static_cast<char>(codePoint);
            length = 1;
        } else if (codePoint <= 0x7FFU) {
            encoded[0] = static_cast<char>(0xC0U | (codePoint >> 6U));
            encoded[1] = static_cast<char>(0x80U | (codePoint & 0x3FU));
            length = 2;
        } else if (codePoint <= 0xFFFFU) {
            encoded[0] = static_cast<char>(0xE0U | (codePoint >> 12U));
            encoded[1] = static_cast<char>(0x80U | ((codePoint >> 6U) & 0x3FU));
            encoded[2] = static_cast<char>(0x80U | (codePoint & 0x3FU));
            length = 3;
        } else {
            encoded[0] = static_cast<char>(0xF0U | (codePoint >> 18U));
            encoded[1] = static_cast<char>(0x80U | ((codePoint >> 12U) & 0x3FU));
            encoded[2] = static_cast<char>(0x80U | ((codePoint >> 6U) & 0x3FU));
            encoded[3] = static_cast<char>(0x80U | (codePoint & 0x3FU));
            length = 4;
        }
        if (output.size() > limits_.maxStringBytes || length > limits_.maxStringBytes - output.size()) {
            return false;
        }
        output.append(encoded, length);
        return true;
    }

    bool appendByte(std::string& output, char value) const {
        if (output.size() >= limits_.maxStringBytes) {
            return false;
        }
        output.push_back(value);
        return true;
    }

    bool consumeNumberByte(std::size_t start) {
        if (pos_ - start >= limits_.maxNumberBytes) {
            return false;
        }
        ++pos_;
        return true;
    }

    bool consume(char expected) {
        if (pos_ < input_.size() && input_[pos_] == expected) {
            ++pos_;
            return true;
        }
        return false;
    }

    bool match(std::string_view keyword) {
        if (input_.compare(pos_, keyword.size(), keyword.data(), keyword.size()) == 0) {
            pos_ += keyword.size();
            return true;
        }
        return false;
    }

    void skipWhitespace() {
        while (pos_ < input_.size() && isJsonWhitespace(input_[pos_])) {
            ++pos_;
        }
    }

    Result<Json> fail(const std::string& message) const {
        return Result<Json>::fail({ErrorCode::ManifestParseFailed, message});
    }

    Result<Json> limit(const std::string& message) const {
        return Result<Json>::fail({ErrorCode::ResourceLimitExceeded, message});
    }

    Result<std::string> stringFail(const std::string& message) const {
        return Result<std::string>::fail({ErrorCode::ManifestParseFailed, message});
    }

    Result<std::string> stringLimit() const {
        return Result<std::string>::fail({ErrorCode::ResourceLimitExceeded, "JSON string limit exceeded"});
    }

    const std::string& input_;
    const JsonResourceLimits& limits_;
    std::size_t pos_ = 0;
    std::size_t nodeCount_ = 0;
};

Result<void> validateValueResources(const Json& value, const JsonResourceLimits& limits, std::size_t depth,
                                    std::size_t& nodes) {
    if (depth > limits.maxDepth) {
        return Result<void>::fail({ErrorCode::ResourceLimitExceeded, "JSON depth limit exceeded"});
    }
    if (nodes >= limits.maxNodes) {
        return Result<void>::fail({ErrorCode::ResourceLimitExceeded, "JSON node limit exceeded"});
    }
    ++nodes;

    if (value.isString()) {
        if (!isValidUtf8(value.asString())) {
            return Result<void>::fail({ErrorCode::ManifestParseFailed, "JSON string is not valid UTF-8"});
        }
        if (value.asString().size() > limits.maxStringBytes) {
            return Result<void>::fail({ErrorCode::ResourceLimitExceeded, "JSON string limit exceeded"});
        }
    } else if (value.isNumber()) {
        if (value.isFloatingPoint() && !std::isfinite(value.asDouble())) {
            return Result<void>::fail({ErrorCode::ManifestParseFailed, "JSON number is not finite"});
        }
        if (numberText(value).size() > limits.maxNumberBytes) {
            return Result<void>::fail({ErrorCode::ResourceLimitExceeded, "JSON number length limit exceeded"});
        }
    } else if (value.isArray()) {
        const auto& array = value.asArray();
        if (array.size() > limits.maxContainerEntries) {
            return Result<void>::fail({ErrorCode::ResourceLimitExceeded, "JSON array entry limit exceeded"});
        }
        for (const auto& item : array) {
            if (depth >= limits.maxDepth) {
                return Result<void>::fail({ErrorCode::ResourceLimitExceeded, "JSON depth limit exceeded"});
            }
            auto valid = validateValueResources(item, limits, depth + 1, nodes);
            if (!valid) {
                return valid;
            }
        }
    } else if (value.isObject()) {
        const auto& object = value.asObject();
        if (object.size() > limits.maxContainerEntries) {
            return Result<void>::fail({ErrorCode::ResourceLimitExceeded, "JSON object entry limit exceeded"});
        }
        for (const auto& item : object) {
            if (!isValidUtf8(item.first)) {
                return Result<void>::fail({ErrorCode::ManifestParseFailed, "JSON object key is not valid UTF-8"});
            }
            if (item.first.size() > limits.maxStringBytes) {
                return Result<void>::fail({ErrorCode::ResourceLimitExceeded, "JSON string limit exceeded"});
            }
            if (depth >= limits.maxDepth) {
                return Result<void>::fail({ErrorCode::ResourceLimitExceeded, "JSON depth limit exceeded"});
            }
            auto valid = validateValueResources(item.second, limits, depth + 1, nodes);
            if (!valid) {
                return valid;
            }
        }
    }
    return Result<void>::ok();
}

void appendIndent(std::string& output, std::size_t depth) {
    output.append(depth, ' ');
}

std::size_t nestedDepth(std::size_t depth, std::size_t indent) {
    if (indent > std::numeric_limits<std::size_t>::max() - depth) {
        throw std::length_error("JSON indentation depth overflows");
    }
    return depth + indent;
}

void stringifyInto(std::string& output, const Json& value, std::size_t indent, std::size_t depth) {
    if (value.isNull()) {
        output += "null";
    } else if (value.isBool()) {
        output += value.asBool() ? "true" : "false";
    } else if (value.isNumber()) {
        output += numberText(value);
    } else if (value.isString()) {
        output.push_back('"');
        output += jsonEscape(value.asString());
        output.push_back('"');
    } else if (value.isArray()) {
        output.push_back('[');
        const auto& array = value.asArray();
        const auto childDepth = indent > 0 ? nestedDepth(depth, indent) : depth;
        for (std::size_t index = 0; index < array.size(); ++index) {
            if (index != 0) {
                output.push_back(',');
            }
            if (indent > 0) {
                output.push_back('\n');
                appendIndent(output, childDepth);
            }
            stringifyInto(output, array[index], indent, childDepth);
        }
        if (indent > 0 && !array.empty()) {
            output.push_back('\n');
            appendIndent(output, depth);
        }
        output.push_back(']');
    } else if (value.isObject()) {
        output.push_back('{');
        const auto& object = value.asObject();
        const auto childDepth = indent > 0 ? nestedDepth(depth, indent) : depth;
        std::size_t index = 0;
        for (const auto& item : object) {
            if (index++ != 0) {
                output.push_back(',');
            }
            if (indent > 0) {
                output.push_back('\n');
                appendIndent(output, childDepth);
            }
            output.push_back('"');
            output += jsonEscape(item.first);
            output += "\":";
            if (indent > 0) {
                output.push_back(' ');
            }
            stringifyInto(output, item.second, indent, childDepth);
        }
        if (indent > 0 && !object.empty()) {
            output.push_back('\n');
            appendIndent(output, depth);
        }
        output.push_back('}');
    }
}

bool limitsExceedSafetyCeiling(const JsonResourceLimits& limits) noexcept {
    return limits.maxDepth > JsonResourceLimits::absoluteMaxDepth ||
           limits.maxNodes > JsonResourceLimits::absoluteMaxNodes ||
           limits.maxStringBytes > JsonResourceLimits::absoluteMaxStringBytes ||
           limits.maxNumberBytes > JsonResourceLimits::absoluteMaxNumberBytes ||
           limits.maxContainerEntries > JsonResourceLimits::absoluteMaxContainerEntries;
}

} // namespace

Json::Json(std::nullptr_t) : storage_(nullptr) {}
Json::Json(bool value) : storage_(value) {}
Json::Json(double value) : storage_(value) {}
Json::Json(std::string value) : storage_(std::move(value)) {}
Json::Json(const char* value)
    : storage_(value != nullptr ? std::string(value) : throw std::invalid_argument("JSON string is null")) {}
Json::Json(Object value) : storage_(std::move(value)) {}
Json::Json(Array value) : storage_(std::move(value)) {}

Result<Json> Json::parse(const std::string& text, const JsonResourceLimits& limits) noexcept {
    try {
        if (limitsExceedSafetyCeiling(limits)) {
            return Result<Json>::fail({ErrorCode::InvalidConfig, "JSON resource limit exceeds the safety ceiling"});
        }
        if (!isValidUtf8(text)) {
            return Result<Json>::fail({ErrorCode::ManifestParseFailed, "JSON input is not valid UTF-8"});
        }
        return Parser(text, limits).parse();
    } catch (...) {
        return Result<Json>::fail({ErrorCode::ManifestParseFailed, "Unexpected JSON parser failure"});
    }
}

Result<void> Json::validateResourceUsage(const Json& value, const JsonResourceLimits& limits) noexcept {
    try {
        if (limitsExceedSafetyCeiling(limits)) {
            return Result<void>::fail({ErrorCode::InvalidConfig, "JSON resource limit exceeds the safety ceiling"});
        }
        std::size_t nodes = 0;
        return validateValueResources(value, limits, 1, nodes);
    } catch (...) {
        return Result<void>::fail({ErrorCode::ResourceLimitExceeded, "Failed to validate JSON resource usage"});
    }
}

bool Json::isNull() const noexcept {
    return std::holds_alternative<std::nullptr_t>(storage_);
}

bool Json::isBool() const noexcept {
    return std::holds_alternative<bool>(storage_);
}

bool Json::isNumber() const noexcept {
    return isInteger() || isFloatingPoint();
}

bool Json::isInteger() const noexcept {
    return isSignedInteger() || isUnsignedInteger();
}

bool Json::isSignedInteger() const noexcept {
    return std::holds_alternative<std::int64_t>(storage_);
}

bool Json::isUnsignedInteger() const noexcept {
    return std::holds_alternative<std::uint64_t>(storage_);
}

bool Json::isFloatingPoint() const noexcept {
    return std::holds_alternative<double>(storage_);
}

bool Json::isString() const noexcept {
    return std::holds_alternative<std::string>(storage_);
}

bool Json::isObject() const noexcept {
    return std::holds_alternative<Object>(storage_);
}

bool Json::isArray() const noexcept {
    return std::holds_alternative<Array>(storage_);
}

bool Json::asBool(bool fallback) const noexcept {
    return isBool() ? std::get<bool>(storage_) : fallback;
}

double Json::asNumber(double fallback) const noexcept {
    if (isFloatingPoint()) {
        const auto value = std::get<double>(storage_);
        return std::isfinite(value) ? value : fallback;
    }
    if (isSignedInteger()) {
        const auto value = std::get<std::int64_t>(storage_);
        const auto magnitude = unsignedMagnitude(value);
        return isExactlyRepresentableAsDouble(magnitude) ? static_cast<double>(value) : fallback;
    }
    if (isUnsignedInteger()) {
        const auto value = std::get<std::uint64_t>(storage_);
        return isExactlyRepresentableAsDouble(value) ? static_cast<double>(value) : fallback;
    }
    return fallback;
}

double Json::asDouble(double fallback) const noexcept {
    return isFloatingPoint() ? std::get<double>(storage_) : fallback;
}

std::int64_t Json::asInt(std::int64_t fallback) const noexcept {
    return asInt64(fallback);
}

std::int64_t Json::asInt64(std::int64_t fallback) const noexcept {
    if (isSignedInteger()) {
        return std::get<std::int64_t>(storage_);
    }
    if (isUnsignedInteger()) {
        const auto value = std::get<std::uint64_t>(storage_);
        if (value <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            return static_cast<std::int64_t>(value);
        }
    }
    return fallback;
}

std::uint64_t Json::asUInt64(std::uint64_t fallback) const noexcept {
    if (isUnsignedInteger()) {
        return std::get<std::uint64_t>(storage_);
    }
    if (isSignedInteger()) {
        const auto value = std::get<std::int64_t>(storage_);
        if (value >= 0) {
            return static_cast<std::uint64_t>(value);
        }
    }
    return fallback;
}

const std::string& Json::asString() const noexcept {
    return isString() ? std::get<std::string>(storage_) : kEmptyString;
}

const Json::Object& Json::asObject() const noexcept {
    return isObject() ? std::get<Object>(storage_) : kEmptyObject;
}

const Json::Array& Json::asArray() const noexcept {
    return isArray() ? std::get<Array>(storage_) : kEmptyArray;
}

const Json* Json::get(const std::string& key) const noexcept {
    if (!isObject()) {
        return nullptr;
    }
    const auto& object = std::get<Object>(storage_);
    const auto found = object.find(key);
    return found == object.end() ? nullptr : &found->second;
}

bool Json::contains(const std::string& key) const noexcept {
    return get(key) != nullptr;
}

std::string Json::stringify(int indent) const {
    std::string output;
    stringifyInto(output, *this, indent > 0 ? static_cast<std::size_t>(indent) : 0, 0);
    return output;
}

std::string jsonEscape(const std::string& text) {
    if (!isValidUtf8(text)) {
        throw std::invalid_argument("JSON string is not valid UTF-8");
    }

    constexpr char hexDigits[] = "0123456789abcdef";
    std::string output;
    output.reserve(text.size());
    for (const unsigned char value : text) {
        switch (value) {
        case '"':
            output += "\\\"";
            break;
        case '\\':
            output += "\\\\";
            break;
        case '\b':
            output += "\\b";
            break;
        case '\f':
            output += "\\f";
            break;
        case '\n':
            output += "\\n";
            break;
        case '\r':
            output += "\\r";
            break;
        case '\t':
            output += "\\t";
            break;
        default:
            if (value < 0x20U) {
                output += "\\u00";
                output.push_back(hexDigits[(value >> 4U) & 0x0FU]);
                output.push_back(hexDigits[value & 0x0FU]);
            } else {
                output.push_back(static_cast<char>(value));
            }
            break;
        }
    }
    return output;
}

} // namespace autoupdater::util
