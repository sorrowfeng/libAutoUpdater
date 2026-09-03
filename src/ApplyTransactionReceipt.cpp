#include "ApplyTransactionReceipt.h"

#include "util/Json.h"
#include "util/Sha256.h"

#include <array>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>

namespace autoupdater {

namespace {

constexpr std::uint64_t kLegacyReceiptSchemaVersion = 2;
constexpr std::uint64_t kReceiptSchemaVersion = 3;
constexpr std::uint64_t kMaxReceiptBytes = 256 * 1024;
constexpr std::size_t kReadChunkBytes = 64 * 1024;
constexpr const char* kTerminalReceiptPath = ".autoupdater/journal/terminal.json";
constexpr std::int64_t kMinimumRfc3339UnixSeconds = INT64_C(-62135596800);
constexpr std::int64_t kMaximumRfc3339UnixSeconds = INT64_C(253402300799);

Error receiptError(const std::string& message) {
    return {ErrorCode::ApplyFailed, message};
}

JsonResourceLimits receiptJsonLimits() {
    JsonResourceLimits limits;
    limits.maxDepth = 8;
    limits.maxNodes = 16;
    limits.maxStringBytes = static_cast<std::size_t>(kMaxReceiptBytes);
    limits.maxNumberBytes = 32;
    limits.maxContainerEntries = 8;
    return limits;
}

Result<std::string> requiredString(const util::Json& object, const std::string& key) {
    const auto* value = object.get(key);
    if (!value || !value->isString()) {
        return Result<std::string>::fail(receiptError("Apply receipt string field is missing: " + key));
    }
    return Result<std::string>::ok(value->asString());
}

Result<void> validateReceiptKeys(const util::Json::Object& object, std::uint64_t schemaVersion) {
    const std::size_t expectedSize = schemaVersion == kReceiptSchemaVersion ? 4 : 3;
    if (object.size() != expectedSize || object.find("schemaVersion") == object.end() ||
        object.find("transactionId") == object.end() || object.find("planDigest") == object.end() ||
        (schemaVersion == kReceiptSchemaVersion && object.find("completedAt") == object.end())) {
        return Result<void>::fail(receiptError("Terminal apply receipt contains unknown or missing fields"));
    }
    return Result<void>::ok();
}

Result<std::string> readBoundedFile(IRootedFile& file, std::uint64_t maxBytes, const std::string& description) {
    auto metadata = file.metadata();
    if (!metadata) {
        return Result<std::string>::fail(metadata.error());
    }
    if (metadata.value().size > maxBytes || metadata.value().size > std::numeric_limits<std::size_t>::max()) {
        return Result<std::string>::fail({ErrorCode::ResourceLimitExceeded, description + " exceeds its byte limit"});
    }
    auto rewound = file.seek(0);
    if (!rewound) {
        return Result<std::string>::fail(rewound.error());
    }
    std::string contents;
    contents.reserve(static_cast<std::size_t>(metadata.value().size));
    std::array<char, kReadChunkBytes> buffer{};
    for (;;) {
        auto read = file.read(buffer.data(), buffer.size());
        if (!read) {
            return Result<std::string>::fail(read.error());
        }
        if (read.value() == 0) {
            break;
        }
        if (contents.size() > maxBytes || read.value() > maxBytes - contents.size()) {
            return Result<std::string>::fail(
                {ErrorCode::ResourceLimitExceeded, description + " grew beyond its byte limit"});
        }
        contents.append(buffer.data(), read.value());
    }
    auto finalMetadata = file.metadata();
    if (!finalMetadata) {
        return Result<std::string>::fail(finalMetadata.error());
    }
    if (finalMetadata.value().identity != metadata.value().identity ||
        finalMetadata.value().size != metadata.value().size || contents.size() != metadata.value().size) {
        return Result<std::string>::fail(
            {ErrorCode::SecurityPolicyViolation, description + " changed while being read"});
    }
    return Result<std::string>::ok(std::move(contents));
}

struct CivilDate {
    int year;
    unsigned int month;
    unsigned int day;
};

CivilDate civilFromDays(std::int64_t days) {
    days += 719468;
    const std::int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    const auto dayOfEra = static_cast<unsigned int>(days - era * 146097);
    const auto yearOfEra =
        static_cast<unsigned int>((dayOfEra - dayOfEra / 1460 + dayOfEra / 36524 - dayOfEra / 146096) / 365);
    int year = static_cast<int>(yearOfEra + era * 400);
    const auto dayOfYear = dayOfEra - (365 * yearOfEra + yearOfEra / 4 - yearOfEra / 100);
    const auto monthPrime = static_cast<unsigned int>((5 * dayOfYear + 2) / 153);
    const auto day = dayOfYear - (153 * monthPrime + 2) / 5 + 1;
    const auto month = static_cast<unsigned int>(static_cast<int>(monthPrime) + (monthPrime < 10 ? 3 : -9));
    year += month <= 2 ? 1 : 0;
    return {year, month, day};
}

} // namespace

Result<std::string> detail::formatApplyCompletionTimestamp(const util::UtcInstant& instant) noexcept {
    try {
        if (instant.unixSeconds < kMinimumRfc3339UnixSeconds || instant.unixSeconds > kMaximumRfc3339UnixSeconds ||
            instant.nanoseconds >= 1000000000U) {
            return Result<std::string>::fail(receiptError("Apply completion timestamp is outside RFC 3339 range"));
        }

        auto days = instant.unixSeconds / 86400;
        auto secondsOfDay = instant.unixSeconds % 86400;
        if (secondsOfDay < 0) {
            --days;
            secondsOfDay += 86400;
        }
        const auto date = civilFromDays(days);
        const auto hour = static_cast<unsigned int>(secondsOfDay / 3600);
        const auto minute = static_cast<unsigned int>((secondsOfDay % 3600) / 60);
        const auto second = static_cast<unsigned int>(secondsOfDay % 60);

        std::ostringstream stream;
        stream.imbue(std::locale::classic());
        stream << std::setfill('0') << std::setw(4) << date.year << '-' << std::setw(2) << date.month << '-'
               << std::setw(2) << date.day << 'T' << std::setw(2) << hour << ':' << std::setw(2) << minute << ':'
               << std::setw(2) << second;
        if (instant.nanoseconds != 0) {
            std::ostringstream fraction;
            fraction.imbue(std::locale::classic());
            fraction << std::setfill('0') << std::setw(9) << instant.nanoseconds;
            auto digits = fraction.str();
            while (digits.back() == '0') {
                digits.pop_back();
            }
            stream << '.' << digits;
        }
        stream << 'Z';
        return Result<std::string>::ok(stream.str());
    } catch (...) {
        return Result<std::string>::fail(receiptError("Failed to format apply completion timestamp"));
    }
}

Result<std::string> serializeApplyTransactionReceipt(const ApplyTransactionReceipt& receipt) noexcept {
    try {
        if (!util::isLowerHexSha256(receipt.transactionId) || !util::isLowerHexSha256(receipt.planDigest)) {
            return Result<std::string>::fail(receiptError("Invalid terminal apply transaction identity"));
        }
        util::Json::Object object;
        object.emplace("schemaVersion", receipt.completedAt ? kReceiptSchemaVersion : kLegacyReceiptSchemaVersion);
        object.emplace("transactionId", receipt.transactionId);
        object.emplace("planDigest", receipt.planDigest);
        if (receipt.completedAt) {
            auto timestamp = detail::formatApplyCompletionTimestamp(*receipt.completedAt);
            if (!timestamp) {
                return Result<std::string>::fail(timestamp.error());
            }
            object.emplace("completedAt", std::move(timestamp.value()));
        }
        return Result<std::string>::ok(util::Json(std::move(object)).stringify(2));
    } catch (...) {
        return Result<std::string>::fail(receiptError("Failed to serialize terminal apply transaction"));
    }
}

Result<ApplyTransactionReceipt> parseApplyTransactionReceipt(const std::string& text) noexcept {
    try {
        if (text.size() > kMaxReceiptBytes) {
            return Result<ApplyTransactionReceipt>::fail(
                {ErrorCode::ResourceLimitExceeded, "Terminal apply receipt exceeds its byte limit"});
        }
        auto parsed = util::Json::parse(text, receiptJsonLimits());
        if (!parsed) {
            return Result<ApplyTransactionReceipt>::fail(parsed.error());
        }
        if (!parsed.value().isObject()) {
            return Result<ApplyTransactionReceipt>::fail(receiptError("Terminal apply receipt must be an object"));
        }
        const auto* schema = parsed.value().get("schemaVersion");
        if (!schema || !schema->isUnsignedInteger() ||
            (schema->asUInt64() != kLegacyReceiptSchemaVersion && schema->asUInt64() != kReceiptSchemaVersion)) {
            return Result<ApplyTransactionReceipt>::fail(
                receiptError("Unsupported terminal apply receipt schema version"));
        }
        const auto schemaVersion = schema->asUInt64();
        auto keys = validateReceiptKeys(parsed.value().asObject(), schemaVersion);
        if (!keys) {
            return Result<ApplyTransactionReceipt>::fail(keys.error());
        }
        auto transactionId = requiredString(parsed.value(), "transactionId");
        auto planDigest = requiredString(parsed.value(), "planDigest");
        if (!transactionId) {
            return Result<ApplyTransactionReceipt>::fail(transactionId.error());
        }
        if (!planDigest) {
            return Result<ApplyTransactionReceipt>::fail(planDigest.error());
        }
        if (!util::isLowerHexSha256(transactionId.value()) || !util::isLowerHexSha256(planDigest.value())) {
            return Result<ApplyTransactionReceipt>::fail(receiptError("Invalid terminal apply transaction identity"));
        }
        std::optional<util::UtcInstant> completedAt;
        if (schemaVersion == kReceiptSchemaVersion) {
            auto timestamp = requiredString(parsed.value(), "completedAt");
            if (!timestamp) {
                return Result<ApplyTransactionReceipt>::fail(timestamp.error());
            }
            auto instant = util::parseRfc3339(timestamp.value());
            if (!instant || !detail::formatApplyCompletionTimestamp(instant.value())) {
                return Result<ApplyTransactionReceipt>::fail(
                    receiptError("Invalid terminal apply completion timestamp"));
            }
            completedAt = instant.value();
        }
        return Result<ApplyTransactionReceipt>::ok({transactionId.value(), planDigest.value(), std::move(completedAt)});
    } catch (...) {
        return Result<ApplyTransactionReceipt>::fail(receiptError("Failed to parse terminal apply receipt"));
    }
}

Result<std::optional<ApplyTransactionReceipt>>
loadTerminalApplyTransaction(IFileSystem& fileSystem, const std::filesystem::path& installDir) noexcept {
    try {
        auto root =
            fileSystem.openRoot(installDir, RootAccess::ReadOnly, false, RootedDirectoryCreationMode::InstalledContent);
        if (!root) {
            return Result<std::optional<ApplyTransactionReceipt>>::fail(root.error());
        }
        auto opened = root.value()->openRegularFile(kTerminalReceiptPath, RootedFileOpenMode::ReadOnly);
        if (!opened) {
            return Result<std::optional<ApplyTransactionReceipt>>::fail(opened.error());
        }
        if (!opened.value().exists()) {
            return Result<std::optional<ApplyTransactionReceipt>>::ok(std::nullopt);
        }
        auto contents = readBoundedFile(*opened.value().file, kMaxReceiptBytes, "Terminal apply receipt");
        auto closed = opened.value().file->close();
        if (!contents) {
            auto error = contents.error();
            if (!closed) {
                error.message += "; failed to close terminal apply receipt: " + closed.error().message;
            }
            return Result<std::optional<ApplyTransactionReceipt>>::fail(std::move(error));
        }
        if (!closed) {
            return Result<std::optional<ApplyTransactionReceipt>>::fail(closed.error());
        }
        auto parsed = parseApplyTransactionReceipt(contents.value());
        if (!parsed) {
            return Result<std::optional<ApplyTransactionReceipt>>::fail(parsed.error());
        }
        return Result<std::optional<ApplyTransactionReceipt>>::ok(std::move(parsed.value()));
    } catch (...) {
        return Result<std::optional<ApplyTransactionReceipt>>::fail(
            receiptError("Failed to read terminal apply receipt"));
    }
}

Result<ApplyPlan> detail::loadTerminalApplyPlan(IFileSystem& fileSystem, const std::filesystem::path& installDir,
                                                const ApplyTransactionReceipt& receipt,
                                                const ResourceLimits& limits) noexcept {
    try {
        if (!util::isLowerHexSha256(receipt.transactionId) || !util::isLowerHexSha256(receipt.planDigest)) {
            return Result<ApplyPlan>::fail(receiptError("Invalid terminal apply transaction identity"));
        }
        auto root =
            fileSystem.openRoot(installDir, RootAccess::ReadOnly, false, RootedDirectoryCreationMode::InstalledContent);
        if (!root) {
            return Result<ApplyPlan>::fail(root.error());
        }
        const auto snapshotPath = ".autoupdater/journal/" + receipt.transactionId + ".plan.json";
        auto opened = root.value()->openRegularFile(snapshotPath, RootedFileOpenMode::ReadOnly);
        if (!opened) {
            return Result<ApplyPlan>::fail(opened.error());
        }
        if (!opened.value().exists()) {
            return Result<ApplyPlan>::fail(receiptError("Terminal apply plan snapshot is missing"));
        }
        auto contents = readBoundedFile(*opened.value().file, limits.maxApplyPlanBytes, "Terminal apply plan snapshot");
        auto closed = opened.value().file->close();
        if (!contents) {
            auto error = contents.error();
            if (!closed) {
                error.message += "; failed to close terminal apply plan snapshot: " + closed.error().message;
            }
            return Result<ApplyPlan>::fail(std::move(error));
        }
        if (!closed) {
            return Result<ApplyPlan>::fail(closed.error());
        }
        if (util::sha256Bytes(contents.value()) != receipt.planDigest) {
            return Result<ApplyPlan>::fail(
                receiptError("Terminal apply plan snapshot digest does not match its receipt"));
        }
        return ApplyPlan::parse(contents.value(), limits);
    } catch (...) {
        return Result<ApplyPlan>::fail(receiptError("Failed to read terminal apply plan snapshot"));
    }
}

} // namespace autoupdater
