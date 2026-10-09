#!/usr/bin/env python3
r"""校验字库(font_cn16.h / font_cn26.h)是否覆盖了固件真正会绘制的字符。

起因:字库是 genvlw.py 扫源码字符串字面量生成的,而"加中文文案忘了重新生成"
没有任何机制拦着 —— 项目里已经发生过三次(295 → 322 → 326 字形,每次都靠人记得)。
一旦漏掉,屏上的表现是:该字画成一个**空心方框**(TFT_eSPI 的 Smooth_font.cpp
对找不到的码位 drawRect 一个 spaceWidth 的框),而且 textWidth 只按 spaceWidth+1
计宽,居中文本会跟着偏移 —— 现场只会看到"排版错乱",很难想到是字库漏字。

    python tools/font_check.py            # 有 ERROR 时返回码 1
    python tools/font_check.py --quiet    # 只打错误与汇总

校验项:
  1. 头文件注释声明的字形数/数据字节与二进制本体一致(拦住手工编辑)
  2. 两个字号(16/26)的字形集完全一致,且版本/字号字段正确
  3. ASCII 0x20–0x7E 齐全(数字、单位、百分号缺一个就是一屏方框)
  4. 源码(src/ 与 include/,不含浏览器专用头)字符串字面量用到的非 ASCII 字符
     必须都在字库中
  5. 非空格字形都不许是空位图(在表里但画不出来,比缺字更隐蔽)
  6. 字库里未被源码引用的字形 → 告警,附占用的 Flash 字节数

只依赖标准库,不需要 PIL/TTF,可以在 CI 里跑(重新生成字库才需要 PIL)。
扫描器在这里是**独立实现**,刻意不复用 genvlw.py 的解析器:同一份逻辑自查等于
没查;本实现额外能看懂原始字符串 R"(...)" 与 \uXXXX/\xNN 转义(genvlw 看不懂,
所以那些写法会被这里报出来)。
"""

import argparse
import os
import struct
import sys

PROJECT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FONT_SIZES = (16, 26)
# 浏览器专用头:其中的中文(CSS/HTML/JS 文案)只由浏览器渲染,TFT 从不绘制,
# 与 genvlw.py 的 BROWSER_ONLY_HEADERS 保持一致。
BROWSER_ONLY_HEADERS = ("web_page.h", "wifi_portal_page.h")
SCAN_DIRS = ("src", "include")
SKIP_HEADERS = BROWSER_ONLY_HEADERS + ("font_cn16.h", "font_cn26.h")


# ------------------------------- 字库解析 -------------------------------


def parse_font(path):
    """解析 vlw 头,返回 (声明字形数, 声明数据字节, 版本, 字号, {码位: (宽, 高)})。"""
    with open(path, "r", encoding="utf-8") as handle:
        raw = handle.read()
    start = raw.index("{") + 1
    end = raw.index("};", start)
    tokens = [t.strip() for t in raw[start:end].replace("\n", "").split(",") if t.strip()]
    blob = bytes(int(t, 16) for t in tokens)

    count, version, size, _mbox, _ascent, _descent = struct.unpack_from(">6I", blob, 0)
    glyphs = {}
    for i in range(count):
        code, height, width = struct.unpack_from(">3i", blob, 24 + i * 28)
        glyphs[code] = (width, height)
    return count, len(blob), version, size, glyphs


def declared_counts(path):
    """读头文件注释 `字形数: N  数据: M 字节`。"""
    line = ""
    with open(path, "r", encoding="utf-8") as handle:
        for line in handle:
            if "字形数" in line:
                break
    digits = [int(tok) for tok in __import__("re").findall(r"\d+", line.split("字形数")[1])]
    return (digits[0], digits[1]) if len(digits) >= 2 else (None, None)


# ------------------------------- 源码扫描 -------------------------------


def scan_literals(text):
    """收集字符串/字符字面量中的非 ASCII 字符,并展开 \\uXXXX / \\xNN 转义。

    返回 (chars, 含转义的字面量数)。注释整体跳过 —— 注释里的汉字不参与绘制,
    genvlw.py 同样不计入,否则字库会被中文注释撑爆。
    """
    chars = set()
    escapes = 0
    i, n = 0, len(text)
    while i < n:
        ch = text[i]
        if ch == "/" and i + 1 < n and text[i + 1] == "/":
            end = text.find("\n", i)
            i = n if end < 0 else end + 1
            continue
        if ch == "/" and i + 1 < n and text[i + 1] == "*":
            end = text.find("*/", i + 2)
            i = n if end < 0 else end + 2
            continue
        if ch == "R" and text[i + 1 : i + 2] == '"':
            # 原始字符串 R"delim(...)delim"
            open_paren = text.find("(", i + 2)
            close = text.find(")" + text[i + 2 : open_paren] + '"', open_paren)
            body = text[open_paren + 1 : close if close >= 0 else n]
            chars |= {c for c in body if ord(c) > 0x7F}
            i = n if close < 0 else close + 1
            continue
        if ch in "\"'":
            quote = ch
            i += 1
            while i < n and text[i] != quote:
                if text[i] == "\\":
                    nxt = text[i + 1 : i + 2]
                    if nxt == "u" and len(text) > i + 5:
                        chars.add(chr(int(text[i + 2 : i + 6], 16)))
                        escapes += 1
                        i += 6
                        continue
                    if nxt == "x" and len(text) > i + 3:
                        chars.add(chr(int(text[i + 2 : i + 4], 16)))
                        escapes += 1
                        i += 4
                        continue
                    i += 2
                    continue
                if ord(text[i]) > 0x7F:
                    chars.add(text[i])
                i += 1
            i += 1
            continue
        i += 1
    return chars, escapes


