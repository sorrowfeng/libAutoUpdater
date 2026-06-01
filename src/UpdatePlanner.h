#pragma once

#include "UpdateTypes.h"

namespace autoupdater {

Result<UpdateDecision> planUpdate(const Config& config,
                                  const ManifestEnvelope& envelope,
                                  const LocalSnapshot& snapshot,
                                  const std::optional<Version>& lastAcceptedVersion);

} // namespace autoupdater

