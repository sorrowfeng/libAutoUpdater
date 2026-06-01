# GitHub Raw Update Server Example

This directory is a static update feed that can be served directly by GitHub.

The demo client downloads:

```text
https://raw.githubusercontent.com/sorrowfeng/libAutoUpdater/main/examples/update-server/releases/2.0.0/manifest.json
```

No custom backend is required. The manifest points every managed file at the
same `raw.githubusercontent.com` release directory.

