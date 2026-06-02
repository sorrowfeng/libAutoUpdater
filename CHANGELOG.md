# CHANGELOG

## v0.1.1 - 2026-06-02

### 修复

- 修复 updater 子目录在被 `add_subdirectory` 或外部项目集成时错误使用 `CMAKE_SOURCE_DIR` 的问题，现在改用 `libAutoUpdater_SOURCE_DIR`。
- 修复受管文件路径在 `safeJoin` 中按本地窄字符路径处理的问题，现在按 UTF-8 解码，改善中文等非 ASCII 路径的跨平台表现。

## v0.1.0 - 2026-06-02

### 初始版本

- 提供 C++17 跨平台在线更新库核心、外部 updater 子程序、CLI 示例、打包脚本、CI、真实 GitHub Raw 更新示例。
