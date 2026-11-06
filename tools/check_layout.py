#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
check_layout.py — 离线校验生成的方舟字体(LVGL fmt_txt) + 界面排版

1) 解析 ark12_font.c：glyph_bitmap / glyph_dsc / unicode_list / cmap
2) 扫 main.c 里全部字符串字面量，按 advance 累加宽度，超屏宽报警
3) 把界面真字符串渲染成 ASCII 点阵，肉眼核对字形与位置

坐标公式照 LVGL 源码：
    letter_coords.x1 = pos.x + ofs_x
    letter_coords.y1 = pos.y + (line_height - base_line) - box_h - ofs_y
位图（bpp=1, stride=0）：字形内部行间不补齐、比特流跨行连续，MSB 在前。

用法:
  python check_layout.py --font ark12_font.c --src main.c [--width 148] [--art "字符串"]
"""

import argparse
import re
import sys

FMT_FILL = {"d": "88", "i": "88", "u": "88", "x": "FF", "X": "FF",
            "c": "W", "s": "XXXXXXXX", "%": "%"}


def parse_font(path):
    src = open(path, encoding="utf-8").read()

    blob = bytearray()
    blk = src.split("glyph_bitmap[] = {", 1)[1].split("};", 1)[0]
    for tok in re.findall(r"0x([0-9a-fA-F]{2})", blk):
        blob.append(int(tok, 16))

    dsces = []
    tbl = src.split("glyph_dsc[] = {", 1)[1].split("};", 1)[0]
    for m in re.finditer(
            r"\.bitmap_index\s*=\s*(\d+),\s*\.adv_w\s*=\s*(\d+),\s*\.box_w\s*=\s*(\d+),\s*"
            r"\.box_h\s*=\s*(\d+),\s*\.ofs_x\s*=\s*(-?\d+),\s*\.ofs_y\s*=\s*(-?\d+)", tbl):
        dsces.append(dict(off=int(m.group(1)), adv=int(m.group(2)), w=int(m.group(3)),
                          h=int(m.group(4)), ox=int(m.group(5)), oy=int(m.group(6))))

    ulist = []
    iblk = src.split("unicode_list[] = {", 1)[1].split("};", 1)[0]
    for tok in re.findall(r"0x([0-9a-fA-F]{4})", iblk):
        ulist.append(int(tok, 16))

    range_start = int(re.search(r"\.range_start = (\d+)", src).group(1))
    line_height = int(re.search(r"\.line_height = (\d+)", src).group(1))
    base_line = int(re.search(r"\.base_line = (\d+)", src).group(1))

    glyphs = {}
    for i, rcpoff in enumerate(ulist):
        cp = range_start + rcpoff
        g = dict(dsces[i + 1])          # id 0 是 reserved
        g["bits"] = blob
        glyphs[cp] = g
    return glyphs, blob, line_height, base_line


def text_width(glyphs, text):
    tot = 0
    for ch in text:
        g = glyphs.get(ord(ch)) or glyphs.get(ord("?"))
        if g:
            tot += g["adv"]
    return tot / 16.0


def render(glyphs, text, line_height, base_line):
    w = int(text_width(glyphs, text)) + 2
    grid = [[0] * w for _ in range(line_height)]
    pen16 = 0
    baseline = line_height - base_line
    for ch in text:
        g = glyphs.get(ord(ch)) or glyphs.get(ord("?"))
        if not g:
            continue
        pen_col = pen16 >> 4
        if g["w"] and g["h"]:
            y1 = baseline - g["h"] - g["oy"]
            for row in range(g["h"]):
                for col in range(g["w"]):
                    k = row * g["w"] + col
                    byte = g["bits"][g["off"] + (k >> 3)]
                    if byte & (0x80 >> (k & 7)):
                        px, py = pen_col + g["ox"] + col, y1 + row
                        if 0 <= px < w and 0 <= py < line_height:
                            grid[py][px] = 1
        pen16 += g["adv"]
    return ["".join("#" if v else "." for v in r) for r in grid]


def strip_noise(text):
    out = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            while i < n and text[i] != "\n":
                i += 1
        elif c == "/" and i + 1 < n and text[i + 1] == "*":
            i += 2
            while i + 1 < n and not (text[i] == "*" and text[i + 1] == "/"):
                i += 1
            i += 2
        elif c == "'":
            i += 1
            while i < n and text[i] != "'":
                i += 2 if text[i] == "\\" else 1
            i += 1
            out.append('"@C@"')
        else:
            out.append(c)
            i += 1
    return "".join(out)


def scan_literals(src_path):
    text = strip_noise(open(src_path, encoding="utf-8").read())
    out = []
    for lit in re.findall(r'"((?:[^"\\]|\\.)*)"', text):
        if not lit:
            continue
        expanded = re.sub(r"%[-0-9.]*([a-zA-Z%])",
                          lambda m: FMT_FILL.get(m.group(1), "??"), lit)
        expanded = expanded.replace("\\n", "")
        out.append((lit, expanded))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--font", required=True)
    ap.add_argument("--src", required=True)
    ap.add_argument("--width", type=int, default=148)
    ap.add_argument("--art", action="append", default=[])
    args = ap.parse_args()

    glyphs, blob, lh, bl = parse_font(args.font)
    cjk = sum(1 for cp in glyphs if cp > 0x2E7F)
    print("font: %d glyphs (%d cjk), %d bytes bitmap, line_height=%d base_line=%d"
          % (len(glyphs), cjk, len(blob), lh, bl))

    over, checked, missing = 0, 0, set()
    for raw, expanded in scan_literals(args.src):
        if expanded.isascii() and len(expanded) > 24:
            continue
        for ch in expanded:
            if ord(ch) > 0x7F and ord(ch) not in glyphs:
                missing.add(ch)
        w = text_width(glyphs, expanded)
        checked += 1
        if w > args.width:
            over += 1
            print("OVERFLOW %6.1fpx > %dpx : %r" % (w, args.width, raw))
    print("--- 宽度检查 (上限 %d px / 12px 字): 检查 %d 条, 超宽 %d 条 ---"
          % (args.width, checked, over))
    if missing:
        print("!! 缺字（会退化成 ?）: %s" % " ".join(sorted(missing)))
    else:
        print("字符覆盖检查: main.c 里所有非 ASCII 字都在字体里 OK")

    for a in (args.art or ["查询 12WPM", "长按A调速"]):
        print("\n=== 点阵核对 %r (宽 %.1f px) ===" % (a, text_width(glyphs, a)))
        for row in render(glyphs, a, lh, bl):
            print("   " + row)
    return 0


if __name__ == "__main__":
    sys.exit(main())
