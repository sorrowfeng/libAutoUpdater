# CHANGELOG

## Unreleased

### 新增

- 补齐开源治理文件、issue/PR 模板、API 文档、安全模型、集成文档、服务器布局、排障文档、质量门禁文档和生态打包模板。
- `tools/make_manifest.py` 支持内容寻址对象存储模式，可用 `path` 指向服务器 object、`localPath` 指向安装路径。
- 新增 `tools/gc_objects.py`、`tools/check_release_ready.py`、`tools/extract_changelog.py` 和 `tools/make_sbom.py`。
- CI 新增 clang-format、clang-tidy、coverage 和内容寻址打包工具校验；release workflow 新增 SBOM 和 CHANGELOG release notes。
- 新增轻量 fuzz smoke 测试，覆盖版本、manifest、index、apply plan 和路径校验解析入口。

### 变更

- `Config::clientVersion` 默认值改为当前库版本，避免版本元数据漂移。
- `no-optional-deps` preset 在 Windows 上也关闭 WinHTTP，更符合“无可选 HTTP 后端”的语义。

## v0.1.2 - 2026-06-02

### 修复

- 修复 Windows 上 `autoupdater_apply` 使用控制台子系统构建时可能弹出控制台窗口的问题，现在 updater 子程序以 GUI subsystem 构建，减少桌面应用自更新时的界面干扰。

## v0.1.1 - 2026-06-02

### 修复

- 修复 updater 子目录在被 `add_subdirectory` 或外部项目集成时错误使用 `CMAKE_SOURCE_DIR` 的问题，现在改用 `libAutoUpdater_SOURCE_DIR`。
- 修复受管文件路径在 `safeJoin` 中按本地窄字符路径处理的问题，现在按 UTF-8 解码，改善中文等非 ASCII 路径的跨平台表现。

## v0.1.0 - 2026-06-02

### 初始版本

- 提供 C++17 跨平台在线更新库核心、外部 updater 子程序、CLI 示例、打包脚本、CI、真实 GitHub Raw 更新示例。
