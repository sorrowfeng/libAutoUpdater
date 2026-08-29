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

// Bounds for safe double -> int64_t conversion. Both are exactly
// representable as double; a value v is convertible iff
// kInt64MinAsDouble <= v < kInt64MaxPlusOneAsDouble.
constexpr double kInt64MinAsDouble = -9223372036854775808.0;       // -2^63
constexpr double kInt64MaxPlusOneAsDouble = 9223372036854775808.0; // 2^63

bool doubleFitsInt64(double value) {
    return std::isfinite(value) && std::trunc(value) == value && value >= kInt64MinAsDouble &&
           value < kInt64MaxPlusOneAsDouble;
}

void appendUtf8(std::string& out, std::uint32_t codePoint) {
    if (codePoint <= 0x7F) {
        out.push_back(static_cast<char>(codePoint));
    } else if (codePoint <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | (codePoint >> 6)));
        out.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
    } else if (codePoint <= 0xFFFF) {
        out.push_back(static_cast<char>(0xE0 | (codePoint >> 12)));
        out.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (codePoint >> 18)));
        out.push_back(static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
    }
}

class Parser {
  public:
    explicit Parser(const std::string& input) : input_(input) {}

    Result<Json> parse() {
        skipWhitespace();
        auto value = parseValue();
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
    static constexpr int kMaxDepth = 256;

    Result<Json> parseValue() {
        skipWhitespace();
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
            return parseObject();
        }
        if (c == '[') {
            return parseArray();
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

    Result<Json> parseObject() {
        if (++depth_ > kMaxDepth) {
            return fail("JSON nesting too deep");
        }
        ++pos_;
        Json::Object object;
        skipWhitespace();
        if (consume('}')) {
            --depth_;
            return Result<Json>::ok(Json(std::move(object)));
        }

        while (true) {
            skipWhitespace();
            if (pos_ >= input_.size() || input_[pos_] != '"') {
                --depth_;
                return fail("Expected object key");
            }
            auto key = parseString();
            if (!key) {
                --depth_;
                return Result<Json>::fail(key.error());
            }
            skipWhitespace();
            if (!consume(':')) {
                --depth_;
                return fail("Expected ':' after object key");
            }
            auto value = parseValue();
            if (!value) {
                --depth_;
                return value;
            }
            object.emplace(std::move(key.value()), std::move(value.value()));
            skipWhitespace();
            if (consume('}')) {
                break;
            }
            if (!consume(',')) {
                --depth_;
                return fail("Expected ',' or '}' in object");
            }
        }
        --depth_;
        return Result<Json>::ok(Json(std::move(object)));
    }

    Result<Json> parseArray() {
        if (++depth_ > kMaxDepth) {
            return fail("JSON nesting too deep");
        }
        ++pos_;
        Json::Array array;
        skipWhitespace();
        if (consume(']')) {
            --depth_;
            return Result<Json>::ok(Json(std::move(array)));
        }

        while (true) {
            auto value = parseValue();
            if (!value) {
                --depth_;
                return value;
            }
            array.push_back(std::move(value.value()));
            skipWhitespace();
            if (consume(']')) {
                break;
            }
            if (!consume(',')) {
                --depth_;
                return fail("Expected ',' or ']' in array");
            }
        }
        --depth_;
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
                ++pos_;
            }
        }

        try {
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
                    out.push_back('"');
                    break;
                case '\\':
                    out.push_back('\\');
                    break;
                case '/':
                    out.push_back('/');
                    break;
                case 'b':
                    out.push_back('\b');
                    break;
                case 'f':
                    out.push_back('\f');
                    break;
                case 'n':
                    out.push_back('\n');
                    break;
                case 'r':
                    out.push_back('\r');
                    break;
                case 't':
                    out.push_back('\t');
                    break;
                case 'u': {
                    std::uint32_t codePoint = 0;
                    if (!parseHex4(codePoint)) {
                        return Result<std::string>::fail({ErrorCode::ManifestParseFailed, "Invalid unicode escape"});
                    }
                    if (codePoint >= 0xD800 && codePoint <= 0xDBFF) {
                        // High surrogate: require a following \uXXXX low surrogate.
                        if (pos_ + 1 >= input_.size() || input_[pos_] != '\\' || input_[pos_ + 1] != 'u') {
                            return Result<std::string>::fail(
                                {ErrorCode::ManifestParseFailed, "Unpaired high surrogate in unicode escape"});
                        }
                        pos_ += 2;
                        std::uint32_t low = 0;
                        if (!parseHex4(low) || low < 0xDC00 || low > 0xDFFF) {
                            return Result<std::string>::fail(
                                {ErrorCode::ManifestParseFailed, "Invalid low surrogate in unicode escape"});
                        }
                        codePoint = 0x10000 + ((codePoint - 0xD800) << 10) + (low - 0xDC00);
                    } else if (codePoint >= 0xDC00 && codePoint <= 0xDFFF) {
                        return Result<std::string>::fail(
                            {ErrorCode::ManifestParseFailed, "Unpaired low surrogate in unicode escape"});
                    }
                    appendUtf8(out, codePoint);
                    break;
                }
                default:
                    return Result<std::string>::fail({ErrorCode::ManifestParseFailed, "Invalid string escape"});
                }
            } else if (static_cast<unsigned char>(c) < 0x20) {
                return Result<std::string>::fail(
                    {ErrorCode::ManifestParseFailed, "Unescaped control character in string"});
            } else {
                out.push_back(c);
            }
        }
        return Result<std::string>::fail({ErrorCode::ManifestParseFailed, "Unterminated string"});
    }

    bool parseHex4(std::uint32_t& value) {
        if (pos_ + 4 > input_.size()) {
            return false;
        }
        value = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = input_[pos_++];
            value <<= 4;
            if (c >= '0' && c <= '9') {
                value |= static_cast<std::uint32_t>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                value |= static_cast<std::uint32_t>(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                value |= static_cast<std::uint32_t>(c - 'A' + 10);
            } else {
                return false;
            }
        }
        return true;
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
        // RFC 8259 allows only space, tab, LF and CR as JSON whitespace.
        while (pos_ < input_.size()) {
            const char c = input_[pos_];
            if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
                break;
            }
            ++pos_;
        }
    }

    Result<Json> fail(const std::string& message) const {
        return Result<Json>::fail({ErrorCode::ManifestParseFailed, message});
    }

    const std::string& input_;
    std::size_t pos_ = 0;
    int depth_ = 0;
};

