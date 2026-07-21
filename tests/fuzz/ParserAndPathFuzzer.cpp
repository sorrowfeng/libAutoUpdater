#include "libAutoUpdater/ApplyPlan.h"
#include "libAutoUpdater/Manifest.h"
#include "libAutoUpdater/ResourceLimits.h"
#include "libAutoUpdater/Version.h"
#include "util/PathUtil.h"
#include "util/UrlUtil.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::string input(reinterpret_cast<const char*>(data), size);

    autoupdater::ResourceLimits limits;
    limits.maxIndexBytes = 1024 * 1024;
    limits.maxManifestBytes = 1024 * 1024;
    limits.maxApplyPlanBytes = 1024 * 1024;
    limits.json.maxStringBytes = 256 * 1024;
    limits.json.maxNodes = 20000;
    limits.json.maxContainerEntries = 5000;

    (void)autoupdater::Version::parse(input);
    (void)autoupdater::Manifest::parse(input, limits);
    (void)autoupdater::IndexManifest::parse(input, limits);
    (void)autoupdater::ApplyPlan::parse(input, limits);

    const std::filesystem::path root = "fuzz-install-root";
    (void)autoupdater::util::validateManagedPath(input);
    (void)autoupdater::util::validateManagedTargetPath(input);
    (void)autoupdater::util::safeJoin(root, input);
    (void)autoupdater::util::parseAbsoluteUrl(input);

    const auto separator = input.find('\0');
    if (separator != std::string::npos) {
        const std::string_view base(input.data(), separator);
        const std::string_view reference(input.data() + separator + 1, input.size() - separator - 1);
        (void)autoupdater::util::resolveUrlReference(base, reference);
    }

    return 0;
}
