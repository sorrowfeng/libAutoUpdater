# Content-Addressed Storage

内容寻址存储用于解决“每个版本目录都复制完整文件导致服务器重复文件过多”的问题。

## 核心思想

- manifest 仍然描述目标版本的完整受管文件清单。
- 服务器文件按 SHA-256 存储到 object store。
- 相同内容只存一份。
- manifest 使用 `path` 指向 object，使用 `localPath` 指向安装路径。

## 生成内容寻址 manifest

```sh
python tools/make_manifest.py dist/MyApp \
  --content-addressed \
  --object-root publish/updates/objects/sha256 \
  --object-prefix objects/sha256 \
  --app-id com.example.myapp \
  --platform windows \
  --arch x64 \
  --version 1.2.3 \
  --release-date 2026-06-02T00:00:00Z \
  --base-url https://example.com/updates/ \
  --output publish/updates/releases/1.2.3/windows-x64/manifest.json
```

输出结构示例：

```text
publish/updates/
  releases/1.2.3/windows-x64/manifest.json
  objects/sha256/9b/9b920c148faf74af60cc7e010b832542a011426c1b2ac3e185c1f0a2d46b1fd4
```

manifest 条目示例：

```json
{
  "path": "objects/sha256/9b/9b920c148faf74af60cc7e010b832542a011426c1b2ac3e185c1f0a2d46b1fd4",
  "localPath": "bin/MyApp.exe",
  "sha256": "9b920c148faf74af60cc7e010b832542a011426c1b2ac3e185c1f0a2d46b1fd4",
  "size": 123456
}
```

## 清理未引用对象

先 dry-run：

```sh
python tools/gc_objects.py publish/updates/objects/sha256 \
  --manifest publish/updates/releases/1.2.3/windows-x64/manifest.json
```

确认后删除：

```sh
python tools/gc_objects.py publish/updates/objects/sha256 \
  --manifest publish/updates/releases/1.2.3/windows-x64/manifest.json \
  --delete
```

保留最近 N 个版本时，把这些版本的 manifest 都传给 `--manifest`。

## 注意事项

- object 文件名就是 SHA-256，上传时不要改名。
- `baseUrl` 应指向 object prefix 的上层，例如 `https://example.com/updates/`。
- `allowedBaseUrls` 也应允许这个上层 URL。
- 删除旧安装文件仍然使用 manifest 的 `remove[]`。
