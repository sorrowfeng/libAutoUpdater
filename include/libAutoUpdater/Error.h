#pragma once

#include <string>

namespace autoupdater {

/// Stable error categories returned through Result<T> and callbacks.
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
    InternalError,
    ResourceLimitExceeded
};

/// Operational phase in which an error was observed. This is orthogonal to
/// ErrorCode so callers can distinguish failures that share the same code.
enum class ErrorPhase {
    General = 0,
    Apply,
    Rollback,
    Recovery,
    StatePersistence,
    Restart,
};

/// Error object containing a stable code, human-readable detail, and phase.
/// message may contain implementation-provided context and is not safe for
/// unattended logs. Use formatDiagnostic() for non-sensitive log output.
struct Error {
    ErrorCode code = ErrorCode::None;
    std::string message;
    ErrorPhase phase = ErrorPhase::General;

    bool ok() const noexcept {
        return code == ErrorCode::None;
    }
};

const char* toString(ErrorCode code) noexcept;
const char* toString(ErrorPhase phase) noexcept;

/// Formats stable structured fields without including the arbitrary message.
std::string formatDiagnostic(const Error& error);

} // namespace autoupdater
