# Getting Started

本文给出 `libAutoUpdater` 的最短接入路径。完整架构见 [architecture-plan.md](architecture-plan.md)。

## 1. 构建库和 updater

```sh
cmake --preset dev
cmake --build --preset dev --parallel
ctest --preset dev
```

正式集成时可以安装到一个前缀目录：

```sh
cmake --preset release
cmake --build --preset release --parallel
cmake --install build/release --config Release --prefix install-package
```

## 2. 在应用中使用

```cpp
#include <libAutoUpdater/Updater.h>

autoupdater::Config config;
config.appId = "com.example.myapp";
config.platform = "windows";
config.arch = "x64";
config.manifestUrl = "https://example.com/updates/releases/1.2.3/manifest.json";
config.currentVersion = autoupdater::Version::parse("1.0.0").value();
config.installDir = "C:/Program Files/MyApp";
config.updaterExecutable = "C:/Program Files/MyApp/autoupdater_apply.exe";
config.restartCommand = {"C:/Program Files/MyApp/MyApp.exe"};

autoupdater::Updater updater(config);
updater.setCallbacks({
    .onCheckResult = [](const autoupdater::CheckResult& result) {
        // Update UI or log result.
    },
    .onProgress = [](const autoupdater::Progress& progress) {
        // Update progress UI.
    },
    .onReadyToApply = [] {
        // Ask user to restart now, or call applyAndRestartAsync().
    },
});
updater.checkAndDownloadAsync();
```

## 3. 生成 manifest

版本目录模式：

```sh
python tools/make_manifest.py dist/MyApp \
  --app-id com.example.myapp \
  --platform windows \
  --arch x64 \
  --version 1.2.3 \
  --release-date 2026-06-02T00:00:00Z \
  --base-url https://example.com/updates/releases/1.2.3/windows-x64/
```

内容寻址模式见 [content-addressed-storage.md](content-addressed-storage.md)。

## 4. 上传服务器文件

将 `manifest.json` 和 manifest 中引用的文件上传到任意静态 HTTP/HTTPS 服务器。客户端只需要能访问 `Config::manifestUrl`。

## 5. 执行更新

推荐流程：

1. 应用启动后静默检查。
2. 有更新时下载到 staging 目录。
3. 用户确认后调用 `applyAndRestartAsync()`。
4. 主程序退出。
5. `autoupdater_apply` 等待主程序退出、备份、覆盖、校验、失败回滚、重启主程序。
