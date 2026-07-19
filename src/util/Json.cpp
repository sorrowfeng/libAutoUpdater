#include "util/Json.h"

#include <cctype>
#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>

namespace autoupdater::util {

namespace {

const std::string kEmptyString;
const Json::Object kEmptyObject;
const Json::Array kEmptyArray;

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

        const char c = input_[pos_];
        if (c == '"') {
            auto value = parseString();
            if (!value) {
                return Result<Json>::fail(value.error());
            }
            return Result<Json>::ok(Json(value.value()));
        }
        if (c == '{') {
            return parseObject(depth);
        }
        if (c == '[') {
            return parseArray(depth);
        }
        if (c == '-' || std::isdigit(static_cast<unsigned char>(c))) {
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
        if (input_[pos_] == '-') {
            ++pos_;
        }
        if (pos_ >= input_.size()) {
            return fail("Invalid number");
        }
        if (input_[pos_] == '0') {
            ++pos_;
        } else if (std::isdigit(static_cast<unsigned char>(input_[pos_]))) {
            while (pos_ < input_.size() && std::isdigit(static_cast<unsigned char>(input_[pos_]))) {
                if (pos_ - start >= limits_.maxNumberBytes) {
                    return limit("JSON number length limit exceeded");
                }
                ++pos_;
            }
        } else {
            return fail("Invalid number");
        }

        if (pos_ < input_.size() && input_[pos_] == '.') {
            ++pos_;
            if (pos_ >= input_.size() || !std::isdigit(static_cast<unsigned char>(input_[pos_]))) {
                return fail("Invalid number fraction");
            }
            while (pos_ < input_.size() && std::isdigit(static_cast<unsigned char>(input_[pos_]))) {
                if (pos_ - start >= limits_.maxNumberBytes) {
                    return limit("JSON number length limit exceeded");
                }
                ++pos_;
            }
        }

        if (pos_ < input_.size() && (input_[pos_] == 'e' || input_[pos_] == 'E')) {
            ++pos_;
            if (pos_ < input_.size() && (input_[pos_] == '+' || input_[pos_] == '-')) {
                ++pos_;
            }
            if (pos_ >= input_.size() || !std::isdigit(static_cast<unsigned char>(input_[pos_]))) {
                return fail("Invalid number exponent");
            }
            while (pos_ < input_.size() && std::isdigit(static_cast<unsigned char>(input_[pos_]))) {
                if (pos_ - start >= limits_.maxNumberBytes) {
                    return limit("JSON number length limit exceeded");
                }
                ++pos_;
            }
        }

        try {
            if (pos_ - start > limits_.maxNumberBytes) {
                return limit("JSON number length limit exceeded");
            }
            const auto text = input_.substr(start, pos_ - start);
            return Result<Json>::ok(Json(std::stod(text)));
        } catch (...) {
            return fail("Invalid number");
        }
    }

    Result<std::string> parseString() {
        if (!consume('"')) {
            return Result<std::string>::fail({ErrorCode::ManifestParseFailed, "Expected string"});
        }
        std::string out;
        while (pos_ < input_.size()) {
            const char c = input_[pos_++];
            if (c == '"') {
                return Result<std::string>::ok(std::move(out));
            }
            if (c == '\\') {
                if (pos_ >= input_.size()) {
                    return Result<std::string>::fail({ErrorCode::ManifestParseFailed, "Invalid string escape"});
                }
                const char esc = input_[pos_++];
                switch (esc) {
                case '"':
                    if (!append(out, '"')) {
                        return stringLimit();
                    }
                    break;
                case '\\':
                    if (!append(out, '\\')) {
                        return stringLimit();
                    }
                    break;
                case '/':
                    if (!append(out, '/')) {
                        return stringLimit();
                    }
                    break;
                case 'b':
                    if (!append(out, '\b')) {
                        return stringLimit();
                    }
                    break;
                case 'f':
                    if (!append(out, '\f')) {
                        return stringLimit();
                    }
                    break;
                case 'n':
                    if (!append(out, '\n')) {
                        return stringLimit();
                    }
                    break;
                case 'r':
                    if (!append(out, '\r')) {
                        return stringLimit();
                    }
                    break;
                case 't':
                    if (!append(out, '\t')) {
                        return stringLimit();
                    }
                    break;
                case 'u':
                    if (pos_ + 4 > input_.size()) {
                        return Result<std::string>::fail({ErrorCode::ManifestParseFailed, "Invalid unicode escape"});
                    }
                    if (limits_.maxStringBytes - out.size() < 6) {
                        return stringLimit();
                    }
                    out.append("\\u");
                    out.append(input_, pos_, 4);
                    pos_ += 4;
                    break;
                default:
                    return Result<std::string>::fail({ErrorCode::ManifestParseFailed, "Invalid string escape"});
                }
            } else {
                if (!append(out, c)) {
                    return stringLimit();
                }
            }
        }
        return Result<std::string>::fail({ErrorCode::ManifestParseFailed, "Unterminated string"});
    }

