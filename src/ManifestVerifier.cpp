#include "ManifestFetcher.h"

namespace autoupdater {

// Manifest verification is intentionally part of fetchAndVerifyManifest for now:
// detached signature verification must run on the original manifest bytes before
// JSON parsing. This file remains as the extension point documented by the
// architecture plan.

} // namespace autoupdater

