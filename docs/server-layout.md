# Server Layout

`libAutoUpdater` 只需要静态文件服务器，不需要自建后端 API。

## 版本目录模式

适合小项目，简单直观：

```text
updates/
  releases/
    1.0.0/
      windows-x64/
        manifest.json
        bin/MyApp.exe
        config/default.json
    1.1.0/
      windows-x64/
        manifest.json
        bin/MyApp.exe
        config/default.json
```

manifest:

```json
{
  "baseUrl": "https://example.com/updates/releases/1.1.0/windows-x64/",
  "files": [
    {
      "path": "bin/MyApp.exe",
      "sha256": "...",
      "size": 123
    }
  ]
}
```

优点是容易理解。缺点是多个版本之间相同文件会重复存储。

## 内容寻址模式

适合中大型项目：

```text
updates/
  releases/
    1.1.0/
      windows-x64/
        manifest.json
  objects/
    sha256/
      9b/
        9b920c148faf74af60cc7e010b832542a011426c1b2ac3e185c1f0a2d46b1fd4
```

manifest:

```json
{
  "baseUrl": "https://example.com/updates/",
  "files": [
    {
      "path": "objects/sha256/9b/9b920c148faf74af60cc7e010b832542a011426c1b2ac3e185c1f0a2d46b1fd4",
      "localPath": "bin/MyApp.exe",
      "sha256": "9b920c148faf74af60cc7e010b832542a011426c1b2ac3e185c1f0a2d46b1fd4",
      "size": 123
    }
  ]
}
```

`path` 是服务器路径，`localPath` 是安装目录落地路径。多个版本可引用同一个 object。

## Index Manifest

多平台发布可用 index manifest 路由：

```json
{
  "schemaVersion": 1,
  "appId": "com.example.myapp",
  "channel": "stable",
  "generatedAt": "2026-06-02T00:00:00Z",
  "targets": [
    {
      "platform": "windows",
      "arch": "x64",
      "manifestUrl": "https://example.com/updates/releases/1.1.0/windows-x64/manifest.json"
    }
  ]
}
```

客户端 `Config::manifestUrl` 可以指向 release manifest，也可以指向 index manifest。
