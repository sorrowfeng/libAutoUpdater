# Contributing to libAutoUpdater

感谢你愿意改进 `libAutoUpdater`。这个项目的目标是提供一个可被桌面应用长期依赖的 C++17 在线更新库，因此贡献时请优先考虑可测试性、跨平台行为和安全边界。

## 开发环境

必需工具：

- CMake 3.21 或更新版本，基础构建最低要求仍为 CMake 3.16。
- 支持 C++17 的编译器：MSVC、GCC、Clang 或 AppleClang。
- Python 3.9 或更新版本，用于打包脚本和端到端测试。

可选工具：

- vcpkg，用于提供 libcurl 和 OpenSSL。
- clang-format、clang-tidy、gcovr，用于质量检查。

## 本地构建

```sh
cmake --preset dev
cmake --build --preset dev --parallel
ctest --preset dev
```

Windows 上如果 `python` 命令指向 Microsoft Store stub，可以使用：

```powershell
uv run python -m py_compile tools/make_manifest.py tools/make_index.py tools/sign_manifest.py
```

## 重要设计原则

- 核心库不依赖 GUI 框架。
- 网络、哈希、文件系统、签名、进程、事件投递、状态存储都必须保持接口隔离。
- 不允许主程序直接覆盖自身文件；替换必须通过外部 updater 进程完成。
- 所有 manifest 路径必须经过安全校验，拒绝绝对路径和路径穿越。
- 公共 API 不应向调用方泄漏异常；使用 `Result<T>`、`ErrorCode` 和错误字符串。
- 新逻辑优先放在可单元测试的纯决策层，真实 IO 通过接口或端到端测试覆盖。

## 提交 PR 前检查

至少运行：

```sh
git diff --check
cmake --preset dev
cmake --build --preset dev --parallel
ctest --preset dev
```

如果改动涉及打包脚本，还应运行：

```sh
python tools/make_manifest.py --help
python tools/make_index.py --help
python tools/gc_objects.py --help
```

如果改动涉及安全、签名、路径、下载、apply 或 rollback，请在 PR 说明里写清楚风险边界和测试证据。

## 代码风格

- C++ 使用 C++17。
- 默认 4 空格缩进。
- 公共头文件应有简明 API 注释。
- 新文件默认 ASCII；中文文档可以使用 UTF-8。
- 不要把第三方库类型暴露到公共 API。

## 分支和提交

推荐提交前缀：

- `feat:` 新功能
- `fix:` 修复
- `docs:` 文档
- `test:` 测试
- `tools:` 工具脚本
- `ci:` CI/CD
- `chore:` 维护性变更

## Release 流程

发版前请确认：

1. `CMakeLists.txt` 里的 `project(... VERSION ...)` 已更新。
2. `vcpkg.json` 的 `version-string` 已同步。
3. `CHANGELOG.md` 中存在对应 `vX.Y.Z` 小节。
4. `main` 分支 CI 和 CodeQL 通过。
5. 推送 `vX.Y.Z` tag 后确认 Release workflow 成功上传三平台 zip、sha256 和 SBOM。
