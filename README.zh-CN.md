# libAutoUpdater

[![CI](https://github.com/sorrowfeng/libAutoUpdater/actions/workflows/ci.yml/badge.svg)](https://github.com/sorrowfeng/libAutoUpdater/actions/workflows/ci.yml)
[![CodeQL](https://github.com/sorrowfeng/libAutoUpdater/actions/workflows/codeql.yml/badge.svg)](https://github.com/sorrowfeng/libAutoUpdater/actions/workflows/codeql.yml)
[![Release](https://img.shields.io/github/v/release/sorrowfeng/libAutoUpdater?include_prereleases)](https://github.com/sorrowfeng/libAutoUpdater/releases)
[![C++17](https://img.shields.io/badge/C%2B%2B-17-blue)](CMakeLists.txt)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)

[English](README.md) | 中文

`libAutoUpdater` 是一个面向 Windows、macOS 和 Linux 桌面应用的 C++17 在线更新库。它读取静态 manifest，只下载变化文件，校验每个文件，通过外部 updater 进程完成安全替换，并在 apply 失败时支持回滚。

它不需要自建更新后端。manifest 和更新文件可以发布到任意 HTTP/HTTPS 静态服务器、对象存储、CDN，甚至 GitHub Raw。

## 亮点

- 静态文件发布更新，不需要运行后端服务。
- 基于 SHA-256 的文件级增量更新。
- 通过 `autoupdater_apply` 安全替换运行中的应用文件。
- 替换或校验失败后可回滚。
- 可选 detached manifest 签名。
- 支持防降级、防重放和可信 base URL 限制。
- Windows 和 macOS 有原生 HTTPS 后端，也可使用 libcurl。
- 核心库不依赖 GUI，提供 CLI 和 Qt 示例。
- 网络、哈希、文件系统、签名、进程、事件分发和状态存储都可注入，便于测试。

## 项目状态

`libAutoUpdater` 当前处于 `0.x` 阶段。核心架构、updater 子程序、manifest 工具、示例、CI 和真实更新流程测试已经具备，但公共 API 和 manifest schema 在 `1.0.0` 前仍可能随 minor 版本调整。

| 模块 | 状态 |
| --- | --- |
| 检查 / 计划 / 下载 / apply 核心流程 | 已实现 |
| 外部 updater 进程 | 已实现 |
| 回滚和更新锁 | 已实现 |
| Manifest 签名 | 可选，基于 OpenSSL |
| Windows/macOS 原生 HTTPS | WinHTTP / CFNetwork |
| Linux HTTPS | libcurl |
| Qt 集成 | 可选示例适配器 |
| 二进制 patch | 未实现 |

## 工作流程

![libAutoUpdater architecture](docs/assets/libautoupdater-architecture.png)

主程序不会直接覆盖正在运行的自身文件。所有替换动作都交给 `autoupdater_apply` 完成。

## 快速开始

```sh
cmake --preset dev
cmake --build --preset dev --parallel
ctest --preset dev
```

运行真实静态更新 demo：

```sh
python examples/github_update_demo.py
```

这个 demo 会创建一个本地 `1.0.0` 安装目录，通过 GitHub Raw 从本仓库获取 `2.0.0` manifest，使用 `autoupdater_apply` 应用更新，并打印更新前后的文件树。

## 最小接入示例

```cpp
#include <libAutoUpdater/Updater.h>

autoupdater::Config config;
config.appId = "com.example.myapp";
config.manifestUrl = "https://example.com/updates/releases/1.4.0/windows-x64/manifest.json";
config.currentVersion = autoupdater::Version::parse("1.3.0").value();
config.installDir = "C:/Program Files/MyApp";
config.updaterExecutable = "C:/Program Files/MyApp/autoupdater_apply.exe";
config.restartCommand = {"C:/Program Files/MyApp/MyApp.exe"};

autoupdater::Updater updater(config);
updater.setCallbacks(callbacks);
updater.checkAndDownloadAsync();
```

当更新准备好后，由你的 UI 或命令流程调用 `applyAndRestartAsync()`。新版本成功启动后调用：

```cpp
updater.markCurrentVersionHealthy();
```

## 安装和集成

安装后使用：

```cmake
find_package(libAutoUpdater CONFIG REQUIRED)
target_link_libraries(MyApp PRIVATE libAutoUpdater::libAutoUpdater)
```

作为子目录引入：

```cmake
add_subdirectory(external/libAutoUpdater)
target_link_libraries(MyApp PRIVATE libAutoUpdater::libAutoUpdater)
```

使用 FetchContent：

```cmake
include(FetchContent)
FetchContent_Declare(
    libAutoUpdater
    GIT_REPOSITORY https://github.com/sorrowfeng/libAutoUpdater.git
    GIT_TAG v0.1.3)
FetchContent_MakeAvailable(libAutoUpdater)

target_link_libraries(MyApp PRIVATE libAutoUpdater::libAutoUpdater)
```

包管理生态状态：

| 生态 | 状态 |
| --- | --- |
| CMake package | 正式支持 |
| vcpkg manifest mode | 支持当前源码树 |
| vcpkg port | 模板位于 `packaging/vcpkg/` |
| Conan 2 | 初始 recipe |
| Homebrew | formula 模板 |

详情见 [docs/ecosystem.md](docs/ecosystem.md)。

## 支持平台和后端

| 平台 | 编译器覆盖 | HTTPS 后端 | 说明 |
| --- | --- | --- | --- |
| Windows | MSVC | WinHTTP, libcurl | WinHTTP 可避免分发 libcurl |
| macOS | AppleClang | CFNetwork, libcurl | CFNetwork 使用系统 framework |
| Linux | GCC, Clang | libcurl | package-manager-owned 安装通常应交给包管理器 |

签名校验是可选能力。默认 verifier 在可用时使用 OpenSSL，应用也可以注入自己的 `ISignatureVerifier`。

## CMake 选项

常用选项：

```text
LIBAUTOUPDATER_BUILD_UPDATER=ON
LIBAUTOUPDATER_BUILD_EXAMPLES=ON
LIBAUTOUPDATER_BUILD_TESTS=ON
LIBAUTOUPDATER_WITH_CURL=ON
LIBAUTOUPDATER_REQUIRE_CURL=OFF
LIBAUTOUPDATER_WITH_WINHTTP=ON
LIBAUTOUPDATER_WITH_CFNETWORK=ON
LIBAUTOUPDATER_WITH_OPENSSL=ON
LIBAUTOUPDATER_WITH_QT=OFF
LIBAUTOUPDATER_ENABLE_WARNINGS=ON
LIBAUTOUPDATER_WARNINGS_AS_ERRORS=OFF
LIBAUTOUPDATER_ENABLE_SANITIZERS=OFF
LIBAUTOUPDATER_ENABLE_COVERAGE=OFF
```

常用 preset：

- `dev`：开发构建，包含示例、测试、updater，并自动探测可选依赖。
- `no-optional-deps`：验证核心库在没有可选 HTTP/加密后端时也能构建。
- `windows-winhttp`：Windows 下使用系统 WinHTTP 提供 HTTPS 支持。
- `macos-cfnetwork`：macOS 下使用 CFNetwork/CoreFoundation 提供 HTTPS 支持。
- `vcpkg-debug`：使用 vcpkg manifest mode 提供 libcurl 和 OpenSSL。
- `release`：本地发布构建。

## 生成更新 feed

从发布目录生成 manifest：

```sh
python tools/make_manifest.py dist/MyApp \
  --app-id com.example.myapp \
  --platform windows \
  --arch x64 \
  --version 1.4.0 \
  --release-date 2026-06-01T10:00:00Z \
  --base-url https://example.com/updates/releases/1.4.0/windows-x64/
```

如果项目较大，可以用内容寻址存储避免多个版本重复上传相同文件：

```sh
python tools/make_manifest.py dist/MyApp \
  --content-addressed \
  --object-root publish/updates/objects/sha256 \
  --object-prefix objects/sha256 \
  --app-id com.example.myapp \
  --platform windows \
  --arch x64 \
  --version 1.4.0 \
  --release-date 2026-06-01T10:00:00Z \
  --base-url https://example.com/updates/ \
  --output publish/updates/releases/1.4.0/windows-x64/manifest.json
```

详情见 [docs/server-layout.md](docs/server-layout.md) 和 [docs/content-addressed-storage.md](docs/content-addressed-storage.md)。

## Manifest 示例

```json
{
  "schemaVersion": 1,
  "appId": "com.example.myapp",
  "channel": "stable",
  "platform": "windows",
  "arch": "x64",
  "version": "1.4.0",
  "releaseId": "1.4.0+20260601.1",
  "releaseDate": "2026-06-01T10:00:00Z",
  "publishedAt": "2026-06-01T10:00:00Z",
  "expiresAt": "2026-07-01T00:00:00Z",
  "minVersion": "1.2.0",
  "minClientVersion": "1.0.0",
  "mandatory": false,
  "allowDowngrade": false,
  "notes": "Fix startup crash and improve sync performance.",
  "baseUrl": "https://example.com/updates/releases/1.4.0/windows-x64/",
  "files": [
    {
      "path": "bin/MyApp.exe",
      "sha256": "9b3f...",
      "size": 18432000
    }
  ],
  "remove": [
    "plugins/old_plugin.dll"
  ]
}
```

## 一分钟 demo 效果

GitHub-hosted demo 会把本地安装目录从 `1.0.0` 更新到 `2.0.0`，本仓库本身就是 HTTPS 更新服务器：

```text
Before update
  bin/demo_app.txt          version=1.0.0
  config/settings.json      unchanged
  legacy/remove-me.txt      present

After update
  bin/demo_app.txt          version=2.0.0
  config/settings.json      unchanged
  resources/feature.txt     added
  legacy/remove-me.txt      removed
```

CI 也会在 Ubuntu/libcurl、Windows/WinHTTP 和 macOS/CFNetwork 上运行这条真实流程。

## 安全能力概览

| 能力 | 默认 / 支持情况 | 生产建议 |
| --- | --- | --- |
| TLS 校验 | 默认开启 | 保持开启 |
| 文件 SHA-256 | 每个受管文件必需 | 必需 |
| Manifest 签名 | 可选 | 公开渠道强制开启 |
| 可信 base URL | 可选 | 公开渠道建议开启 |
| 外部 apply 进程 | 必需 | 始终使用 |
| 回滚 | 已实现 | 健康确认前保留备份 |
| 路径穿越保护 | 强制校验 | 不要绕过 manifest 校验 |
| package-manager-owned 安装 | 按布局策略拒绝 | 使用系统包管理器 |

更多信息见 [SECURITY.md](SECURITY.md) 和 [docs/security-model.md](docs/security-model.md)。

## FAQ

### 一定需要 libcurl 吗？

不需要。本地路径和 `file://` URL 始终可用。Windows 可使用 WinHTTP，macOS 可使用 CFNetwork，Linux 通常使用 libcurl 处理 HTTP/HTTPS。

### 一定需要 Qt 吗？

不需要。核心库没有 GUI 依赖。Qt 示例只是展示如何把回调投递回 Qt UI 线程，以及如何使用 `QNetworkAccessManager`。

### 不同版本的文件数量和类型可以不一样吗？

可以。每个 manifest 描述目标版本的完整受管文件集合。客户端只下载缺失或 hash 不一致的文件，`remove[]` 用于删除目标版本不再需要的文件。

### 如何避免服务器上重复存很多相同文件？

使用内容寻址存储。服务端按 SHA-256 存 object，每个 release manifest 用服务器 `path` 映射到安装目录 `localPath`。

### 应用运行中能更新自己吗？

库可以在应用运行时检查、下载并准备更新。真正的文件替换会在主程序退出后，由 `autoupdater_apply` 完成。

### Linux 发行版包或 Homebrew 应用应该自更新吗？

通常不建议。如果安装目录由包管理器管理，应交给对应包管理器更新。

## 文档

- [Getting started](docs/getting-started.md)
- [Integration guide](docs/integration.md)
- [API overview](docs/api.md)
- [Architecture plan](docs/architecture-plan.md)
- [Server layout](docs/server-layout.md)
- [Content-addressed storage](docs/content-addressed-storage.md)
- [Security model](docs/security-model.md)
- [Release process](docs/release-process.md)
- [Troubleshooting](docs/troubleshooting.md)
- [Quality gates](docs/quality.md)
- [Ecosystem packaging](docs/ecosystem.md)

## 项目结构

```text
include/libAutoUpdater/       公共 C++ 头文件
src/                          核心实现和默认适配器
updater/                      外部 apply 可执行程序
examples/cli/                 CLI 示例
examples/qt/                  可选 Qt 示例
tests/                        单元测试和流程测试
tools/                        发布和打包工具
docs/                         设计与集成文档
```

## CI/CD

GitHub Actions 覆盖 source hygiene、clang-format、clang-tidy、GCC/Clang/AppleClang/MSVC 构建、无可选依赖构建、ASan/UBSan、coverage、安装树打包、`find_package` 消费者验证、CodeQL，以及真实 GitHub 更新演示。

Release workflow 会发布 Windows、macOS、Linux 三个平台的安装树 ZIP、SPDX SBOM 文件，并从 `CHANGELOG.md` 提取 release notes。

## 社区

- [贡献指南](CONTRIBUTING.md)
- [安全策略](SECURITY.md)
- [行为准则](CODE_OF_CONDUCT.md)
- [更新日志](CHANGELOG.md)

## License

本项目使用 MIT 协议。详见 [LICENSE](LICENSE)。