    bool consume(char expected) {
        if (pos_ < input_.size() && input_[pos_] == expected) {
            ++pos_;
            return true;
        }
        return false;
    }

    bool match(const char* keyword) {
        const std::string text(keyword);
        if (input_.compare(pos_, text.size(), text) == 0) {
            pos_ += text.size();
            return true;
        }
        return false;
    }

    void skipWhitespace() {
        while (pos_ < input_.size() && std::isspace(static_cast<unsigned char>(input_[pos_]))) {
            ++pos_;
        }
    }

    Result<Json> fail(const std::string& message) const {
        return Result<Json>::fail({ErrorCode::ManifestParseFailed, message});
    }

    Result<Json> limit(const std::string& message) const {
        return Result<Json>::fail({ErrorCode::ResourceLimitExceeded, message});
    }

    bool append(std::string& output, char value) const {
        if (output.size() >= limits_.maxStringBytes) {
            return false;
        }
        output.push_back(value);
        return true;
    }

    Result<std::string> stringLimit() const {
        return Result<std::string>::fail({ErrorCode::ResourceLimitExceeded, "JSON string limit exceeded"});
    }

    const std::string& input_;
    const JsonResourceLimits& limits_;
    std::size_t pos_ = 0;
    std::size_t nodeCount_ = 0;
};

std::string indentText(int depth) {
    return std::string(static_cast<std::size_t>(depth), ' ');
}

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
        if (value.asString().size() > limits.maxStringBytes) {
            return Result<void>::fail({ErrorCode::ResourceLimitExceeded, "JSON string limit exceeded"});
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

void stringifyInto(std::ostringstream& stream, const Json& value, int indent, int depth) {
    if (value.isNull()) {
        stream << "null";
    } else if (value.isBool()) {
        stream << (value.asBool() ? "true" : "false");
    } else if (value.isNumber()) {
        const double number = value.asNumber();
        const auto int64Lower = static_cast<double>(std::numeric_limits<std::int64_t>::min());
        const auto int64UpperExclusive = -int64Lower;
        if (std::isfinite(number) && std::floor(number) == number && number >= int64Lower &&
            number < int64UpperExclusive) {
            stream << static_cast<std::int64_t>(number);
        } else {
            stream << std::setprecision(15) << number;
        }
    } else if (value.isString()) {
        stream << '"' << jsonEscape(value.asString()) << '"';
    } else if (value.isArray()) {
        stream << '[';
        const auto& array = value.asArray();
        for (std::size_t i = 0; i < array.size(); ++i) {
            if (i != 0) {
                stream << ',';
            }
            if (indent > 0) {
                stream << '\n' << indentText(depth + indent);
            }
            stringifyInto(stream, array[i], indent, depth + indent);
        }
        if (indent > 0 && !array.empty()) {
            stream << '\n' << indentText(depth);
        }
        stream << ']';
    } else if (value.isObject()) {
        stream << '{';
        const auto& object = value.asObject();
        std::size_t index = 0;
        for (const auto& item : object) {
            if (index++ != 0) {
                stream << ',';
            }
            if (indent > 0) {
                stream << '\n' << indentText(depth + indent);
            }
            stream << '"' << jsonEscape(item.first) << "\":";
            if (indent > 0) {
                stream << ' ';
            }
            stringifyInto(stream, item.second, indent, depth + indent);
        }
        if (indent > 0 && !object.empty()) {
            stream << '\n' << indentText(depth);
        }
        stream << '}';
    }
}

} // namespace

Json::Json(std::nullptr_t) : storage_(nullptr) {}
Json::Json(bool value) : storage_(value) {}
Json::Json(double value) : storage_(value) {}
Json::Json(std::string value) : storage_(std::move(value)) {}
Json::Json(const char* value) : storage_(std::string(value)) {}
Json::Json(Object value) : storage_(std::move(value)) {}
Json::Json(Array value) : storage_(std::move(value)) {}

Result<Json> Json::parse(const std::string& text, const JsonResourceLimits& limits) noexcept {
    try {
        if (limits.maxDepth > JsonResourceLimits::absoluteMaxDepth ||
            limits.maxNodes > JsonResourceLimits::absoluteMaxNodes ||
            limits.maxStringBytes > JsonResourceLimits::absoluteMaxStringBytes ||
            limits.maxNumberBytes > JsonResourceLimits::absoluteMaxNumberBytes ||
            limits.maxContainerEntries > JsonResourceLimits::absoluteMaxContainerEntries) {
            return Result<Json>::fail({ErrorCode::InvalidConfig, "JSON resource limit exceeds the safety ceiling"});
        }
        return Parser(text, limits).parse();
    } catch (...) {
        return Result<Json>::fail({ErrorCode::ManifestParseFailed, "Unexpected JSON parser failure"});
    }
}

Result<void> Json::validateResourceUsage(const Json& value, const JsonResourceLimits& limits) noexcept {
    try {
        if (limits.maxDepth > JsonResourceLimits::absoluteMaxDepth ||
            limits.maxNodes > JsonResourceLimits::absoluteMaxNodes ||
            limits.maxStringBytes > JsonResourceLimits::absoluteMaxStringBytes ||
            limits.maxNumberBytes > JsonResourceLimits::absoluteMaxNumberBytes ||
            limits.maxContainerEntries > JsonResourceLimits::absoluteMaxContainerEntries) {
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
    return isNumber() ? std::get<double>(storage_) : fallback;
}

std::int64_t Json::asInt(std::int64_t fallback) const noexcept {
    if (!isNumber()) {
        return fallback;
    }
    const double value = std::get<double>(storage_);
    if (!std::isfinite(value) || std::floor(value) != value ||
        value < static_cast<double>(std::numeric_limits<std::int64_t>::min()) ||
        value >= -static_cast<double>(std::numeric_limits<std::int64_t>::min())) {
        return fallback;
    }
    return static_cast<std::int64_t>(value);
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
    const auto it = object.find(key);
    return it == object.end() ? nullptr : &it->second;
}

bool Json::contains(const std::string& key) const noexcept {
    return get(key) != nullptr;
}

std::string Json::stringify(int indent) const {
    std::ostringstream stream;
    stringifyInto(stream, *this, indent, 0);
    return stream.str();
}

std::string jsonEscape(const std::string& text) {
    std::ostringstream stream;
    for (const char c : text) {
        switch (c) {
        case '"':
            stream << "\\\"";
            break;
        case '\\':
            stream << "\\\\";
            break;
        case '\b':
            stream << "\\b";
            break;
        case '\f':
            stream << "\\f";
            break;
        case '\n':
            stream << "\\n";
            break;
        case '\r':
            stream << "\\r";
            break;
        case '\t':
            stream << "\\t";
            break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                stream << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                       << static_cast<int>(static_cast<unsigned char>(c)) << std::dec << std::setfill(' ');
            } else {
                stream << c;
            }
            break;
        }
    }
    return stream.str();
}

} // namespace autoupdater::util
