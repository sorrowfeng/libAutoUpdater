# Public API Overview

公共 API 位于 `include/libAutoUpdater/`。

如果本机安装了 Doxygen，可以生成 HTML API 文档：

```sh
doxygen Doxyfile
```

输出目录为 `build/docs/html`。

## Updater

`Updater` 是门面类，负责：

- 检查 manifest。
- 规划更新。
- 下载变更文件。
- 写入 apply plan。
- 启动外部 updater。
- 周期检查、取消和回滚入口。

`Updater` 对调用方暴露异步方法。检查、下载和 apply 调度在后台执行；回调通过 `IEventDispatcher` 投递。

## Config

`Config` 描述客户端环境和策略：

- 应用身份：`appId`、`channel`、`platform`、`arch`。
- 版本：`currentVersion`、`clientVersion`。
- 路径：`installDir`、`tempDir`、`updaterExecutable`。
- 网络策略：超时、TLS、断点续传。
- 安全策略：签名、公钥、URL allowlist、防降级、防过期 manifest。
- apply 策略：等待超时、健康确认超时。

## Manifest

`Manifest` 表示服务端 release manifest。`files[]` 描述目标版本完整受管文件集合。下载时只下载本地缺失或 SHA-256 不一致的文件。

`ManifestFile::path` 是服务器路径，`ManifestFile::localPath` 是可选落地路径。内容寻址模式依赖这个分离。

## Result and Error

公共 API 不向调用方穿透异常。失败通过：

- `Result<T>`
- `ErrorCode`
- `Error::message`

表示。

## Interfaces

可注入接口：

- `INetworkClient`
- `IHashProvider`
- `IFileSystem`
- `ISignatureVerifier`
- `IEventDispatcher`
- `IProcessLauncher`
- `IStateStore`

测试、Qt 集成、自定义网络栈和沙箱环境应通过这些接口接入。

## Threading Contract

- `Updater` 方法可从应用主线程调用。
- 同一个 `Updater` 实例内部序列化状态转换。
- 回调不保证天然在 UI 线程；由 dispatcher 决定。
- 调用方应避免在回调中长时间阻塞。
- `cancel()` 是协作式取消，可能不会立即中断底层系统调用。

## ABI Note

项目目前 `0.x` 阶段不承诺稳定 ABI。建议使用 CMake package 或源码方式集成，并在升级时重新编译。
