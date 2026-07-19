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

The collector validates every manifest and object-store entry before deleting
anything. It applies the library's default JSON depth, node, string, number,
and container limits as well as the manifest schema. Invalid JSON, duplicate
keys, schema/type errors, malformed object paths, missing referenced objects,
and size, digest, or shard mismatches fail closed. `--delete` streams and
verifies the SHA-256 of every retained object before removing the first stale
object; a dry run verifies declared sizes but does not hash object contents.

The object store must contain exactly two levels of real directories and
regular files. Symbolic links, Windows junctions, and other reparse points are
rejected. On Windows this restriction applies to every directory component in
the absolute path to the store, because Python pathname deletion cannot be made
relative to a directory handle. The complete path chain is guarded while the
collector runs; on POSIX, traversal and deletion instead use no-following
directory-relative operations. Every shard and stale-file identity is checked
again immediately before use or removal. Empty shard directories may remain
after collection.

Run deletion in an exclusive maintenance window: do not upload, replace, or
publish manifests or objects while the collector is running. Directory guards
prevent path rebinding, but the tool cannot make an uncooperative publisher's
multi-file changes transactional; a concurrent mutation can therefore stop a
run after earlier stale objects have already been removed.

When validated manifests contain zero object references, deletion from a
non-empty store is also refused by default. Use `--allow-empty` together with
`--delete` only when intentionally removing every object.

## Notes

- The object filename is the SHA-256 digest. Do not rename object files after upload.
- `baseUrl` should point above the object prefix, for example `https://example.com/updates/`.
- `allowedBaseUrls` should allow the same parent URL.
- Old installed files are still removed through manifest `remove[]`.
