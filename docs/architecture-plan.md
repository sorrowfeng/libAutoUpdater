# libAutoUpdater 架构与实施计划

本文档描述 `libAutoUpdater` 的目标架构、核心模块、关键设计取舍、实施阶段与验收标准。

目标是实现一个通用的、生产可用的 C++17 桌面应用在线更新库，支持 Windows、macOS、Linux，后端使用静态文件与 HTTP/HTTPS 服务器，不强依赖自建服务端。

## 1. 设计目标

`libAutoUpdater` 面向桌面应用自更新场景，核心目标如下：

- 跨平台：支持 Windows、macOS、Linux。
- 低耦合：库本身不绑定任何 GUI 框架。
- 可替换：网络、哈希、文件系统、签名、进程启动、事件投递均通过接口抽象。
- 可测试：核心决策逻辑尽量纯函数化，便于 mock 与单元测试。
- 可恢复：更新失败时可回滚；新版本启动失败时可恢复上一版本。
- 可审计：manifest、apply plan、事务日志均为清晰的结构化文件。
- 后端简单：支持纯静态文件服务器发布更新。
- 安全优先：支持 HTTPS 校验、SHA-256 校验、manifest 签名、防降级、防重放。

非目标：

- 第一阶段不实现二进制 patch。
- 第一阶段不内置服务端。
- 第一阶段不直接绑定 Qt、MFC、wxWidgets、Electron 等 GUI 框架。
- 对系统包管理器安装的软件，不默认绕过包管理器进行自更新。

## 2. 总体架构

整体采用四层结构：

```text
Application
  |
  v
Updater Facade
  |
  v
Update Orchestrator
  |
  +-- ManifestFetcher
  +-- ManifestVerifier
  +-- LocalSnapshotBuilder
  +-- UpdatePlanner
  +-- DownloadExecutor
  +-- ApplyPlanWriter
  +-- ApplyLauncher
  |
  v
Infrastructure Interfaces
  |
  +-- INetworkClient
  +-- IHashProvider
  +-- IFileSystem
  +-- ISignatureVerifier
  +-- IEventDispatcher
  +-- IProcessLauncher
  +-- IStateStore
  |
  v
Default Implementations
```

文件替换不在主应用进程内执行，而由独立 updater 子程序完成：

```text
Main App
  |
  | check / download / verify
  v
libAutoUpdater
  |
  | write apply-plan.json
  | launch updater process
  v
autoupdater_apply
  |
  | wait main process exit
  | backup old files
  | replace / remove files
  | verify result
  | rollback on failure
  | restart app
  v
Updated App
```

## 3. 推荐目录结构

```text
libAutoUpdater/
  CMakeLists.txt
  cmake/
    libAutoUpdaterConfig.cmake.in

  include/
    libAutoUpdater/
      Updater.h
      Config.h
      Version.h
      Manifest.h
      ApplyPlan.h
      Error.h
      Result.h
      Types.h
      interfaces/
        INetworkClient.h
        IHashProvider.h
        IFileSystem.h
        ISignatureVerifier.h
        IEventDispatcher.h
        IProcessLauncher.h
        IStateStore.h

  src/
    Updater.cpp
    Version.cpp
    Manifest.cpp
    ApplyPlan.cpp
    ManifestFetcher.cpp
    ManifestVerifier.cpp
    LocalSnapshotBuilder.cpp
    UpdatePlanner.cpp
    DownloadExecutor.cpp
    ApplyPlanWriter.cpp
    ApplyLauncher.cpp
    util/
      PathUtil.cpp
      JsonUtil.cpp
      UrlUtil.cpp
    default/
      CurlNetworkClient.cpp
      Sha256HashProvider.cpp
      StdFileSystem.cpp
      OpenSslSignatureVerifier.cpp
      NullSignatureVerifier.cpp
      DirectDispatcher.cpp
      ThreadDispatcher.cpp
      ProcessLauncher.cpp
      JsonStateStore.cpp

  updater/
    CMakeLists.txt
    main.cpp
    ApplyExecutor.h
    ApplyExecutor.cpp
    TransactionJournal.h
    TransactionJournal.cpp
    platform/
      FileReplace_win.cpp
      FileReplace_posix.cpp
      ProcessWait_win.cpp
      ProcessWait_posix.cpp
      Lock_win.cpp
      Lock_posix.cpp

  examples/
    cli/
      CMakeLists.txt
      main.cpp
    qt/
      CMakeLists.txt
      QtNetworkClient.h
      QtNetworkClient.cpp
      QtDispatcher.h
      QtDispatcher.cpp
      main.cpp

  tests/
    CMakeLists.txt
    VersionTests.cpp
    ManifestTests.cpp
    UpdatePlannerTests.cpp
    DownloadExecutorTests.cpp
    ApplyPlanTests.cpp
    MockNetworkClient.h
    MockFileSystem.h
    MockHashProvider.h
    MockStateStore.h

  tools/
    make_manifest.py

  docs/
    architecture-plan.md
    manifest.example.json
    apply-plan.example.json
```

