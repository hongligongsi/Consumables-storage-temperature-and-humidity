#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""从系统 TTF 生成 TFT_eSPI 平滑字体(vlw)格式的 C 头文件。

TFT_eSPI 的 vlw 格式没有官方文档,格式来自 Extensions/Smooth_font.cpp 的逆向注释:

  头部 24 字节 = 6 个 uint32(大端):
    1. gCount      字形数量
    2. version     0x0B
    3. fontSize    字号(磅)
    4. mboxY       已废弃,填 0
    5. ascent      基线到 "d" 顶部的像素数
    6. descent     基线到 "p" 底部的像素数

  随后 gCount 组,每组 7 个 int32(大端,共 28 字节):
    1. unicode     码位
    2. height      位图高
    3. width       位图宽
    4. gxAdvance   下一个字形的光标推进量
    5. dY          基线到字形位图顶部的距离(向上为正)
    6. dX          光标到字形位图左边的距离(向左为负)
    7. 占位 0

  位图区:每像素 1 字节,8 位 alpha,行优先(y 外层、x 内层)。

  尾部:字体名长度+名字、PostScript 名长度+名字、抗锯齿标志(1)。
  库里不读尾部,但为与官方生成器保持一致仍然写出。

用法:
    python tools/genvlw.py            # 生成 include/font_cn16.h、include/font_cn26.h
    python tools/genvlw.py --preview  # 额外输出 tools/preview_cn.png 供人眼核对
