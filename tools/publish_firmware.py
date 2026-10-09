#!/usr/bin/env python3
"""把当前构建产物发布成仓库里的预编译镜像 firmware/firmware.bin。

为什么需要它:仓库里那份 firmware.bin 曾经落后源码 20 多个提交,而 README 还
写着"与当前源码对应"。手工 `cp` 解决不了这个问题 —— 手工步骤会忘,忘了也看不
出来。本脚本把"发布"变成一条命令,并顺手写下这台机器上无法伪造的台账:
构建产物时间、字节数、SHA-256、当时的源码提交与工作区是否干净。

    pio run                                   # 先构建
    python tools/publish_firmware.py          # 再发布
    python tools/publish_firmware.py --check  # 只校验现状,不写任何文件

产出三个文件:
    firmware/firmware.bin           应用镜像本体
    firmware/firmware.bin.sha256    摘要文件,格式与 sha256sum 兼容
    firmware/BUILD.txt              构建台账(体积/摘要/提交/时间),供 doc_check 校验
"""

import argparse
import hashlib
import subprocess
import sys
from datetime import datetime
from pathlib import Path

PROJECT_DIR = Path(__file__).resolve().parent.parent
BUILD_DIR = PROJECT_DIR / ".pio" / "build"
OUT_DIR = PROJECT_DIR / "firmware"
DEFAULT_ENV = "esp32-s3-n16r8"

# 台账里记录"源码是否干净"时,只关心真会影响镜像内容的路径。
# 改 README、改设计稿不该让镜像看起来是脏的。
SOURCE_PATHS = ("src", "include", "platformio.ini")


def git(*args: str) -> str:
    try:
        return subprocess.run(
            ["git", *args],
            cwd=PROJECT_DIR,
            capture_output=True,
            text=True,
            check=True,
        ).stdout.strip()
    except (subprocess.CalledProcessError, FileNotFoundError):
        return ""


