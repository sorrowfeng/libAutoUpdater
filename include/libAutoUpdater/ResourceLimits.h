#pragma once

#include <cstddef>
#include <cstdint>

namespace autoupdater {

/// Hard budgets used while parsing untrusted JSON documents.
struct JsonResourceLimits {
    static constexpr std::size_t absoluteMaxDepth = 256;
    static constexpr std::size_t absoluteMaxNodes = 1000000;
    static constexpr std::size_t absoluteMaxStringBytes = 16 * 1024 * 1024;
    static constexpr std::size_t absoluteMaxNumberBytes = 1024;
    static constexpr std::size_t absoluteMaxContainerEntries = 100000;

    std::size_t maxDepth = 64;
    std::size_t maxNodes = 100000;
    std::size_t maxStringBytes = 1024 * 1024;
    std::size_t maxNumberBytes = 128;
    std::size_t maxContainerEntries = 10000;
};

/// Hard byte and parser budgets for update metadata and artifacts.
///
/// Updater configurations may lower these defaults. The built-in external
/// updater uses the same defaults as safety ceilings so the producer and
/// consumer of apply-plan.json enforce a consistent contract.
struct ResourceLimits {
    std::uint64_t maxIndexBytes = 1024 * 1024;
    std::uint64_t maxManifestBytes = 4 * 1024 * 1024;
    std::uint64_t maxSignatureBytes = 64 * 1024;
    std::uint64_t maxArtifactBytes = UINT64_C(8) * 1024 * 1024 * 1024;
    std::uint64_t maxTotalArtifactBytes = UINT64_C(32) * 1024 * 1024 * 1024;
    std::uint64_t maxApplyPlanBytes = 8 * 1024 * 1024;
    std::uint64_t maxStateBytes = 8 * 1024 * 1024;
    JsonResourceLimits json;
};

} // namespace autoupdater