std::string indentText(int depth) {
    return std::string(static_cast<std::size_t>(depth), ' ');
}

void stringifyInto(std::ostringstream& stream, const Json& value, int indent, int depth) {
    if (value.isNull()) {
        stream << "null";
    } else if (value.isBool()) {
        stream << (value.asBool() ? "true" : "false");
    } else if (value.isNumber()) {
        const double number = value.asNumber();
        if (doubleFitsInt64(number)) {
            stream << static_cast<std::int64_t>(number);
        } else {
            stream << std::setprecision(17) << number;
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

Result<Json> Json::parse(const std::string& text) noexcept {
    try {
        return Parser(text).parse();
    } catch (...) {
        return Result<Json>::fail({ErrorCode::ManifestParseFailed, "Unexpected JSON parser failure"});
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
    if (!doubleFitsInt64(value)) {
        return fallback;
    }
    return static_cast<std::int64_t>(value);
}

bool Json::asUInt64(std::uint64_t& out) const noexcept {
    if (!isNumber()) {
        return false;
    }
    const double value = std::get<double>(storage_);
    // 2^64 is exactly representable as double; any finite integral double in
    // [0, 2^64) converts to std::uint64_t without undefined behavior.
    constexpr double kUInt64MaxPlusOneAsDouble = 18446744073709551616.0; // 2^64
    if (!std::isfinite(value) || std::trunc(value) != value || value < 0.0 || value >= kUInt64MaxPlusOneAsDouble) {
        return false;
    }
    out = static_cast<std::uint64_t>(value);
    return true;
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