"""

from __future__ import annotations

import argparse
import os
import shutil
import struct
import subprocess
import sys

from PIL import Image, ImageDraw, ImageFont

# ---------------------------------------------------------------------------
# 字体来源与字号
# ---------------------------------------------------------------------------
TTF_CANDIDATES = [
    r"C:\Windows\Fonts\simhei.ttf",
    r"C:\Windows\Fonts\Deng.ttf",
    r"C:\Windows\Fonts\msyh.ttc",
    "/System/Library/Fonts/STHeiti Light.ttc",
    "/System/Library/Fonts/Hiragino Sans GB.ttc",
    "/Library/Fonts/Arial Unicode.ttf",
    "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
    "/usr/share/fonts/truetype/wqy/wqy-microhei.ttc",
]

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
INCLUDE_DIR = os.path.join(REPO_ROOT, "include")
PREVIEW_PATH = os.path.join(REPO_ROOT, "tools", "preview_cn.png")

# 需要生成的字号(像素)
SIZES = [
    (16, "FontCN16"),
    (26, "FontCN26"),
]

# ASCII 可打印区 + 常用符号。0xB0 是 °,0x2103 是 ℃。
EXTRA_CHARS = "\u00b0\u2103"

# 兜底汉字:即使某个字暂时没写进源码也不至于变成空白框。
BASE_CJK = (
    "智能耗材仓排风自动温度打印后主板湿床电压流最低高速时间待机预照明设置开关"
    "就绪检测中气故障语言状态文英网络已连接未确定取消返回启用禁警告正常请稍候"
    "版本系统信号强弱手动停止错误剩余更换滤芯净化容量过载秒分小天上限传感器环境目标前"
)

SRC_DIRS = ("src", "include")


def collect_literal_chars(text: str) -> set[str]:
    """只取字符串字面量里的非 ASCII 字符。

    注释里的汉字不能算数,否则中文注释会把字库撑到几百 KB。
    处理不了原始字符串与续行,对本工程足够。
    """
    found: set[str] = set()
    i = 0
    n = len(text)
    while i < n:
        ch = text[i]
        # 行注释
        if ch == "/" and i + 1 < n and text[i + 1] == "/":
            i = text.find("\n", i)
            if i < 0:
                break
            continue
        # 块注释
        if ch == "/" and i + 1 < n and text[i + 1] == "*":
            end = text.find("*/", i + 2)
            i = n if end < 0 else end + 2
            continue
        # 字符串 / 字符字面量
        if ch in ('"', "'"):
            quote = ch
            i += 1
            while i < n:
                cur = text[i]
                if cur == "\\":
                    i += 2
                    continue
                if cur == quote:
                    i += 1
                    break
                if ord(cur) > 0x7F:
                    found.add(cur)
                if cur == "\n":  # 字面量未闭合,提前收尾
                    break
                i += 1
            continue
        i += 1
    return found


def collect_source_chars() -> set[str]:
    """扫描 src/ 与 include/ 下的源码,收集字符串字面量中的汉字。"""
    found: set[str] = set()
    for folder in SRC_DIRS:
        root_dir = os.path.join(REPO_ROOT, folder)
        if not os.path.isdir(root_dir):
            continue
        for name in sorted(os.listdir(root_dir)):
            if not name.endswith((".c", ".cpp", ".h", ".hpp")):
                continue
            with open(os.path.join(root_dir, name), "r", encoding="utf-8") as handle:
                found |= collect_literal_chars(handle.read())
    return found


def build_charset() -> list[str]:
    chars = set(chr(c) for c in range(0x20, 0x7F)) | set(BASE_CJK + EXTRA_CHARS)
    source_chars = collect_source_chars()
    missing = source_chars - set(BASE_CJK + EXTRA_CHARS)
    chars |= source_chars
    if missing:
        print(f"从源码补充 {len(missing)} 个汉字: {''.join(sorted(missing))}")
    return sorted(chars, key=ord)


def pick_ttf(requested: str | None = None) -> str:
    if requested:
        path = os.path.abspath(os.path.expanduser(requested))
        if not os.path.isfile(path):
            raise SystemExit(f"字体文件不存在: {path}")
        return path
    for path in TTF_CANDIDATES:
        if os.path.exists(path):
            return path
    # Linux 发行版通常提供 fontconfig；让它挑一个确实覆盖中文的字体。
    if shutil.which("fc-match"):
        result = subprocess.run(
            ["fc-match", "-f", "%{file}", ":lang=zh-cn"],
            check=False, capture_output=True, text=True,
        )
        path = result.stdout.strip().splitlines()[0] if result.stdout.strip() else ""
        if os.path.isfile(path):
            return path
    raise SystemExit("找不到可用的中文字体；请用 --font 指定 TTF/TTC 文件")


def render_glyph(font: ImageFont.FreeTypeFont, ch: str) -> tuple[int, int, int, int, int, bytes]:
    """返回 (width, height, gxAdvance, dY, dX, bitmap)。

    dY 是基线到字形位图顶部的距离(向上为正),dX 是光标到字形位图左边的距离。
    这两个字段的语义与 TFT_eSPI 的 drawGlyph() 严格对应:

        cy = cursor_y + maxAscent - dY
        cx = cursor_x + dX
    """
    advance = int(round(font.getlength(ch)))

    # 空白字符没有位图,只占位。
    if ch.isspace():
        return 0, 0, advance, 0, 0, b""

    bbox = font.getbbox(ch, anchor="ls")  # 'l'=左边 's'=基线
    left, top, right, bottom = (int(round(v)) for v in bbox)
    width = right - left
    height = bottom - top
    if width <= 0 or height <= 0:
        return 0, 0, advance, 0, 0, b""

    # 用一个刚好装下字形的画布渲染,坐标系原点平移到 bbox 左上角。
    img = Image.new("L", (width, height), 0)
    ImageDraw.Draw(img).text((-left, -top), ch, font=font, fill=255, anchor="ls")

    d_y = -top          # 向上为正
    d_x = left          # 向左为负
    return width, height, advance, d_y, d_x, img.tobytes()


def build_vlw(font_path: str, px: int, chars: list[str], name: str) -> bytes:
    font = ImageFont.truetype(font_path, px)

    glyphs = []
    for ch in chars:
        width, height, advance, d_y, d_x, bitmap = render_glyph(font, ch)
        glyphs.append((ord(ch), width, height, advance, d_y, d_x, bitmap))

    # 行高取所有字形的实际包围盒,避免 loadMetrics() 事后又去放大 maxDescent
    # 而让 yAdvance 在运行期变化(行距跳变)。
    ascent = max((g[4] for g in glyphs if g[2] > 0), default=font.getmetrics()[0])
    descent = max((g[2] - g[4] for g in glyphs if g[2] > 0), default=font.getmetrics()[1])
    ascent = max(ascent, 1)
    descent = max(descent, 1)

    out = bytearray()
    out += struct.pack(">6I", len(glyphs), 0x0B, px, 0, ascent, descent)
    for unicode_value, width, height, advance, d_y, d_x, _ in glyphs:
        assert 0 < unicode_value <= 0xFFFF, "库只支持 BMP 平面"
        assert 0 <= width <= 255 and 0 <= height <= 255, "位图尺寸必须是 uint8"
        assert 0 <= advance <= 255, "gxAdvance 在 loadMetrics() 中被截断成 uint8"
        out += struct.pack(">7i", unicode_value, height, width, advance, d_y, d_x, 0)
    for _, _, _, _, _, _, bitmap in glyphs:
        out += bitmap

    # 尾部(库不读取,只为与官方生成器格式一致)
    out += bytes([len(name)]) + name.encode("ascii")
    out += bytes([len(name)]) + name.encode("ascii")
    out += bytes([1])  # antialiased

    return bytes(out)


def write_header(path: str, var_name: str, blob: bytes, px: int, glyph_count: int) -> None:
    lines = [
        "// 由 tools/genvlw.py 生成,请勿手工编辑。",
        f"// 字体: {os.path.basename(TTF_PATH)}  {px}px  字形数: {glyph_count}  数据: {len(blob)} 字节",
        "// 格式: TFT_eSPI 平滑字体(vlw),用 tft.loadFont(FontCXXX) 加载。",
        "#pragma once",
        "#include <pgmspace.h>",
        "",
        f"const uint8_t {var_name}[] PROGMEM = {{",
    ]

    row = []
    for i, byte in enumerate(blob):
        row.append(f"0x{byte:02X},")
        if len(row) == 16:
            lines.append("  " + "".join(row))
            row = []
    if row:
        lines.append("  " + "".join(row))
    lines.append("};")
    lines.append("")

    with open(path, "w", encoding="utf-8", newline="\n") as handle:
        handle.write("\n".join(lines))


# ---------------------------------------------------------------------------
# 回读校验:把生成的 blob 按 Smooth_font.cpp 的解析逻辑重新解一遍并合成图片,
# 任何字段顺序/字节序写错都会在这里暴露成乱码。
# ---------------------------------------------------------------------------
def preview(blobs: dict[str, tuple[bytes, int, str]], sample: str) -> None:
    slices = []
    for var_name, (blob, px, _) in blobs.items():
        count, _version, _size, _mbox, ascent, _descent = struct.unpack_from(">6I", blob, 0)
        base = 24 + count * 28
        index = {}
        cursor = base
        for i in range(count):
            unicode_value, height, width, advance, d_y, d_x, _pad = struct.unpack_from(">7i", blob, 24 + i * 28)
            # 位图偏移按 loadMetrics() 的累加方式计算
            index[unicode_value] = (width, height, advance, d_y, d_x, cursor)
            cursor += width * height

        # 逐字符测量需要的宽度
        total = sum(index[ord(c)][2] for c in sample if ord(c) in index)
        canvas = Image.new("L", (max(total, 1), ascent + 4), 0)

        x = 0
        for c in sample:
            if ord(c) not in index:
                continue
            width, height, advance, d_y, d_x, start = index[ord(c)]
            if width and height:
                data = blob[start:start + width * height]
                glyph = Image.frombytes("L", (width, height), data)
                canvas.paste(glyph, (x + d_x, ascent - d_y), glyph)
            x += advance

        slices.append((px, canvas))

    width = max(c.width for _, c in slices)
    height = sum(c.height + 6 for _, c in slices)
    sheet = Image.new("L", (width, height), 0)
    y = 0
    for _, c in slices:
        sheet.paste(c, (0, y))
        y += c.height + 6

    sheet.save(PREVIEW_PATH)
    print(f"预览已写出: {PREVIEW_PATH}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--preview", action="store_true", help="同时输出 PNG 预览做回读校验")
    parser.add_argument("--font", help="指定系统 TTF/TTC；省略则自动搜索 Windows/macOS/Linux 字体")
    args = parser.parse_args()

    global TTF_PATH
    TTF_PATH = pick_ttf(args.font)
    print(f"字体: {TTF_PATH}")

    charset = build_charset()
    print(f"字符集: {len(charset)} 个(ASCII {sum(1 for c in charset if ord(c) < 0x80)} + 非 ASCII)")

    blobs = {}
    for px, var_name in SIZES:
        blob = build_vlw(TTF_PATH, px, charset, var_name)
        out_path = os.path.join(INCLUDE_DIR, f"font_cn{px}.h")
        write_header(out_path, var_name, blob, px, len(charset))
        blobs[var_name] = (blob, px, out_path)
        print(f"  {px:>2}px -> {os.path.relpath(out_path, REPO_ROOT)}  {len(blob):,} 字节")

    if args.preview:
        preview(blobs, "智能耗材仓 排风自动 主板温度 预热中 已连接 45°C")

    return 0


if __name__ == "__main__":
    sys.exit(main())
