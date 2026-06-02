# Security Policy

`libAutoUpdater` 处理远程 manifest、下载文件和本地文件替换，属于安全敏感组件。请不要在公开 issue 中披露可利用的漏洞细节。

## Supported Versions

当前维护策略：

| Version | Supported |
| --- | --- |
| 最新 minor / patch | Yes |
| 更早版本 | Best effort |

在 `1.0.0` 之前，API 和 manifest 细节仍可能变化。安全修复会优先发布到最新版本。

## Reporting a Vulnerability

请通过 GitHub Security Advisory 报告漏洞，或通过仓库所有者公开资料中列出的安全联系方式私下联系。

报告时请尽量包含：

- 受影响版本或 commit。
- 影响平台。
- 复现步骤或最小 PoC。
- 预期影响，例如任意文件写入、降级攻击、签名绕过、路径穿越、回滚破坏。
- 你是否已经公开披露。

我们会尽快确认报告，并在可行时提供修复计划和发布时间。

## Security Scope

重点安全边界包括：

- manifest 签名验证。
- HTTPS/TLS 校验。
- `allowedBaseUrls` 限制。
- 路径穿越和绝对路径拒绝。
- 下载后 SHA-256 校验。
- 外部 updater 的备份、替换、校验和回滚。
- anti-downgrade 和 anti-replay 行为。

更多细节见 [docs/security-model.md](docs/security-model.md)。
