# Security Model

`libAutoUpdater` 的安全目标是：即使更新文件托管在静态 HTTP/HTTPS 服务器上，也尽量减少恶意更新、路径穿越、降级攻击和失败替换造成的风险。

## Threat Model

主要关注：

- manifest 或更新文件被篡改。
- CDN、对象存储或静态服务器配置错误。
- 旧版本 manifest 被重放。
- 攻击者试图通过 manifest 写入安装根之外的文件。
- 更新过程中崩溃或断电导致安装目录不一致。

不直接解决：

- 主机已经被管理员权限恶意软件控制。
- 调用方把 `verifyTls=false` 或签名校验关闭后仍信任不可信网络。
- package manager 管理的安装目录由应用私自替换。

## Manifest Signature

推荐生产环境开启：

```cpp
config.security.requireManifestSignature = true;
config.security.manifestSignatureUrl = "https://example.com/updates/releases/1.2.3/manifest.json.sig";
config.security.publicKeyPem = "... built-in public key ...";
```

建议：

- 私钥只放在离线或受保护的发版环境。
- 公钥内置在客户端，或通过应用自身可信配置分发。
- key rotation 应通过新客户端版本完成。
- 签名覆盖原始 `manifest.json` 字节，不要在签名前后重新格式化 JSON。

## HTTPS and TLS

默认 `NetworkOptions::verifyTls=true`。生产环境不要关闭 TLS 校验。  
签名和 HTTPS 是互补关系：

- HTTPS 保护传输链路和隐私。
- Manifest 签名保护静态服务器被控后的更新完整性。

## Base URL Allowlist

`SecurityOptions::allowedBaseUrls` 可限制 manifest 和 index 选择出的 URL 必须位于可信前缀下。前缀匹配必须包含完整 host 边界，避免 `https://trusted.example.com.evil.com` 这类绕过。

## Path Safety

所有 manifest 文件路径必须是受管相对路径：

- 拒绝绝对路径。
- 拒绝 `..`。
- 拒绝 Windows drive prefix。
- 拒绝空路径。
- 路径使用正斜杠。

更新文件先下载到 staging 目录，验证 SHA-256 后才写入 apply plan。

## Downgrade and Replay

默认开启：

- `rejectDowngrade=true`
- `rejectExpiredManifest=true`

manifest 可以设置：

- `publishedAt`
- `expiresAt`
- `releaseId`
- `allowDowngrade`

只有明确允许时才应接受降级。

## Apply and Rollback

替换由 `autoupdater_apply` 完成：

1. 等待主程序退出。
2. 获取安装目录更新锁。
3. 备份将被替换或删除的文件。
4. 从 staging 镜像覆盖到安装目录。
5. 校验安装后的文件 SHA-256。
6. 失败则回滚备份。

调用方应在新版本启动成功后调用 `markCurrentVersionHealthy()`，用于确认上一版本可被认为健康。

## Security Review Checklist

修改以下区域时请补充测试和 PR 说明：

- manifest parser。
- path validation / safeJoin。
- URL allowlist。
- signature verifier。
- download resume。
- apply-plan schema。
- updater backup / rollback。
- process launch arguments。
