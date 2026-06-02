# libAutoUpdater

[![CI](https://github.com/sorrowfeng/libAutoUpdater/actions/workflows/ci.yml/badge.svg)](https://github.com/sorrowfeng/libAutoUpdater/actions/workflows/ci.yml)
[![CodeQL](https://github.com/sorrowfeng/libAutoUpdater/actions/workflows/codeql.yml/badge.svg)](https://github.com/sorrowfeng/libAutoUpdater/actions/workflows/codeql.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)

[English](README.md) | 中文

`libAutoUpdater` 是一个面向生产环境的 C++17 桌面应用在线更新库，支持 Windows、macOS 和 Linux。

它的核心思路是“静态文件更新服务”：你只需要把 manifest 和更新文件发布到 HTTP/HTTPS 服务器、对象存储、CDN，甚至 GitHub Raw，不需要自建后端服务。

## 项目价值

- 支持跨平台桌面应用自更新。
- 核心库不绑定 GUI，可接入 Qt、命令行、原生 UI 或自定义 shell。
- 基于 SHA-256 做文件级增量更新。
- 使用外部 updater 进程安全替换运行中的应用文件。
- 支持更新失败后的回滚。
- 可选 manifest 数字签名校验。
- 网络层抽象，支持 libcurl、WinHTTP、CFNetwork 和自定义适配器。
- 网络、哈希、文件系统、签名、进程、事件分发和状态存储都可注入，便于测试。

## 工作流程

```text
Application
  -> libAutoUpdater 检查 manifest
  -> 只下载变化文件到 staging 目录
  -> 每个下载文件都做 SHA-256 校验
  -> 写入 apply-plan.json
  -> autoupdater_apply 等待主程序退出
  -> 备份、替换、校验，失败则回滚
  -> 重启应用
```

主程序不会直接覆盖正在运行的自身文件。所有替换动作都交给 `autoupdater_apply` 完成。

## 快速开始

```sh
cmake --preset dev
cmake --build --preset dev --parallel
ctest --preset dev
```

用 CLI 示例检查一个本地或远程 manifest：

```sh
build/examples/cli/Debug/libAutoUpdater_cli.exe \
  --manifest file:///C:/path/to/release/manifest.json \
  --version 1.3.0 \
  --install C:/path/to/install \
  --updater build/updater/Debug/autoupdater_apply.exe
```

Linux 和 macOS 下，如果没有使用多配置生成器，可执行文件通常直接位于 `build/examples/cli/` 和 `build/updater/` 下。

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

## 构建选项

常用 CMake 选项：

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

## 依赖模型

公共 API 不暴露 libcurl、OpenSSL、Qt 或 JSON 库类型。

网络后端选择：

- 本地路径和 `file://` URL 始终可用。
- CMake 找到 `CURL::libcurl` 时使用 libcurl 处理 HTTP/HTTPS。
- Windows 可在不使用 libcurl 的情况下使用 WinHTTP。
- macOS 可在不使用 libcurl 的情况下使用 CFNetwork/CoreFoundation。
- Qt 用户可以注入 `QNetworkAccessManager` 适配器。

最终应用负责打包运行时依赖。如果动态链接 libcurl 或 OpenSSL，需要随应用分发匹配的运行时库和许可证说明。

## CMake 集成

安装后使用：

```cmake
find_package(libAutoUpdater CONFIG REQUIRED)
target_link_libraries(MyApp PRIVATE libAutoUpdater::libAutoUpdater)
```

或直接嵌入源码：

```cmake
add_subdirectory(external/libAutoUpdater)
target_link_libraries(MyApp PRIVATE libAutoUpdater::libAutoUpdater)
```

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

详情见 [docs/content-addressed-storage.md](docs/content-addressed-storage.md)。

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

## 真实更新演示

仓库的 `examples/update-server` 目录包含一个真实的静态更新 feed。GitHub 会通过 `raw.githubusercontent.com` 提供这些文件，因此仓库本身就可以作为测试更新服务器。

```sh
python examples/github_update_demo.py
```

这个脚本会创建一个本地 `1.0.0` 安装目录，从 GitHub Raw 下载 `2.0.0` manifest，通过 `autoupdater_apply` 应用更新，并打印更新前后的文件树。

CI 也会在 Ubuntu/libcurl、Windows/WinHTTP 和 macOS/CFNetwork 上运行这条真实更新流程。

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

Release workflow 会发布 Windows、macOS、Linux 三个平台的安装树 ZIP、SHA-256 文件、SPDX SBOM 文件，并从 `CHANGELOG.md` 提取 release notes。

## 安全

- 生产环境保持 TLS 校验开启。
- 面向公开更新渠道时，建议强制 manifest 签名。
- 更新私钥不要放在静态文件服务器上。
- 公开渠道建议配置 `allowedBaseUrls`。
- manifest 不要包含绝对路径或 `..`。
- package manager 管理的安装目录更适合交给系统包管理器更新。

更多信息见 [SECURITY.md](SECURITY.md) 和 [docs/security-model.md](docs/security-model.md)。

## 社区

- [贡献指南](CONTRIBUTING.md)
- [安全策略](SECURITY.md)
- [行为准则](CODE_OF_CONDUCT.md)
- [更新日志](CHANGELOG.md)

## License

本项目使用 MIT 协议。详见 [LICENSE](LICENSE)。
