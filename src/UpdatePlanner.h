#pragma once

#include "UpdateTypes.h"
#include "util/Rfc3339.h"

namespace autoupdater {

Result<UpdateDecision> planUpdate(const Config& config, const ManifestEnvelope& envelope, const LocalSnapshot& snapshot,
                                  const std::optional<Version>& lastAcceptedVersion,
                                  const util::UtcInstant& currentTime);

} // namespace autoupdater
