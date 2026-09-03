#pragma once

#include "libAutoUpdater/Result.h"
#include "libAutoUpdater/ResourceLimits.h"

#include <cstdint>
#include <map>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

namespace autoupdater::util {

class Json {
  public:
    using Object = std::map<std::string, Json>;
    using Array = std::vector<Json>;

  private:
    using Storage = std::variant<std::nullptr_t, bool, std::int64_t, std::uint64_t, double, std::string, Object, Array>;

    template <typename Integer> static Storage integerStorage(Integer value) noexcept {
        static_assert(sizeof(Integer) <= sizeof(std::uint64_t), "JSON integers wider than 64 bits are unsupported");
        if constexpr (std::is_signed<Integer>::value) {
            return Storage(std::in_place_type<std::int64_t>, static_cast<std::int64_t>(value));
        } else {
            return Storage(std::in_place_type<std::uint64_t>, static_cast<std::uint64_t>(value));
        }
    }

  public:
    Json() = default;
    Json(std::nullptr_t);
    Json(bool value);
    template <typename Integer,
              typename std::enable_if<std::is_integral<Integer>::value &&
                                          !std::is_same<typename std::remove_cv<Integer>::type, bool>::value,
                                      int>::type = 0>
    Json(Integer value) noexcept : storage_(integerStorage(value)) {}
    Json(double value);
    Json(std::string value);
    Json(const char* value);
    Json(Object value);
    Json(Array value);

    static Result<Json> parse(const std::string& text, const JsonResourceLimits& limits) noexcept;
    static Result<void> validateResourceUsage(const Json& value, const JsonResourceLimits& limits) noexcept;

    bool isNull() const noexcept;
    bool isBool() const noexcept;
    bool isNumber() const noexcept;
    bool isInteger() const noexcept;
    bool isSignedInteger() const noexcept;
    bool isUnsignedInteger() const noexcept;
    bool isFloatingPoint() const noexcept;
    bool isString() const noexcept;
    bool isObject() const noexcept;
    bool isArray() const noexcept;

    bool asBool(bool fallback = false) const noexcept;
    double asNumber(double fallback = 0) const noexcept;
    double asDouble(double fallback = 0) const noexcept;
    std::int64_t asInt(std::int64_t fallback = 0) const noexcept;
    std::int64_t asInt64(std::int64_t fallback = 0) const noexcept;
    std::uint64_t asUInt64(std::uint64_t fallback = 0) const noexcept;
    const std::string& asString() const noexcept;
    const Object& asObject() const noexcept;
    const Array& asArray() const noexcept;

    const Json* get(const std::string& key) const noexcept;
    bool contains(const std::string& key) const noexcept;

    std::string stringify(int indent = 0) const;

  private:
    Storage storage_ = nullptr;
};

std::string jsonEscape(const std::string& text);

} // namespace autoupdater::util
