#!/usr/bin/env python3
"""使用当前 GitHub 仓库作为静态云端更新服务器，运行中文路径更新演示。"""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
import time
from pathlib import Path


DEFAULT_REPO = "sorrowfeng/libAutoUpdater"
DEFAULT_BRANCH = "main"
RELEASE_PATH = "examples/update-server/releases/2.0.0"

OLD_APP = """libAutoUpdater demo application
version=1.0.0
served-from=local-install
"""

NEW_APP = """libAutoUpdater demo application
version=2.0.0
served-from=github-raw
"""

UNCHANGED_SETTINGS = """{
  "channel": "stable",
  "theme": "system",
  "unchangedAcrossUpdate": true
}
"""


def default_manifest_url(repo: str, branch: str) -> str:
    return f"https://raw.githubusercontent.com/{repo}/{branch}/{RELEASE_PATH}/manifest.json"


def default_build_path(root: Path, *parts: str) -> Path:
    return root.joinpath("build", *parts)


def first_existing(candidates: list[Path]) -> Path:
    for candidate in candidates:
        if candidate.exists():
            return candidate
    return candidates[0]


def default_cli(root: Path) -> Path:
    if sys.platform == "win32":
        return first_existing([
            default_build_path(root, "dev", "examples", "cli", "Debug", "libAutoUpdater_cli.exe"),
            default_build_path(root, "examples", "cli", "Debug", "libAutoUpdater_cli.exe"),
            default_build_path(root, "examples", "cli", "Release", "libAutoUpdater_cli.exe"),
        ])
    return first_existing([
        default_build_path(root, "dev", "examples", "cli", "libAutoUpdater_cli"),
        default_build_path(root, "examples", "cli", "libAutoUpdater_cli"),
    ])


def default_updater(root: Path) -> Path:
    if sys.platform == "win32":
        return first_existing([
            default_build_path(root, "dev", "updater", "Debug", "autoupdater_apply.exe"),
            default_build_path(root, "updater", "Debug", "autoupdater_apply.exe"),
            default_build_path(root, "updater", "Release", "autoupdater_apply.exe"),
        ])
    return first_existing([
        default_build_path(root, "dev", "updater", "autoupdater_apply"),
        default_build_path(root, "updater", "autoupdater_apply"),
    ])


def write_text(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="\n") as handle:
        handle.write(text)


def print_tree(root: Path) -> None:
    for path in sorted(root.rglob("*")):
        rel = path.relative_to(root).as_posix()
        if rel.startswith(".autoupdater/") or rel == ".autoupdater":
            continue
        if path.is_file():
            print(f"  {rel}")


def print_file(root: Path, rel: str) -> None:
    path = root / rel
    print(f"\n--- {rel} ---")
    if path.exists():
        print(path.read_text(encoding="utf-8").rstrip())
    else:
        print("<不存在>")


def assert_text(path: Path, expected: str) -> None:
    actual = path.read_text(encoding="utf-8")
    if actual != expected:
        raise AssertionError(f"{path} 期望内容 {expected!r}，实际内容 {actual!r}")


def create_old_install(install_dir: Path) -> None:
    if install_dir.exists():
        shutil.rmtree(install_dir)
    write_text(install_dir / "bin" / "demo_app.txt", OLD_APP)
    write_text(install_dir / "config" / "settings.json", UNCHANGED_SETTINGS)
    write_text(install_dir / "legacy" / "remove-me.txt", "这个旧文件应该被更新流程删除。\n")


def should_retry_cli(result: subprocess.CompletedProcess[str]) -> bool:
    combined_output = result.stdout + result.stderr
    return (
        (result.returncode != 0 or "state=Failed" in combined_output)
        and "NetworkError" in combined_output
        and "No HTTP network adapter is available" not in combined_output
        and "state=Applying" not in combined_output
    )


def run_update_cli(command: list[str], attempts: int) -> subprocess.CompletedProcess[str]:
    result: subprocess.CompletedProcess[str] | None = None
    for attempt in range(1, attempts + 1):
        if attempts > 1:
            print(f"  第 {attempt}/{attempts} 次尝试")
        result = subprocess.run(command, capture_output=True, encoding="utf-8", errors="replace", text=True, timeout=60)
        if not should_retry_cli(result) or attempt == attempts:
            return result
        print("  遇到临时网络错误，准备重试...")
        time.sleep(1)
    assert result is not None
    return result


