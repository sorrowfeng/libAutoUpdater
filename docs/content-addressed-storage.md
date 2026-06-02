# Content-Addressed Storage

Content-addressed storage avoids duplicating unchanged files in every release directory.

## Core Idea

- The manifest still describes the complete managed file set for the target version.
- Server files are stored in an object store by SHA-256.
- Identical content is stored only once.
- Manifest `path` points to the server object, while `localPath` points to the installation path.

## Generate a Content-Addressed Manifest

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

Example output:

```text
publish/updates/
  releases/1.2.3/windows-x64/manifest.json
  objects/sha256/9b/9b920c148faf74af60cc7e010b832542a011426c1b2ac3e185c1f0a2d46b1fd4
```

Example manifest entry:

```json
{
  "path": "objects/sha256/9b/9b920c148faf74af60cc7e010b832542a011426c1b2ac3e185c1f0a2d46b1fd4",
  "localPath": "bin/MyApp.exe",
  "sha256": "9b920c148faf74af60cc7e010b832542a011426c1b2ac3e185c1f0a2d46b1fd4",
  "size": 123456
}
```

## Garbage-Collect Unreferenced Objects

Dry run:

```sh
python tools/gc_objects.py publish/updates/objects/sha256 \
  --manifest publish/updates/releases/1.2.3/windows-x64/manifest.json
```

Delete after review:

```sh
python tools/gc_objects.py publish/updates/objects/sha256 \
  --manifest publish/updates/releases/1.2.3/windows-x64/manifest.json \
  --delete
```

To retain the latest N releases, pass every retained release manifest through `--manifest`.

## Notes

- The object filename is the SHA-256 digest. Do not rename object files after upload.
- `baseUrl` should point above the object prefix, for example `https://example.com/updates/`.
- `allowedBaseUrls` should allow the same parent URL.
- Old installed files are still removed through manifest `remove[]`.