## 4. 核心对象

### 4.1 Updater

`Updater` 是应用侧唯一需要直接使用的门面类。

职责：

- 管理状态机。
- 管理后台检查与下载线程。
- 暴露取消能力。
- 暴露检查结果、进度、错误、可应用更新等回调。
- 调用 orchestrator 完成实际流程。
- 通过 `IEventDispatcher` 投递回调。

典型使用：

```cpp
autoupdater::Config config = ...;
autoupdater::Updater updater(config);
updater.setCallbacks(callbacks);
updater.checkAndDownloadAsync();
```

状态机：

```text
Idle
  -> Checking
  -> UpToDate
  -> UpdateAvailable
  -> Downloading
  -> ReadyToApply
  -> Applying
  -> Failed
```

### 4.2 Version

`Version` 负责 SemVer 解析与比较。

支持格式：

- `1.2.3`
- `1.2.3-alpha`
- `1.2.3-beta.1`
- `1.2.3+build.5`
- `1.2.3-alpha.1+build.5`

比较规则遵循 SemVer：

- major、minor、patch 依次比较。
- release 版本高于 prerelease 版本。
- build metadata 不参与优先级比较。

### 4.3 Manifest

manifest 分两类：

1. Index manifest
2. Release manifest

Index manifest 用于路由不同平台、架构、渠道。

Release manifest 描述某个具体版本的文件、删除项、安全信息和兼容范围。

这样可以支持：

- stable / beta / canary 渠道
- Windows / macOS / Linux
- x64 / arm64
- 灰度发布
- 平台暂停更新
- 回滚发布

### 4.4 UpdatePlanner

`UpdatePlanner` 只负责决策，不做 IO。

输入：

- 当前配置快照。
- 当前版本。
- 远端 manifest。
- 本地文件快照。
- 持久化状态。

输出：

- 是否有更新。
- 是否需要完整重装。
- 是否强制更新。
- 需要下载的文件。
- 需要替换的文件。
- 需要删除的文件。
- 是否拒绝降级。
- 是否拒绝过期 manifest。

设计原则：

```text
UpdatePlanner = pure decision module
```

这样单元测试可以直接构造输入并断言输出，不需要真实文件系统或网络。

### 4.5 DownloadExecutor

职责：

- 根据 planner 输出下载缺失或 hash 不一致的文件。
- 下载到 staging 目录。
- staging 目录结构镜像安装目录。
- 下载后逐文件 SHA-256 校验。
- 失败后按配置重试。
- 支持取消。
- 支持断点续传。
- 上报进度。

断点续传需要保存：

- ETag
- Last-Modified
- Content-Length
- 已下载字节数

续传时使用：

```http
Range: bytes=<offset>-
If-Range: "<etag>"
```

如果 ETag 或 Last-Modified 不匹配，则丢弃临时文件重新下载。

### 4.6 ApplyPlan

`ApplyPlan` 是 updater 子程序的唯一输入，不让 updater 重新理解 manifest。

它是事务描述，而不是简单文件列表。

包含：

- schemaVersion
- appId
- fromVersion
- toVersion
- manifestSha256
- installDir
- stagingDir
- backupDir
- restartCommand
- operations

operation 类型：

- `replace`
- `remove`
- 后续可扩展 `chmod`、`mkdir`、`rmdir`、`symlink`

### 4.7 StateStore

`IStateStore` 负责持久化更新状态。

保存内容：

- 上次成功版本。
- 上次接受的 releaseId。
- 待健康确认的新版本。
- 上次失败原因。
- 回滚信息。
- 断点续传元信息。
- 防降级状态。

建议默认实现使用 JSON 文件：

```text
install/.autoupdater/state.json
```

## 5. Manifest 设计

### 5.1 Index Manifest

示例：

```json
{
  "schemaVersion": 1,
  "appId": "com.example.myapp",
  "channel": "stable",
  "generatedAt": "2026-06-01T10:00:00Z",
  "targets": [
    {
      "platform": "windows",
      "arch": "x64",
      "manifestUrl": "https://example.com/releases/1.4.0/windows-x64/manifest.json"
    },
    {
      "platform": "macos",
      "arch": "arm64",
      "manifestUrl": "https://example.com/releases/1.4.0/macos-arm64/manifest.json"
    }
  ]
}
```