def main() -> int:
    repo_root = Path(__file__).resolve().parents[1]

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", default=DEFAULT_REPO, help="作为 raw.githubusercontent.com 源的 GitHub owner/repo")
    parser.add_argument("--branch", default=DEFAULT_BRANCH, help="GitHub 分支或 tag")
    parser.add_argument("--manifest-url", default="", help="覆盖默认 manifest URL")
    parser.add_argument("--work-dir", type=Path, default=repo_root / "demo-work" / "中文云端更新")
    parser.add_argument("--cli", type=Path, default=default_cli(repo_root))
    parser.add_argument("--updater", type=Path, default=default_updater(repo_root))
    parser.add_argument("--attempts", type=int, default=3, help="遇到临时网络错误时最多尝试几次")
    args = parser.parse_args()

    manifest_url = args.manifest_url or default_manifest_url(args.repo, args.branch)
    install_dir = args.work_dir.resolve() / "安装目录"

    print("GitHub 云端更新中文路径演示")
    print(f"  manifest: {manifest_url}")
    print(f"  本地安装目录: {install_dir}")
    print(f"  CLI:          {args.cli}")
    print(f"  updater:      {args.updater}")

    if not args.cli.exists():
        print("\n没有找到 CLI 可执行文件，请先构建项目：")
        print("  cmake --preset dev")
        print("  cmake --build --preset dev --parallel")
        return 2
    if not args.updater.exists():
        print("\n没有找到 updater 可执行文件，请开启 LIBAUTOUPDATER_BUILD_UPDATER 后重新构建。")
        return 2

    create_old_install(install_dir)

    print("\n更新前")
    print_tree(install_dir)
    print_file(install_dir, "bin/demo_app.txt")
    print_file(install_dir, "legacy/remove-me.txt")

    command = [
        str(args.cli),
        "--manifest",
        manifest_url,
        "--version",
        "1.0.0",
        "--install",
        str(install_dir),
        "--updater",
        str(args.updater),
        "--apply",
    ]

    print("\n运行 libAutoUpdater CLI")
    result = run_update_cli(command, max(1, args.attempts))
    print(result.stdout)
    if result.stderr:
        print(result.stderr, file=sys.stderr)
    combined_output = result.stdout + result.stderr
    if result.returncode != 0 or "state=Failed" in combined_output:
        if "No HTTP network adapter is available" in combined_output:
            print("当前构建没有 HTTP/HTTPS 网络后端。")
            print("Windows: 重新构建时开启 LIBAUTOUPDATER_WITH_WINHTTP=ON。")
            print("macOS: 重新构建时开启 LIBAUTOUPDATER_WITH_CFNETWORK=ON。")
            print("Linux: 安装 libcurl 开发包并开启 LIBAUTOUPDATER_WITH_CURL=ON。")
        return result.returncode if result.returncode != 0 else 1

    deadline = time.time() + 20
    last_error: object = "更新结果暂时还不可见"
    while time.time() < deadline:
        try:
            assert_text(install_dir / "bin" / "demo_app.txt", NEW_APP)
            assert_text(install_dir / "config" / "settings.json", UNCHANGED_SETTINGS)
            assert_text(install_dir / "resources" / "feature.txt", "New file delivered by libAutoUpdater 2.0.0.\n")
            if (install_dir / "legacy" / "remove-me.txt").exists():
                last_error = "legacy/remove-me.txt 仍然存在"
            else:
                break
        except (AssertionError, FileNotFoundError):
            last_error = sys.exc_info()[1]
            time.sleep(0.2)
    else:
        print("\n等待外部 updater 超时，当前安装目录内容：")
        print_tree(install_dir)
        print_file(install_dir, "bin/demo_app.txt")
        print_file(install_dir, "config/settings.json")
        print_file(install_dir, "resources/feature.txt")
        print_file(install_dir, "legacy/remove-me.txt")
        raise AssertionError(f"外部 updater 未在超时时间内完成：{last_error}")

    print("\n更新后")
    print_tree(install_dir)
    print_file(install_dir, "bin/demo_app.txt")
    print_file(install_dir, "resources/feature.txt")
    print_file(install_dir, "legacy/remove-me.txt")
    print("\n演示完成：文件已从 GitHub Raw 云端下载，并在中文本地路径下完成替换。")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
