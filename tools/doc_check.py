#!/usr/bin/env python3
"""校验文档与代码/产物是否仍然一致,并在 CI 里把漂移拦下来。

起因:README 的文件树里写着 `ui_model.h（194 行）`、`font_cn26.h（约 805 KB）`,
内联设置表也只列到第 27 项 —— 都是当初写对、后来代码变了没人回头改的。
这类数字没有任何机制守着,必然再次跑偏,所以这里把它们变成断言。

    python tools/doc_check.py            # 有 ERROR 时返回码 1
    python tools/doc_check.py --strict   # WARN 也当 ERROR(想卡得更紧时用)

校验范围:
  1. README 文件树声明的行数 / 字库体积 vs 实际文件
  2. 设置项数:枚举 Count、README 内联表、docs/settings.md 表、"N 项"字样
  3. firmware.bin 台账自洽:体积与 SHA-256 三方(BUILD.txt / .sha256 / 文件本体)
  4. 镜像新鲜度:源码在台账记录的提交之后是否又改过(仅告警,不阻塞)
  5. 字库覆盖:源码字符串字面量用到的字符必须都在 font_cn*.h 里(见 tools/font_check.py)

只依赖标准库与 git;没有 git(例如从 tarball 解压)时跳过第 4 项。
第 5 项复用 tools/font_check.py 的实现,不重复写一份扫描器。
"""

import argparse
import hashlib
import importlib.util
import re
import subprocess
import sys
from pathlib import Path

PROJECT_DIR = Path(__file__).resolve().parent.parent
README = PROJECT_DIR / "README.md"
DOCS_DIR = PROJECT_DIR / "docs"
SETTINGS_DOC = DOCS_DIR / "settings.md"
UI_MODEL_HEADER = PROJECT_DIR / "include" / "ui_model.h"
FIRMWARE_DIR = PROJECT_DIR / "firmware"
MANIFEST = FIRMWARE_DIR / "BUILD.txt"

# 解析文件树里的短名时按此顺序找(README 的树把 include/ 与 src/ 摊平了)。
SEARCH_DIRS = ("include", "src", "tools", ".", "docs")


class Report:
    def __init__(self) -> None:
        self.errors: list = []
        self.warnings: list = []
        self.passed = 0

    def error(self, text: str, fix: str = "") -> None:
        self.errors.append((text, fix))

    def warn(self, text: str, fix: str = "") -> None:
        self.warnings.append((text, fix))

    def ok(self) -> None:
        self.passed += 1


def resolve(name: str):
    for directory in SEARCH_DIRS:
        candidate = PROJECT_DIR / directory / name
        if candidate.exists():
            return candidate
    return None


def count_lines(path: Path) -> int:
    with path.open("rb") as handle:
        return sum(1 for _ in handle)