def sha256_of(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def read_define(header: Path, name: str) -> str:
    """从 C 头文件里取 `#define NAME "value"` 的值,取不到返回空串。"""
    if not header.exists():
        return ""
    for line in header.read_text(encoding="utf-8", errors="replace").splitlines():
        stripped = line.strip()
        if stripped.startswith(f"#define {name} "):
            return stripped.split(None, 2)[2].strip().strip('"')
    return ""


def read_ini_value(ini: Path, section: str, key: str) -> str:
    """只在指定的 [section] 内查找 ini 键。

    必须限定 section:platformio.ini 里 `platform` 在固件与主机测试两个
    环境下各出现一次,不限定就会读到错的那一个。
    """
    if not ini.exists():
        return ""
    current = ""
    for line in ini.read_text(encoding="utf-8", errors="replace").splitlines():
        stripped = line.strip()
        if stripped.startswith("[") and stripped.endswith("]"):
            current = stripped[1:-1].strip()
            continue
        if current != section or "=" not in stripped or stripped.startswith(";"):
            continue
        name, _, value = stripped.partition("=")
        if name.strip() == key:
            return value.split(";")[0].strip()
    return ""


def dirty_source_files() -> list:
    out = git("status", "--porcelain", "--", *SOURCE_PATHS)
    return [line.strip() for line in out.splitlines() if line.strip()]


def collect(env: str) -> dict:
    build_bin = BUILD_DIR / env / "firmware.bin"
    if not build_bin.exists():
        sys.exit(
            f"找不到构建产物 {build_bin.relative_to(PROJECT_DIR)}\n"
            f"请先运行:pio run -e {env}"
        )

    bootloader = BUILD_DIR / env / "bootloader.bin"
    partitions = BUILD_DIR / env / "partitions.bin"
    dirty = dirty_source_files()
    ini = PROJECT_DIR / "platformio.ini"

    return {
        "env": env,
        "version": read_define(PROJECT_DIR / "include" / "version.h", "FW_VERSION"),
        "platform": read_ini_value(ini, f"env:{env}", "platform"),
        "commit": git("rev-parse", "HEAD") or "unknown",
        "commit_short": git("rev-parse", "--short", "HEAD") or "unknown",
        "commit_date": git("log", "-1", "--format=%ad", "--date=short") or "unknown",
        "source_dirty": "true" if dirty else "false",
        "source_dirty_files": " ".join(dirty) if dirty else "-",
        "build_time": datetime.fromtimestamp(build_bin.stat().st_mtime).strftime(
            "%Y-%m-%d %H:%M:%S"
        ),
        "size": build_bin.stat().st_size,
        "sha256": sha256_of(build_bin),
        "bootloader_size": bootloader.stat().st_size if bootloader.exists() else 0,
        "partitions_size": partitions.stat().st_size if partitions.exists() else 0,
    }


def render_manifest(meta: dict) -> str:
    lines = [
        "# firmware.bin 构建台账 —— 由 tools/publish_firmware.py 自动生成,请勿手改",
        "# 校验:python tools/doc_check.py   刷新:pio run && python tools/publish_firmware.py",
        f"env = {meta['env']}",
        f"version = {meta['version']}",
        f"platform = {meta['platform']}",
        f"commit = {meta['commit']}",
        f"commit_short = {meta['commit_short']}",
        f"commit_date = {meta['commit_date']}",
        f"source_dirty = {meta['source_dirty']}",
        f"source_dirty_files = {meta['source_dirty_files']}",
        f"build_time = {meta['build_time']}",
        f"size = {meta['size']}",
        f"sha256 = {meta['sha256']}",
        f"bootloader_size = {meta['bootloader_size']}",
        f"partitions_size = {meta['partitions_size']}",
        "",
        "# 烧录偏移见 README「烧录预编译固件」一节:本文件是纯应用镜像,只能写 0x10000。",
    ]
    return "\n".join(lines) + "\n"


def read_manifest() -> dict:
    manifest = OUT_DIR / "BUILD.txt"
    meta = {}
    if not manifest.exists():
        return meta
    for line in manifest.read_text(encoding="utf-8").splitlines():
        if "=" in line and not line.startswith("#"):
            key, _, value = line.partition("=")
            meta[key.strip()] = value.strip()
    return meta


def report(meta: dict) -> None:
    print("预编译镜像台账")
    print(f"  环境      {meta['env']}")
    print(f"  版本      {meta['version']}  (platform {meta['platform']})")
    print(f"  源码提交  {meta['commit_short']}  {meta['commit_date']}")
    if meta["source_dirty"] == "true":
        print(f"  工作区    不干净 —— 镜像含未提交改动:{meta['source_dirty_files']}")
    else:
        print("  工作区    干净")
    print(f"  构建时间  {meta['build_time']}")
    print(f"  体积      {int(meta['size'])} 字节")
    print(f"  SHA-256   {meta['sha256']}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("-e", "--env", default=DEFAULT_ENV, help="PlatformIO 环境名")
    parser.add_argument(
        "--check",
        action="store_true",
        help="只比对现状与台账,不写文件;不一致时返回码非 0",
    )
    args = parser.parse_args()

    build_bin = BUILD_DIR / args.env / "firmware.bin"
    if args.check:
        meta = read_manifest()
        if not meta:
            print("firmware/BUILD.txt 不存在,请先发布一次:python tools/publish_firmware.py")
            return 1
        report(meta)
        problems = []
        actual = OUT_DIR / "firmware.bin"
        if not actual.exists():
            problems.append("firmware/firmware.bin 不存在")
        else:
            if sha256_of(actual) != meta.get("sha256"):
                problems.append("firmware.bin 的 SHA-256 与 BUILD.txt 不一致")
            if actual.stat().st_size != int(meta.get("size", -1)):
                problems.append("firmware.bin 的体积与 BUILD.txt 不一致")
        if build_bin.exists():
            if sha256_of(build_bin) != meta.get("sha256"):
                print(
                    "\n提示:当前 .pio 构建产物与已发布镜像不同 —— "
                    "源码改过但没重新发布,跑一次 python tools/publish_firmware.py 即可。"
                )
        for problem in problems:
            print(f"  ✗ {problem}")
        return 1 if problems else 0

    meta = collect(args.env)
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    (OUT_DIR / "firmware.bin").write_bytes(build_bin.read_bytes())
    (OUT_DIR / "firmware.bin.sha256").write_text(
        f"{meta['sha256']}  firmware.bin\n", encoding="utf-8"
    )
    (OUT_DIR / "BUILD.txt").write_text(render_manifest(meta), encoding="utf-8")

    report(meta)
    if meta["source_dirty"] == "true":
        print(
            "\n注意:这次发布取自未提交的工作区,台账里已如实标注。\n"
            "      想让镜像与某个确定的提交严格对应,请先提交再重新发布一次。"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
