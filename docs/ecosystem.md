# Ecosystem Packaging

本项目优先支持 CMake package。其他包管理生态提供初始模板，发布到对应官方 registry 前仍需要维护者按各生态规则提交审核。

## vcpkg

仓库根目录的 `vcpkg.json` 用于 manifest mode。

示例 port 位于 `packaging/vcpkg/`，可用于私有 registry 或后续提交到 vcpkg 官方 ports。该目录当前是模板，使用前请先阅读 `packaging/vcpkg/README.md` 并替换 release hash。

## Conan

`conanfile.py` 提供基础 Conan 2 recipe，适合内部验证或私有 remote。

## Homebrew

`packaging/homebrew/libautoupdater.rb` 是 formula 模板。正式提交到 tap 前需要更新 release URL 和 SHA-256。

## FetchContent

```cmake
include(FetchContent)
FetchContent_Declare(
    libAutoUpdater
    GIT_REPOSITORY https://github.com/sorrowfeng/libAutoUpdater.git
    GIT_TAG v0.1.2)
FetchContent_MakeAvailable(libAutoUpdater)

target_link_libraries(MyApp PRIVATE libAutoUpdater::libAutoUpdater)
```
