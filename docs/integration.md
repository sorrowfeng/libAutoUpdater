# Integration Guide

## CMake add_subdirectory

```cmake
add_subdirectory(external/libAutoUpdater)
target_link_libraries(MyApp PRIVATE libAutoUpdater::libAutoUpdater)
```

## CMake find_package

安装后使用：

```cmake
find_package(libAutoUpdater CONFIG REQUIRED)
target_link_libraries(MyApp PRIVATE libAutoUpdater::libAutoUpdater)
```

## 网络后端

默认选择顺序：

- 本地路径和 `file://` 始终可用。
- CMake 找到 `CURL::libcurl` 时使用 libcurl。
- Windows 上没有 libcurl 且 `LIBAUTOUPDATER_WITH_WINHTTP=ON` 时使用 WinHTTP。
- macOS 上没有 libcurl 且 `LIBAUTOUPDATER_WITH_CFNETWORK=ON` 时使用 CFNetwork。
- Qt 用户可以注入 `QNetworkAccessManager` 适配器。

## 回调线程

`Updater` 在后台线程执行检查和下载。回调通过 `IEventDispatcher` 投递：

- 默认 dispatcher 会直接执行回调。
- GUI 应用应注入自己的 dispatcher，把回调投递回 UI 线程。
- Qt 示例提供了 `QtDispatcher`。

## 取消语义

调用 `Updater::cancel()` 会请求取消当前检查或下载。取消是协作式的：

- 网络后端会定期检查 token。
- 下载中断后可能留下临时文件和 resume 状态。
- 后续重试可以继续断点下载，具体取决于服务器是否支持 Range。

## 强制更新

manifest 中 `mandatory=true` 时，`CheckResult::mandatory` 会为 true。库不直接禁用 UI 的“稍后”，由调用方根据产品策略处理。

## package-manager-owned 安装

如果应用由系统包管理器管理，不建议自替换文件。可将 `Config::installLayout` 设为 `PackageManagerOwned`，库会拒绝自更新流程，调用方应引导用户使用系统包管理器。