第一阶段可以允许 `Config.manifestUrl` 直接指向 release manifest，index manifest 作为可选增强。

### 5.2 Release Manifest

示例：

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
  "baseUrl": "https://example.com/releases/1.4.0/windows-x64/",
  "files": [
    {
      "path": "bin/MyApp.exe",
      "sha256": "9b3f...",
      "size": 18432000
    },
    {
      "path": "resources/app.dat",
      "localPath": "resources/app.dat",
      "sha256": "1a2b...",
      "size": 7340032
    }
  ],
  "remove": [
    "plugins/old_plugin.dll"
  ]
}
```

### 5.3 Path 规则

manifest 中所有路径必须满足：

- 使用正斜杠 `/`。
- 必须是相对路径。
- 不允许为空。
- 不允许 `..`。
- 不允许绝对路径。
- 不允许 Windows drive prefix。
- 解析后必须位于 installDir 或 stagingDir 内。

这可以防止路径穿越和错误覆盖系统文件。

## 6. Apply Plan 设计

示例：

```json
{
  "schemaVersion": 1,
  "appId": "com.example.myapp",
  "fromVersion": "1.3.0",
  "toVersion": "1.4.0",
  "releaseId": "1.4.0+20260601.1",
  "manifestSha256": "abc...",
  "installDir": "C:/Program Files/MyApp",
  "stagingDir": "C:/Program Files/MyApp/.autoupdater/staging/1.4.0",
  "backupDir": "C:/Program Files/MyApp/.autoupdater/backup/1.3.0-to-1.4.0",
  "restartCommand": [
    "C:/Program Files/MyApp/MyApp.exe"
  ],
  "operations": [
    {
      "type": "replace",
      "source": "bin/MyApp.exe",
      "target": "bin/MyApp.exe",
      "sha256": "9b3f...",
      "size": 18432000
    },
    {
      "type": "remove",
      "target": "plugins/old_plugin.dll"
    }
  ]
}
```

updater 子程序执行 apply plan，并生成事务日志：

```text
install/.autoupdater/journal/<transaction-id>.json
```

事务日志记录每一步是否完成，便于中断后恢复或回滚。

## 7. 更新流程

### 7.1 检查更新

```text
checkAsync()
  |
  v
validate config
  |
  v
download manifest bytes
  |
  v
download manifest signature if required
  |
  v
verify manifest signature
  |
  v
parse manifest
  |
  v
validate schema / appId / platform / arch / channel
  |
  v
build local snapshot
  |
  v
plan update
  |
  +-- up to date
  +-- update available
  +-- reinstall required
  +-- rejected
```

### 7.2 下载更新

```text
checkAndDownloadAsync()
  |
  v
check update
  |
  v
create staging dir
  |
  v
download changed or missing files
  |
  v
verify sha256
  |
  v
write apply-plan.json
  |
  v
ReadyToApply
```

### 7.3 应用更新

```text
applyAndRestartAsync()
  |
  v
launch autoupdater_apply
  |
  v
main app exits
  |
  v
updater waits for pid
  |
  v
acquire update lock
  |
  v
backup affected files
  |
  v
execute operations
  |
  v
verify installed files
  |
  +-- success -> restart app
  +-- failure -> rollback -> report failure
```

### 7.4 健康确认

更新成功并不代表新版本可用。新版本启动后应主动调用：

```cpp
updater.markCurrentVersionHealthy();
```

流程：

```text
updater applied update
  |
  v
state = pending healthy confirmation
  |
  v
new app starts
  |
  v
markCurrentVersionHealthy()
  |
  v
clear backup / clear pending state
```

如果新版本未在约定时间内确认健康，则下一次启动时可以：

- 提示用户。
- 自动回滚。
- 或进入安全模式。

## 8. 安全策略

### 8.1 HTTPS

默认开启 TLS 证书校验。

允许配置关闭：

```cpp
network.verifyTls = false;
```

仅建议测试环境使用。

### 8.2 文件完整性

manifest 中每个文件必须包含 SHA-256。

下载后必须校验：

```text
download file
  |
  v
sha256 downloaded file
  |
  +-- match -> accept
  +-- mismatch -> retry
  +-- retry exhausted -> fail
