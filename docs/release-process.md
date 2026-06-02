# Release Process

## Versioning

项目使用 SemVer。

发版前必须同步：

- `CMakeLists.txt` 中的 `project(... VERSION X.Y.Z)`。
- `vcpkg.json` 中的 `version-string`。
- `CHANGELOG.md` 中的 `## vX.Y.Z` 小节。

预检：

```sh
python tools/check_release_ready.py --tag vX.Y.Z
```

## Local Checks

```sh
git diff --check
cmake --preset dev
cmake --build --preset dev --parallel
ctest --preset dev
```

## Release Tag

```sh
git tag -a vX.Y.Z -m "libAutoUpdater vX.Y.Z"
git push origin vX.Y.Z
```

GitHub Release workflow 会：

1. 在 Linux、macOS、Windows 构建 Release。
2. 执行 `cmake --install`。
3. 打包安装树 zip。
4. 生成 `.sha256`。
5. 生成 SPDX SBOM。
6. 从 `CHANGELOG.md` 抽取 release notes。
7. 创建或更新 GitHub Release。

## Release Artifacts

每个平台应包含：

- `libAutoUpdater-vX.Y.Z-Linux.zip`
- `libAutoUpdater-vX.Y.Z-macOS.zip`
- `libAutoUpdater-vX.Y.Z-Windows.zip`
- 对应 `.sha256`
- 对应 `.spdx.json`

## Post-release

发布后检查：

- Release 页面不是 draft。
- 三个平台 zip 均存在。
- sha256 和 SBOM 均存在。
- CI、CodeQL、Release workflow 均通过。
