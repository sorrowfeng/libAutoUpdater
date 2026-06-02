# Ecosystem Packaging

The primary supported consumption model is the CMake package. Other package-manager files are starter templates and still require maintainer review before submission to their official registries.

## vcpkg

The repository root `vcpkg.json` is for manifest mode.

The example port in `packaging/vcpkg/` can be used for a private registry or as a starting point for a future upstream vcpkg port. It is currently a template; read `packaging/vcpkg/README.md` and replace the release hash before using it.

## Conan

`conanfile.py` provides a basic Conan 2 recipe for internal validation or private remotes.

## Homebrew

`packaging/homebrew/libautoupdater.rb` is a formula template. Update the release URL and SHA-256 before submitting it to a tap.

## FetchContent

```cmake
include(FetchContent)
FetchContent_Declare(
    libAutoUpdater
    GIT_REPOSITORY https://github.com/sorrowfeng/libAutoUpdater.git
    GIT_TAG v0.1.3)
FetchContent_MakeAvailable(libAutoUpdater)

target_link_libraries(MyApp PRIVATE libAutoUpdater::libAutoUpdater)
```
