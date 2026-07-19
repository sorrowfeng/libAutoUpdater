#include "ApplyTransactionReceipt.h"

#include "util/Json.h"
#include "util/Sha256.h"

#include <array>
#include <cstdint>
#include <limits>

namespace autoupdater {

namespace {

constexpr std::uint64_t kJournalSchemaVersion = 2;
constexpr std::uint64_t kMaxReceiptBytes = 256 * 1024;
constexpr std::size_t kReadChunkBytes = 64 * 1024;
constexpr const char* kTerminalReceiptPath = ".autoupdater/journal/terminal.json";

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

Result<void> validateReceiptKeys(const util::Json::Object& object) {
    if (object.size() != 3 || object.find("schemaVersion") == object.end() ||
        object.find("transactionId") == object.end() || object.find("planDigest") == object.end()) {
        return Result<void>::fail(receiptError("Terminal apply receipt contains unknown or missing fields"));
    }
    return Result<void>::ok();
}

Result<std::string> readReceipt(IRootedFile& file) {
    auto metadata = file.metadata();
    if (!metadata) {
        return Result<std::string>::fail(metadata.error());
    }
    if (metadata.value().size > kMaxReceiptBytes || metadata.value().size > std::numeric_limits<std::size_t>::max()) {
        return Result<std::string>::fail(
            {ErrorCode::ResourceLimitExceeded, "Terminal apply receipt exceeds its byte limit"});
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
        if (contents.size() > kMaxReceiptBytes || read.value() > kMaxReceiptBytes - contents.size()) {
            return Result<std::string>::fail(
                {ErrorCode::ResourceLimitExceeded, "Terminal apply receipt grew beyond its byte limit"});
        }
        contents.append(buffer.data(), read.value());
    }
    return Result<std::string>::ok(std::move(contents));
}

} // namespace

Result<std::string> serializeApplyTransactionReceipt(const ApplyTransactionReceipt& receipt) noexcept {
    try {
        if (!util::isLowerHexSha256(receipt.transactionId) || !util::isLowerHexSha256(receipt.planDigest)) {
            return Result<std::string>::fail(receiptError("Invalid terminal apply transaction identity"));
        }
        util::Json::Object object;
        object.emplace("schemaVersion", kJournalSchemaVersion);
        object.emplace("transactionId", receipt.transactionId);
        object.emplace("planDigest", receipt.planDigest);
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
        if (!schema || !schema->isUnsignedInteger() || schema->asUInt64() != kJournalSchemaVersion) {
            return Result<ApplyTransactionReceipt>::fail(
                receiptError("Unsupported terminal apply receipt schema version"));
        }
        auto keys = validateReceiptKeys(parsed.value().asObject());
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
        return Result<ApplyTransactionReceipt>::ok({transactionId.value(), planDigest.value()});
    } catch (...) {
        return Result<ApplyTransactionReceipt>::fail(receiptError("Failed to parse terminal apply receipt"));
    }
}

Result<std::optional<ApplyTransactionReceipt>>
loadTerminalApplyTransaction(IFileSystem& fileSystem, const std::filesystem::path& installDir) noexcept {
    try {
        auto root = fileSystem.openRoot(installDir, RootAccess::ReadOnly, false,
                                        RootedDirectoryCreationMode::InstalledContent);
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
        auto contents = readReceipt(*opened.value().file);
        if (!contents) {
            return Result<std::optional<ApplyTransactionReceipt>>::fail(contents.error());
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

} // namespace autoupdater
