#pragma once

#include "libAutoUpdater/Config.h"
#include "util/UrlUtil.h"

#include <string_view>
#include <vector>

namespace autoupdater {

/// URL authorization policy rooted exclusively in the trusted configured
/// manifest URL. Parsed manifest data and redirect targets can never create a
/// new trust root.
class UrlPolicy {
  public:
    static Result<UrlPolicy> fromConfig(const Config& config) noexcept;

    const util::ParsedUrl& initialUrl() const noexcept;
    Result<util::ParsedUrl> authorize(std::string_view url) const noexcept;
    Result<void> authorize(const util::ParsedUrl& url) const noexcept;
    Result<void> authorizeTransition(const util::ParsedUrl& from, const util::ParsedUrl& to) const noexcept;
    Result<util::ParsedUrl> resolveAndAuthorize(const util::ParsedUrl& base, std::string_view reference) const noexcept;

  private:
    enum class Mode { Network, LocalFile };

    bool matchesNetworkScope(const util::ParsedUrl& url) const noexcept;
    bool matchesLocalScope(const util::ParsedUrl& url) const noexcept;

    Mode mode_ = Mode::Network;
    util::ParsedUrl initial_;
    util::ParsedUrl localRoot_;
    std::vector<util::ParsedUrl> networkScopes_;
};

} // namespace autoupdater
