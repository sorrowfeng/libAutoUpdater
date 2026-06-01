#pragma once

#include <string>

namespace autoupdater {

enum class ErrorCode {
    None = 0,
    InvalidConfig,
    NetworkError,
    ManifestDownloadFailed,
    UnsupportedManifestSchema,
    ManifestParseFailed,
    ManifestSignatureInvalid,
    VersionParseFailed,
    ReinstallRequired,
    FileSystemError,
    PathTraversalRejected,
    HashMismatch,
    DownloadFailed,
    Cancelled,
    ApplyLaunchFailed,
    ApplyFailed,
    StateStoreError,
    SecurityPolicyViolation,
    UnsupportedInstallLayout,
    InternalError
};

struct Error {
    ErrorCode code = ErrorCode::None;
    std::string message;

    bool ok() const noexcept { return code == ErrorCode::None; }
};

const char* toString(ErrorCode code) noexcept;

} // namespace autoupdater