def sha256_of(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


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


def read_manifest() -> dict:
    if not MANIFEST.exists():
        return {}
    meta = {}
    for line in MANIFEST.read_text(encoding="utf-8").splitlines():
        if "=" in line and not line.startswith("#"):
            key, _, value = line.partition("=")
            meta[key.strip()] = value.strip()
    return meta


# --------------------------- 1. 文件树里的数字 ---------------------------


# 只认文件树行(以 │ ├ └ 开头)。放开到全文会误伤 —— 例如
# 「每屏只显示 5 行」那句同页出现了 `ui_preview.html`,会被当成"该文件 5 行"。
TREE_LINE = re.compile(r"^\s*[│├└]")
NAME_IN_TREE = re.compile(r"([A-Za-z0-9_]+\.(?:h|cpp|py|html|md|bin|txt))")
LINES_CLAIM = re.compile(r"[（(](\d+)\s*行[）)]")
SIZE_CLAIM = re.compile(r"约\s*(\d+(?:\.\d+)?)\s*KB")


def check_tree_numbers(report: Report) -> None:
    """README 文件树中形如 `xxx.h ...（194 行）` / `...（约 336 KB）` 的声明。"""
    for line in README.read_text(encoding="utf-8").splitlines():
        if not TREE_LINE.match(line):
            continue
        name_match = NAME_IN_TREE.search(line)
        if name_match is None:
            continue
        name = name_match.group(1)
        line_claim = LINES_CLAIM.search(line)
        size_claim = SIZE_CLAIM.search(line)
        if line_claim is None and size_claim is None:
            continue

        path = resolve(name)
        if path is None:
            report.error(
                f"README 文件树里的 {name} 找不到对应文件", "改对文件名或删掉该声明"
            )
            continue

        if line_claim is not None:
            claimed, actual = int(line_claim.group(1)), count_lines(path)
            if claimed != actual:
                report.error(
                    f"{name} 行数不符:README 写 {claimed} 行,实际 {actual} 行",
                    f"把 README 里的 {claimed} 改成 {actual}",
                )
            else:
                report.ok()

        if size_claim is not None:
            claimed = float(size_claim.group(1))
            size = path.stat().st_size
            # "约 N KB" 的约定是"N = 实际 KB 四舍五入",故按整数精确比对。
            # 曾用 2% 容差,结果 805→793 这种真实漂移(1.5%)被吞掉了。
            if round(size / 1024) != round(claimed):
                report.error(
                    f"{name} 体积不符:README 写约 {claimed:g} KB,"
                    f"实际 {size} 字节 = {size / 1024:.1f} KB",
                    f"把 README 里的 {claimed:g} 改成 {round(size / 1024)}",
                )
            else:
                report.ok()


# --------------------------- 2. 设置项数 ---------------------------


def enum_field_count(header: Path, enum_name: str):
    """数出枚举里除末尾哨兵 Count 之外的条目数。"""
    text = header.read_text(encoding="utf-8")
    match = re.search(
        r"enum\s+class\s+%s\s*:\s*\w+\s*\{(.*?)\n\}" % re.escape(enum_name),
        text,
        re.S,
    )
    if not match:
        return None
    body = re.sub(r"//[^\n]*", "", match.group(1))
    names = [item.strip() for item in body.split(",") if item.strip()]
    return len(names) - 1 if names else None


def numbered_tables(text: str) -> list:
    """抽出文档里所有"编号表"的首列序号。

    只认表头第一格是 `#` 的表格 —— 这是本仓库设置表的固定写法。
    否则会把引脚表、参数表这类同样以数字开头的表格一起算进来,
    得到"44 行 vs 28 项"这种假报错(第一版就是这么错的)。
    """
    tables = []
    lines = text.splitlines()
    index = 0
    while index < len(lines):
        if not re.match(r"^\|\s*#\s*\|", lines[index]):
            index += 1
            continue
        cursor = index + 1
        while cursor < len(lines) and re.match(r"^\|[\s\-:|]+\|$", lines[cursor]):
            cursor += 1
        numbers = []
        while cursor < len(lines):
            match = re.match(r"^\|\s*(\d+)\s*\|", lines[cursor])
            if not match:
                break
            numbers.append(int(match.group(1)))
            cursor += 1
        if numbers:
            tables.append(numbers)
        index = cursor
    return tables


def check_settings_count(report: Report) -> None:
    count = enum_field_count(UI_MODEL_HEADER, "SystemSettingField")
    if count is None:
        report.error("无法从 include/ui_model.h 解析 SystemSettingField 枚举", "确认枚举写法未被改动")
        return

    expected = list(range(1, count + 1))
    for label, path in (("README", README), ("docs/settings.md", SETTINGS_DOC)):
        tables = numbered_tables(path.read_text(encoding="utf-8"))
        if any(table == expected for table in tables):
            report.ok()
            continue
        found = "、".join(f"{len(t)} 行" for t in tables) or "未找到编号表"
        report.error(
            f"{label} 的编号表与 SystemSettingField 不齐:期望连续 {count} 项,"
            f"实际{found}",
            f"补全成 1..{count} 连续序号",
        )

    # 各处"N 项"字样
    for path in (README, DOCS_DIR / "README.md", SETTINGS_DOC):
        if not path.exists():
            continue
        for claimed in set(re.findall(r"(\d+)\s*项", path.read_text(encoding="utf-8"))):
            if int(claimed) != count:
                report.error(
                    f"{path.name} 里写的「{claimed} 项」与枚举的 {count} 项不符",
                    f"改成「{count} 项」",
                )
            else:
                report.ok()


# --------------------------- 3. 固件台账一致性 ---------------------------


def check_firmware_ledger(report: Report) -> None:
    binary = FIRMWARE_DIR / "firmware.bin"
    sidecar = FIRMWARE_DIR / "firmware.bin.sha256"
    meta = read_manifest()

    if not binary.exists():
        report.error("firmware/firmware.bin 不存在", "pio run && python tools/publish_firmware.py")
        return
    if not meta:
        report.error("firmware/BUILD.txt 不存在", "python tools/publish_firmware.py")
        return

    actual_size = binary.stat().st_size
    actual_sha = sha256_of(binary)

    if str(actual_size) != meta.get("size"):
        report.error(
            f"firmware.bin 体积与台账不符:台账 {meta.get('size')},实际 {actual_size}",
            "重新发布:python tools/publish_firmware.py",
        )
    else:
        report.ok()

    if actual_sha != meta.get("sha256"):
        report.error(
            "firmware.bin 的 SHA-256 与台账不符 —— 镜像被替换过或台账未同步",
            "重新发布:python tools/publish_firmware.py",
        )
    else:
        report.ok()

    if sidecar.exists():
        declared = sidecar.read_text(encoding="utf-8").split()[0]
        if declared != actual_sha:
            report.error(
                "firmware.bin.sha256 与镜像不符",
                "重新发布:python tools/publish_firmware.py",
            )
        else:
            report.ok()
    else:
        report.error("firmware/firmware.bin.sha256 缺失", "重新发布:publish_firmware.py")


# --------------------------- 4. 镜像新鲜度 ---------------------------


def check_freshness(report: Report) -> None:
    meta = read_manifest()
    commit = meta.get("commit")
    if not commit or not git("rev-parse", "HEAD"):
        report.warn("无 git 环境,跳过镜像新鲜度检查")
        return

    if meta.get("source_dirty") == "true":
        report.warn(
            f"当前镜像是从「未提交的工作区」构建的(含 {meta.get('source_dirty_files')})",
            "想让它对应一个确定的提交,请先提交再跑 python tools/publish_firmware.py",
        )

    exists = subprocess.run(
        ["git", "cat-file", "-e", commit], cwd=PROJECT_DIR, capture_output=True
    ).returncode
    if exists != 0:
        # 浅克隆(默认 fetch-depth=1)里老提交查不到,这属于环境限制而非文档问题。
        report.warn(
            f"仓库中查不到台账记录的提交 {commit[:8]}(浅克隆?),跳过新鲜度比对",
            "CI 里把 actions/checkout 的 fetch-depth 设为 0",
        )
        return

    changed = git("diff", "--name-only", f"{commit}..HEAD", "--", "src", "include", "platformio.ini")
    if changed:
        files = ", ".join(changed.splitlines()[:5])
        report.warn(
            f"镜像落后于源码:台账记于 {meta.get('commit_short')},"
            f"其后 src/include/platformio.ini 有 {len(changed.splitlines())} 个文件改动({files})",
            "pio run && python tools/publish_firmware.py",
        )
    else:
        report.ok()


# --------------------------- 5. 字库覆盖 ---------------------------


def check_fonts(report: Report) -> None:
    """字库是否覆盖了源码里真正会绘制的字符。

    字库由 genvlw.py 扫源码字面量生成,"加中文文案忘了重新生成"没有机制守着 ——
    项目里已经发生过三次。漏字的现场表现是屏上一个空心方框 + 居中排版偏移,
    很难反推回字库,所以在 CI 里直接拦。

    实现放在 tools/font_check.py(可单独运行),这里只把结果并进同一份报告。
    """
    path = PROJECT_DIR / "tools" / "font_check.py"
    if not path.exists():
        report.warn("tools/font_check.py 缺失,跳过字库覆盖检查")
        return
    spec = importlib.util.spec_from_file_location("_font_check", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    module.run_check(report, quiet=True)


# --------------------------- 入口 ---------------------------


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "--strict", action="store_true", help="把告警也视为失败(便于卡紧 CI)"
    )
    args = parser.parse_args()

    report = Report()
    check_tree_numbers(report)
    check_settings_count(report)
    check_firmware_ledger(report)
    check_fonts(report)
    check_freshness(report)

    for text, fix in report.warnings:
        print(f"WARN  {text}")
        if fix:
            print(f"      → {fix}")
    for text, fix in report.errors:
        print(f"ERROR {text}")
        if fix:
            print(f"      → {fix}")

    print(
        f"\n文档一致性:{report.passed} 项通过,"
        f"{len(report.warnings)} 条告警,{len(report.errors)} 条错误"
    )
    if report.errors or (args.strict and report.warnings):
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
