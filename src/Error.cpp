#include "libAutoUpdater/Error.h"

namespace autoupdater {

const char* toString(ErrorCode code) noexcept {
    switch (code) {
    case ErrorCode::None:
        return "None";
    case ErrorCode::InvalidConfig:
        return "InvalidConfig";
    case ErrorCode::NetworkError:
        return "NetworkError";
    case ErrorCode::ManifestDownloadFailed:
        return "ManifestDownloadFailed";
    case ErrorCode::UnsupportedManifestSchema:
        return "UnsupportedManifestSchema";
    case ErrorCode::ManifestParseFailed:
        return "ManifestParseFailed";
    case ErrorCode::ManifestSignatureInvalid:
        return "ManifestSignatureInvalid";
    case ErrorCode::VersionParseFailed:
        return "VersionParseFailed";
    case ErrorCode::ReinstallRequired:
        return "ReinstallRequired";
    case ErrorCode::FileSystemError:
        return "FileSystemError";
    case ErrorCode::PathTraversalRejected:
        return "PathTraversalRejected";
    case ErrorCode::HashMismatch:
        return "HashMismatch";
    case ErrorCode::DownloadFailed:
        return "DownloadFailed";
    case ErrorCode::Cancelled:
        return "Cancelled";
    case ErrorCode::ApplyLaunchFailed:
        return "ApplyLaunchFailed";
    case ErrorCode::ApplyFailed:
        return "ApplyFailed";
    case ErrorCode::StateStoreError:
        return "StateStoreError";
    case ErrorCode::SecurityPolicyViolation:
        return "SecurityPolicyViolation";
    case ErrorCode::UnsupportedInstallLayout:
        return "UnsupportedInstallLayout";
    case ErrorCode::InternalError:
        return "InternalError";
    }
    return "Unknown";
}

} // namespace autoupdater