def scan_sources():
    """返回 (需要的字符, {字符: [来源文件]})。"""
    used = set()
    origin = {}
    for folder in SCAN_DIRS:
        directory = os.path.join(PROJECT_DIR, folder)
        if not os.path.isdir(directory):
            continue
        for name in sorted(os.listdir(directory)):
            if not name.endswith((".c", ".cpp", ".h", ".hpp")) or name in SKIP_HEADERS:
                continue
            with open(os.path.join(directory, name), "r", encoding="utf-8") as handle:
                found, _ = scan_literals(handle.read())
            rel = f"{folder}/{name}"
            for ch in found:
                origin.setdefault(ch, []).append(rel)
            used |= found
    return used, origin


# ------------------------------- 校验 -------------------------------


def run_check(report, quiet=False):
    """把结果写进 report(需要 error/warn/ok 三个方法,与 doc_check.Report 同形)。"""
    fonts = {}
    for px in FONT_SIZES:
        path = os.path.join(PROJECT_DIR, "include", f"font_cn{px}.h")
        if not os.path.exists(path):
            report.error(f"include/font_cn{px}.h 不存在", "python tools/genvlw.py")
            return
        count, blob_len, version, size, glyphs = parse_font(path)
        declared_count, declared_bytes = declared_counts(path)
        fonts[px] = glyphs

        if declared_count != count or declared_bytes != blob_len:
            report.error(
                f"font_cn{px}.h 头部注释与实际数据不符:注释写 {declared_count} 字形/"
                f"{declared_bytes} 字节,实际 {count}/{blob_len}",
                "重新生成:python tools/genvlw.py",
            )
        else:
            report.ok()

        if version != 0x0B or size != px:
            report.error(
                f"font_cn{px}.h 的头字段异常:version=0x{version:02X} fontSize={size}",
                f"期望 version=0x0B、fontSize={px};重新生成字库",
            )
        else:
            report.ok()

        blank = sorted(
            c for c, (w, h) in glyphs.items()
            if (w == 0 or h == 0) and not chr(c).isspace()
        )
        if blank:
            shown = " ".join(f"U+{c:04X}{chr(c)}" for c in blank[:10])
            report.error(
                f"font_cn{px}.h 有 {len(blank)} 个非空格字形位图为空(屏上是空白):{shown}",
                "该字在所选 TTF 里没有字形,换字体或改写文案",
            )
        else:
            report.ok()

        missing_ascii = [c for c in range(0x20, 0x7F) if c not in glyphs]
        if missing_ascii:
            report.error(
                f"font_cn{px}.h 缺少 {len(missing_ascii)} 个 ASCII 可打印字符"
                f"({''.join(chr(c) for c in missing_ascii)})",
                "genvlw.py 的 build_charset 里补上 0x20-0x7E",
            )
        else:
            report.ok()

    # 只比码位集合:两个字号的位图尺寸本就不同,拿整张 glyphs 比会永远不相等。
    if 16 in fonts and 26 in fonts and set(fonts[16]) != set(fonts[26]):
        only16 = sorted(set(fonts[16]) - set(fonts[26]))
        only26 = sorted(set(fonts[26]) - set(fonts[16]))
        report.error(
            f"两个字号的字形集不一致:仅 16px 有 {len(only16)} 个,仅 26px 有 {len(only26)} 个",
            "两个字号必须用同一份字符集,重新生成:python tools/genvlw.py",
        )
    elif fonts:
        report.ok()

    used, origin = scan_sources()
    reference = fonts[min(fonts)] if fonts else {}
    missing = sorted(used - {chr(c) for c in reference})
    if missing:
        detail = "; ".join(
            f"{ch}(U+{ord(ch):04X}) ← {', '.join(origin.get(ch, [])[:2])}" for ch in missing[:12]
        )
        report.error(
            f"字库缺少 {len(missing)} 个源码用到的字符:{detail}",
            "重新生成字库:python tools/genvlw.py,并同步 README 里的体积标注",
        )
    else:
        report.ok()

    if reference:
        unused = sorted(
            chr(c) for c in reference if c > 0x7F and chr(c) not in used
        )
        if unused:
            waste = 0
            for px, screen in fonts.items():
                for c in screen:
                    if c > 0x7F and chr(c) in unused:
                        w, h = screen[c]
                        waste += 28 + w * h
            if not quiet:
                report.warn(
                    f"字库中 {len(unused)} 个字形未被源码引用(兜底字表 BASE_CJK 里过期的字),"
                    f"两个字号共占 {waste:,} 字节:"
                    + "".join(unused),
                    "确认无用后从 tools/genvlw.py 的 BASE_CJK 删掉再重新生成",
                )


class _Report:
    """独立运行时的最小 report 实现,与 doc_check.Report 接口一致。"""

    def __init__(self):
        self.errors = []
        self.warnings = []
        self.passed = 0

    def error(self, text, fix=""):
        self.errors.append((text, fix))

    def warn(self, text, fix=""):
        self.warnings.append((text, fix))

    def ok(self):
        self.passed += 1


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--quiet", action="store_true", help="不打告警明细")
    args = parser.parse_args()

    report = _Report()
    run_check(report, quiet=args.quiet)

    for text, fix in report.warnings:
        print(f"WARN  {text}")
        if fix:
            print(f"      → {fix}")
    for text, fix in report.errors:
        print(f"ERROR {text}")
        if fix:
            print(f"      → {fix}")
    print(
        f"\n字库覆盖:{report.passed} 项通过,"
        f"{len(report.warnings)} 条告警,{len(report.errors)} 条错误"
    )
    return 1 if report.errors else 0


if __name__ == "__main__":
    sys.exit(main())
