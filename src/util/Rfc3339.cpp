#include "util/Rfc3339.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <ctime>

namespace autoupdater::util {

namespace {

Result<UtcInstant> invalidTimestamp() {
    return Result<UtcInstant>::fail(
        {ErrorCode::ManifestParseFailed,
         "Timestamp must use YYYY-MM-DDTHH:MM:SS[.1-9DIGIT](Z|+/-HH:MM) with a valid Gregorian date"});
}

Result<UtcInstant> clockConversionFailed() {
    return Result<UtcInstant>::fail({ErrorCode::InternalError, "The system wall clock could not be converted to UTC"});
}

bool parseDigits(std::string_view text, std::size_t offset, std::size_t count, unsigned int& value) {
    if (offset > text.size() || count > text.size() - offset) {
        return false;
    }
    value = 0;
    for (std::size_t index = 0; index < count; ++index) {
        const char character = text[offset + index];
        if (character < '0' || character > '9') {
            return false;
        }
        value = value * 10U + static_cast<unsigned int>(character - '0');
    }
    return true;
}

bool isLeapYear(unsigned int year) {
    return year % 4U == 0U && (year % 100U != 0U || year % 400U == 0U);
}

unsigned int daysInMonth(unsigned int year, unsigned int month) {
    constexpr unsigned int days[] = {31U, 28U, 31U, 30U, 31U, 30U, 31U, 31U, 30U, 31U, 30U, 31U};
    if (month == 2U && isLeapYear(year)) {
        return 29U;
    }
    return days[month - 1U];
}

// Gregorian civil date to days since 1970-01-01. This is defined for the
// complete accepted year range and does not depend on time_t or the locale.
std::int64_t daysFromCivil(std::int64_t year, unsigned int month, unsigned int day) {
    year -= month <= 2U ? 1 : 0;
    const std::int64_t era = (year >= 0 ? year : year - 399) / 400;
    const auto yearOfEra = static_cast<unsigned int>(year - era * 400);
    const auto adjustedMonth = static_cast<int>(month) + (month > 2U ? -3 : 9);
    const auto dayOfYear = static_cast<unsigned int>((153 * adjustedMonth + 2) / 5) + day - 1U;
    const auto dayOfEra = yearOfEra * 365U + yearOfEra / 4U - yearOfEra / 100U + dayOfYear;
    return era * 146097 + static_cast<std::int64_t>(dayOfEra) - 719468;
}

} // namespace

Result<UtcInstant> parseRfc3339(std::string_view text) noexcept {
    try {
        if (text.size() < 20 || text[4] != '-' || text[7] != '-' || text[10] != 'T' || text[13] != ':' ||
            text[16] != ':') {
            return invalidTimestamp();
        }

        unsigned int year = 0;
        unsigned int month = 0;
        unsigned int day = 0;
        unsigned int hour = 0;
        unsigned int minute = 0;
        unsigned int second = 0;
        if (!parseDigits(text, 0, 4, year) || !parseDigits(text, 5, 2, month) || !parseDigits(text, 8, 2, day) ||
            !parseDigits(text, 11, 2, hour) || !parseDigits(text, 14, 2, minute) || !parseDigits(text, 17, 2, second) ||
            year == 0U || month == 0U || month > 12U || day == 0U || day > daysInMonth(year, month) || hour > 23U ||
            minute > 59U || second > 59U) {
            return invalidTimestamp();
        }

        std::size_t position = 19;
        std::uint32_t nanoseconds = 0;
        if (position < text.size() && text[position] == '.') {
            ++position;
            const std::size_t fractionStart = position;
            std::uint32_t scale = 100000000U;
            while (position < text.size() && text[position] >= '0' && text[position] <= '9') {
                if (position - fractionStart >= 9) {
                    return invalidTimestamp();
                }
                nanoseconds += static_cast<std::uint32_t>(text[position] - '0') * scale;
                scale /= 10U;
                ++position;
            }
            if (position == fractionStart) {
                return invalidTimestamp();
            }
        }

        int offsetSeconds = 0;
        if (position < text.size() && text[position] == 'Z') {
            ++position;
        } else if (position < text.size() && (text[position] == '+' || text[position] == '-')) {
            const bool negativeOffset = text[position] == '-';
            unsigned int offsetHour = 0;
            unsigned int offsetMinute = 0;
            if (position + 6 > text.size() || text[position + 3] != ':' ||
                !parseDigits(text, position + 1, 2, offsetHour) || !parseDigits(text, position + 4, 2, offsetMinute) ||
                offsetHour > 23U || offsetMinute > 59U || (negativeOffset && offsetHour == 0U && offsetMinute == 0U)) {
                return invalidTimestamp();
            }
            offsetSeconds = static_cast<int>(offsetHour * 3600U + offsetMinute * 60U);
            if (negativeOffset) {
                offsetSeconds = -offsetSeconds;
            }
            position += 6;
        } else {
            return invalidTimestamp();
        }
        if (position != text.size()) {
            return invalidTimestamp();
        }

        const std::int64_t localSeconds = daysFromCivil(year, month, day) * 86400 +
                                          static_cast<std::int64_t>(hour) * 3600 +
                                          static_cast<std::int64_t>(minute) * 60 + second;
        return Result<UtcInstant>::ok({localSeconds - offsetSeconds, nanoseconds});
    } catch (...) {
        return invalidTimestamp();
    }
}

Result<UtcInstant> currentUtcInstant() noexcept {
    try {
        const auto now = std::chrono::system_clock::now();
        const std::time_t coarseTime = std::chrono::system_clock::to_time_t(now);
        std::tm utc{};
#ifdef _WIN32
        if (::gmtime_s(&utc, &coarseTime) != 0) {
            return clockConversionFailed();
        }
#else
        if (::gmtime_r(&coarseTime, &utc) == nullptr) {
            return clockConversionFailed();
        }
#endif

        const std::int64_t year = static_cast<std::int64_t>(utc.tm_year) + 1900;
        const auto month = static_cast<unsigned int>(utc.tm_mon + 1);
        const auto day = static_cast<unsigned int>(utc.tm_mday);
        if (year < 1 || year > 9999 || month == 0U || month > 12U || day == 0U ||
            day > daysInMonth(static_cast<unsigned int>(year), month) || utc.tm_hour < 0 || utc.tm_hour > 23 ||
            utc.tm_min < 0 || utc.tm_min > 59 || utc.tm_sec < 0 || utc.tm_sec > 59) {
            return clockConversionFailed();
        }

        const std::int64_t coarseUnixSeconds = daysFromCivil(year, month, day) * 86400 +
                                               static_cast<std::int64_t>(utc.tm_hour) * 3600 +
                                               static_cast<std::int64_t>(utc.tm_min) * 60 + utc.tm_sec;

        // C++17 does not require system_clock's epoch to be 1970. Anchor the
        // coarse value through UTC civil time, then retain the clock's
        // sub-second precision relative to the corresponding time_t value.
        auto delta = now - std::chrono::system_clock::from_time_t(coarseTime);
        auto wholeDelta = std::chrono::duration_cast<std::chrono::seconds>(delta);
        auto remainder = delta - wholeDelta;
        if (remainder < decltype(remainder)::zero()) {
            wholeDelta -= std::chrono::seconds(1);
            remainder += std::chrono::seconds(1);
        }
        const auto nanoseconds = std::chrono::duration_cast<std::chrono::nanoseconds>(remainder).count();
        if (nanoseconds < 0 || nanoseconds >= 1000000000) {
            return clockConversionFailed();
        }
        return Result<UtcInstant>::ok({coarseUnixSeconds + static_cast<std::int64_t>(wholeDelta.count()),
                                       static_cast<std::uint32_t>(nanoseconds)});
    } catch (...) {
        return clockConversionFailed();
    }
}

} // namespace autoupdater::util
