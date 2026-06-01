#pragma once

#include "libAutoUpdater/Result.h"

#include <cstdint>
#include <map>
#include <string>
#include <variant>
#include <vector>

namespace autoupdater::util {

class Json {
public:
    using Object = std::map<std::string, Json>;
    using Array = std::vector<Json>;

    Json() = default;
    Json(std::nullptr_t);
    Json(bool value);
    Json(double value);
    Json(std::string value);
    Json(const char* value);
    Json(Object value);
    Json(Array value);

    static Result<Json> parse(const std::string& text) noexcept;

    bool isNull() const noexcept;
    bool isBool() const noexcept;
    bool isNumber() const noexcept;
    bool isString() const noexcept;
    bool isObject() const noexcept;
    bool isArray() const noexcept;

    bool asBool(bool fallback = false) const noexcept;
    double asNumber(double fallback = 0) const noexcept;
    std::int64_t asInt(std::int64_t fallback = 0) const noexcept;
    const std::string& asString() const noexcept;
    const Object& asObject() const noexcept;
    const Array& asArray() const noexcept;

    const Json* get(const std::string& key) const noexcept;
    bool contains(const std::string& key) const noexcept;

    std::string stringify(int indent = 0) const;

private:
    using Storage = std::variant<std::nullptr_t, bool, double, std::string, Object, Array>;
    Storage storage_ = nullptr;
};

std::string jsonEscape(const std::string& text);

} // namespace autoupdater::util

