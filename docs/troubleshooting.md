# Troubleshooting

## No HTTP network adapter is available

原因：当前构建没有可用 HTTP/HTTPS 后端。

解决：

- Linux/macOS：安装 libcurl development package，或 macOS 使用 CFNetwork。
- Windows：开启 `LIBAUTOUPDATER_WITH_WINHTTP=ON`。
- vcpkg：使用 `cmake --preset vcpkg-debug`。

## Manifest baseUrl is not allowed

原因：`SecurityOptions::allowedBaseUrls` 不包含 manifest 的 `baseUrl` 或 index 选出的 manifest URL。

解决：把完整可信前缀加入 allowlist，注意 host 边界。

## PathTraversalRejected

manifest 中存在非法路径：

- 绝对路径。
- `..`。
- Windows drive prefix。
- 空路径。

修复 manifest 或打包脚本输入。

## Server ignored Range request

断点续传时服务器返回 200 而不是 206。库会拒绝把已有 partial 文件和完整响应混在一起。

解决：

- 配置服务器支持 Range。
- 或关闭 `NetworkOptions::enableResume`。

## HashMismatch

下载文件 SHA-256 与 manifest 不一致。

检查：

- manifest 是否与实际上传文件对应。
- CDN 是否缓存了旧文件。
- 是否手工修改了 object 文件。
- manifest 签名是否覆盖了正确的 manifest。

## External updater did not replace files

检查：

- 主程序是否仍在运行。
- `updaterExecutable` 是否指向 `autoupdater_apply`。
- `apply-plan.json` 是否写入。
- 安装目录权限是否允许替换。
- 是否存在残留 `.autoupdater/update.lock`。

## Python command fails on Windows

Windows 上 `python` 可能指向 Microsoft Store alias。可以使用：

```powershell
uv run python --version
uv run python tools/make_manifest.py --help
```