```

### 8.3 Manifest 签名

生产环境推荐强制签名。

采用 detached signature：

```text
manifest.json
manifest.sig
```

验签流程：

```text
download manifest raw bytes
download signature bytes
verify raw bytes with public key
parse manifest only after signature passes
```

优点：

- 避免 JSON canonicalization 问题。
- 静态服务器容易部署。
- 服务器被篡改时无法伪造合法 manifest。

### 8.4 防降级

本地持久化：

- lastAcceptedVersion
- lastAcceptedReleaseId

默认拒绝低于已接受版本的 manifest。

只有在 manifest 显式设置 `allowDowngrade=true` 且签名有效时，才允许降级。

### 8.5 防重放

manifest 包含：

- publishedAt
- expiresAt

客户端拒绝过期 manifest。

如果本地系统时间明显异常，应返回可诊断错误。

### 8.6 下载源限制

`Config` 支持：

```cpp
std::vector<std::string> allowedBaseUrls;
```

即使 manifest 签名通过，也只允许从受信任的 baseUrl 下载文件。

## 9. 平台策略

### 9.1 安装布局

引入安装布局枚举：

```cpp
enum class InstallLayout {
    PortableDirectory,
    WindowsDirectory,
    MacOSAppBundle,
    LinuxAppImage,
    PackageManagerOwned
};
```

第一阶段重点支持：

- `PortableDirectory`
- `WindowsDirectory`
- `MacOSAppBundle` 的基础目录替换

对 `PackageManagerOwned` 默认返回不支持自更新，建议交给系统包管理器。

### 9.2 Windows

注意事项：

- 运行中的 `.exe` / `.dll` 不能直接覆盖。
- 使用独立 updater 等待主进程退出。
- 文件 API 使用宽字符路径。
- 支持长路径。
- 对短暂文件锁定做重试。
- 需要考虑 UAC 权限不足。

### 9.3 macOS

注意事项：

- `.app` 是 bundle 目录。
- 更新后 code signature 必须仍然有效。
- 需要尽量保留权限和 extended attributes。
- notarization 与 quarantine 属性需要谨慎处理。

### 9.4 Linux

注意事项：

- portable tarball / AppImage 更适合自更新。
- deb/rpm 管理的软件不建议绕过包管理器。
- 覆盖后需要保留可执行权限。

## 10. 并发与锁

主库侧：

- 单个 `Updater` 实例同一时间只允许一个任务。
- 支持取消检查和下载。
- 取消后状态回到 Idle 或 Failed，错误码为 Cancelled。

updater 子程序侧：

- 对安装目录加 single-instance lock。
- 防止两个 updater 同时覆盖同一个安装目录。

锁位置：

```text
install/.autoupdater/update.lock
```

Windows 可使用 named mutex 或 lock file。

POSIX 可使用 `flock` 或 lock file。

## 11. CMake 目标

建议导出目标：

```cmake
libAutoUpdater::libAutoUpdater
libAutoUpdater::autoupdater_apply
```

构建选项：

```cmake
LIBAUTOUPDATER_BUILD_UPDATER=ON
LIBAUTOUPDATER_BUILD_EXAMPLES=ON
LIBAUTOUPDATER_BUILD_TESTS=ON
LIBAUTOUPDATER_WITH_CURL=ON
LIBAUTOUPDATER_WITH_OPENSSL=ON
LIBAUTOUPDATER_WITH_QT=OFF
```

使用方式：

```cmake
find_package(libAutoUpdater CONFIG REQUIRED)
target_link_libraries(MyApp PRIVATE libAutoUpdater::libAutoUpdater)
```

也支持：

```cmake
add_subdirectory(external/libAutoUpdater)
target_link_libraries(MyApp PRIVATE libAutoUpdater)
```

## 12. 测试计划

### 12.1 单元测试

覆盖：

- SemVer 解析与比较。
- manifest 解析。
- schemaVersion 校验。
- platform / arch / channel 校验。
- minVersion 判断。
- mandatory 判断。
- 防降级判断。
- 过期 manifest 判断。
- 文件级差量计划。
- remove operation 生成。
- path traversal 拒绝。
- apply plan 序列化与反序列化。

### 12.2 Mock IO 测试

使用 mock 实现：

- `INetworkClient`
- `IFileSystem`
- `IHashProvider`
- `IStateStore`

覆盖：

- 下载成功。
- 下载失败重试。
- hash mismatch 重试。
- 断点续传。
- 取消下载。
- manifest 签名失败。

### 12.3 集成测试

覆盖：

- 本地 HTTP server 发布静态 manifest 和文件。
- CLI 示例检查并下载。
- updater 子程序在临时安装目录执行替换。
- 替换失败后回滚。
- 删除旧文件。

### 12.4 平台测试

Windows：

- exe 被占用时必须通过 updater 替换。
- Unicode 路径。
- 长路径。

macOS：

- `.app` bundle 目录替换。
- 权限保留。

Linux：

- 可执行权限保留。
- symlink 行为后续扩展验证。

## 13. 实施阶段

### Phase 1: 基础骨架

交付：

- CMake 工程。
- 公共头文件。
- `Version`。
- `Result` / `Error`。
- 接口定义。
- 基础测试框架。

验收：

- 项目可配置、可编译。
- `VersionTests` 通过。

### Phase 2: Manifest 与 Planner

交付：

- Release manifest 解析。
- schema 校验。
- path 校验。
- `LocalSnapshot` 数据结构。
- 纯函数化 `UpdatePlanner`。

验收：

- 能判断 up-to-date、update available、reinstall required。
- 能生成 replace / remove 操作计划。
- path traversal 测试通过。

### Phase 3: 下载与校验

交付：

- `INetworkClient`。
- libcurl 默认实现。
- `IHashProvider`。
- SHA-256 默认实现。
- `DownloadExecutor`。
- staging 目录镜像。
- 重试、取消、进度回调。

验收：

- 能按文件 hash 差量下载。
- hash mismatch 会重试。
- 取消下载能终止流程。

### Phase 4: Apply Plan 与 updater 子程序

交付：

- apply plan 生成。
- updater 子程序。
- 事务日志。
- 备份、覆盖、删除、校验、回滚。
- single-instance lock。

验收：

- 临时安装目录可以完成真实替换。
- 替换失败能回滚。
- remove operation 生效。

### Phase 5: 安全能力

交付：

- detached manifest signature。
- OpenSSL 验签实现。
- allowedBaseUrls。
- 防降级。
- 防重放。

验收：

- 签名失败拒绝更新。
- 过期 manifest 拒绝更新。
- 降级 manifest 默认拒绝。

### Phase 6: 示例、打包工具与文档

交付：

- CLI 示例。
- Qt 示例。
- `make_manifest.py`。
- README。
- manifest 示例。
- apply plan 示例。

验收：

- 从示例发布目录可以生成 manifest。
- CLI 示例可完成检查、下载、应用调度。
- README 覆盖接入和发布流程。

## 14. 第一版实现范围建议

为了控制复杂度，第一版建议明确范围：

必须实现：

- C++17 核心库。
- release manifest。
- SemVer。
- 文件级差量。
- SHA-256 校验。
- libcurl 网络实现。
- 独立 updater 子程序。
- apply plan。
- replace / remove。
- 回滚。
- CLI 示例。
- 打包脚本。
- 单元测试。

建议实现：

- manifest detached signature。
- 防降级。
- 防重放。
- allowedBaseUrls。
- 健康确认。

可延后：

- index manifest。
- Qt 网络实现。
- AppImage 专用流程。
- macOS extended attributes 深度处理。
- 二进制 patch。
- 灰度发布策略。

## 15. 关键风险

### 15.1 权限不足

安装目录位于系统目录时，普通用户可能无法覆盖文件。

处理策略：

- 明确错误码。
- 文档说明权限要求。
- 后续可扩展提权 helper。

### 15.2 macOS 签名破坏

替换 bundle 内文件可能导致 code signature 无效。

处理策略：

- 发布侧应整体签名。
- 更新后可执行 `codesign --verify` 作为外部校验。
- 文档明确约束。

### 15.3 updater 自身更新

updater 子程序运行时不能直接覆盖自身。

处理策略：

- 第一版不更新正在运行的 updater。
- 新 updater 可以随主应用下载，下一次更新再生效。

### 15.4 断电或进程被杀

apply 过程中可能中断。

处理策略：

- 事务日志记录每一步。
- 下次启动检测未完成事务。
- 支持继续回滚或清理。

## 16. 验收标准

整体完成后，库应满足：

- 应用接入检查更新不超过 5 行核心代码。
- 无 GUI 依赖。
- 支持 mock 网络和 mock 文件系统测试。
- 支持静态文件服务器发布更新。
- 下载只发生在缺失或 hash 不一致的文件上。
- 所有下载文件必须通过 SHA-256。
- manifest schema 不支持时能优雅失败。
- 版本低于 `minVersion` 时返回完整重装要求。
- updater 能安全覆盖运行中应用的文件。
- 覆盖失败能回滚。
- 新版本可通过健康确认清理备份。
- CMake 支持 `find_package` 和 `add_subdirectory`。

