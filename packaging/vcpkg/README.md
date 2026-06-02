# vcpkg Port Template

This directory is a starter template for a private vcpkg registry or a future upstream vcpkg port.

Before using it as a real port:

- Replace `SHA512 0` in `portfile.cmake` with the SHA-512 digest of the release tarball.
- Keep `vcpkg.json` in sync with the project version.
- Review optional dependencies and expose them as vcpkg features if you need curl, OpenSSL, or Qt to be selectable.

The repository root `vcpkg.json` is the manifest-mode file for consumers who build directly from this source tree.
