# Server Layout

`libAutoUpdater` only needs a static file server. No custom backend API is required.

## Version Directory Layout

This layout is simple and works well for small projects:

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

Manifest:

```json
{
  "schemaVersion": 1,
  "version": "1.1.0",
  "baseUrl": "https://example.com/updates/releases/1.1.0/windows-x64/",
  "files": [
    {
      "path": "bin/MyApp.exe",
      "sha256": "9b920c148faf74af60cc7e010b832542a011426c1b2ac3e185c1f0a2d46b1fd4",
      "size": 123
    }
  ]
}
```

The benefit is readability. The drawback is duplicated storage when unchanged files appear in many versions.

## Content-Addressed Layout

This layout is better for medium and large projects:

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

Manifest:

```json
{
  "schemaVersion": 1,
  "version": "1.1.0",
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

`path` is the server path. `localPath` is the installation path. Multiple versions can reference the same object.

## Index Manifest

Use an index manifest to route multi-platform releases:

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

`Config::manifestUrl` may point directly to a release manifest or to an index manifest.

An omitted or empty target `platform` or `arch` is a wildcard. Exact
platform/architecture pairs take precedence over one-dimensional wildcards,
which take precedence over a global wildcard. If two matching targets have the
same highest specificity, the index is rejected; array order never breaks a
tie. Selectors are case-sensitive and must be unique. With `make_index.py`, use
`*` for a wildcard, for example `--target "windows/*=releases/windows/manifest.json"`.
