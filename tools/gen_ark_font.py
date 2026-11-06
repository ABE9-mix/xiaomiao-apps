#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
gen_ark_font.py — 把方舟像素字体(Ark Pixel 12px)转成 LVGL 9 的「原生 fmt_txt 字体」

为什么不用自定义回调：LVGL 9.5 里自己实现 get_glyph_bitmap 很难踩对契约
（静态位图快路径只认 A8、mask stride 用 letter_coords 宽等等），实测会崩/糊。
这里改成输出 LVGL 官方 fmt_txt 表（Montserrat 等自带字体就是这个格式）：
  * bpp = 1（1bit/像素，最省 flash）
  * 绘制时由 lv_font_get_bitmap_fmt_txt() 自己把 1bpp 展开成 A8，跨步/边界它保证
  * get_glyph_dsc / get_glyph_bitmap 直接指向 LVGL 现成函数，零自定义代码

关键坐标约定（照 LVGL 源码反推，踩过坑）：
  letter_coords.y1 = pos.y + (line_height - base_line) - box_h - ofs_y
  => ofs_y = ascent - y0 - box_h          （字形底边距基线的像素数，正=在基线之上）
  letter_coords.x1 = pos.x + ofs_x        （ofs_x = 字形在画布里的左边界）

位图排布（bpp=1、font_dsc.stride=0 时 LVGL 的读法）：
  每个字形内部**行间不补齐**，比特流跨行连续；字形之间按字节对齐。
  bitmap_index 是字节偏移。

用法:
  python gen_ark_font.py --ttf <ttf> --src <main.c> --out-c ark12_font.c --out-h ark12_font.h
"""

import argparse
import os
import re
import sys

from PIL import Image, ImageDraw, ImageFont

CJK_RANGES = [
    (0x2E80, 0x2EFF), (0x3000, 0x303F), (0x3100, 0x312F), (0x31C0, 0x31EF),
    (0x3200, 0x32FF), (0x3400, 0x4DBF), (0x4E00, 0x9FFF), (0xF900, 0xFAFF),
    (0xFE30, 0xFE4F), (0xFF00, 0xFFEF),
]


def is_cjk(cp):
    return any(lo <= cp <= hi for lo, hi in CJK_RANGES)


def strip_noise(text):
    """剥掉注释和字符字面量。
    必须做：Morse 表里有 {'"', ".-..-."} 这种字符字面量，里面的 " 会让朴素的
    字符串配对从第一个引号一路串到很后面，字集就全错了（踩过）。"""
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
        else:
            out.append(c)
            i += 1
    return "".join(out)


def collect_charset(srcs, extra):
    cps = set(range(0x20, 0x7F))
    for s in srcs:
        try:
            text = strip_noise(open(s, encoding="utf-8").read())
        except Exception as exc:  # noqa: BLE001
            print("WARN: 读不到 %s (%s)" % (s, exc))
            continue
        for lit in re.findall(r'"((?:[^"\\]|\\.)*)"', text):
            for ch in lit:
                if ord(ch) > 0x7F:
                    cps.add(ord(ch))
    for ch in extra or "":
        cps.add(ord(ch))
    return sorted(cps)


def render_glyph(font, ascent, ch):
    """渲染单字 -> (adv_w, w, h, ox, oy, bit_stream_bytes)
    bit_stream: 行间不补齐、跨行连续的 1bpp 比特流（字形末尾按字节对齐由调用方处理）"""
    canvas = 40
    img = Image.new("L", (canvas, canvas), 0)
    ImageDraw.Draw(img).text((0, 0), ch, font=font, fill=255)
    px = img.load()

    xs = [x for x in range(canvas) for y in range(canvas) if px[x, y] > 127]
    ys = [y for y in range(canvas) for x in range(canvas) if px[x, y] > 127]
    adv_w = max(0, int(round(font.getlength(ch) * 16)))

    if not xs:
        return adv_w, 0, 0, 0, 0, b""

    x0, x1 = min(xs), max(xs)
    y0, y1 = min(ys), max(ys)
    w, h = x1 - x0 + 1, y1 - y0 + 1

    bits = []
    for yy in range(h):
        for xx in range(w):
            bits.append(1 if px[x0 + xx, y0 + yy] > 127 else 0)

    data = bytearray((len(bits) + 7) // 8)
    for i, b in enumerate(bits):
        if b:
            data[i >> 3] |= 0x80 >> (i & 7)

    ofs_x = x0
    ofs_y = ascent - y0 - h          # 见文件头推导
    return adv_w, w, h, ofs_x, ofs_y, bytes(data)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ttf", required=True)
    ap.add_argument("--src", action="append", default=[])
    ap.add_argument("--out-c", required=True)
    ap.add_argument("--out-h", required=True)
    ap.add_argument("--extra", default="")
    ap.add_argument("--size", type=int, default=12)
    ap.add_argument("--name", default="ark12")
    args = ap.parse_args()

    font = ImageFont.truetype(args.ttf, args.size)
    ascent, descent = font.getmetrics()
    line_height, base_line = ascent + descent, descent
    print("font: %s" % os.path.basename(args.ttf))
    print("size=%d ascent=%d descent=%d line_height=%d base_line=%d"
          % (args.size, ascent, descent, line_height, base_line))

    cps = collect_charset(args.src, args.extra)
    cjk = [c for c in cps if is_cjk(c)]
    print("charset: %d total (%d cjk)" % (len(cps), len(cjk)))

    glyphs, tofu, blob = [], [], bytearray()
    for cp in cps:
        adv_w, w, h, ox, oy, data = render_glyph(font, ascent, chr(cp))
        if w == 0 and h == 0 and cp != 0x20:
            tofu.append(chr(cp))
        idx = len(blob)
        blob += data
        glyphs.append((cp, idx, adv_w, w, h, ox, oy))

    if tofu:
        print("!! TOFU (字体里取不到点阵): %s" % " ".join(tofu))
    else:
        print("tofu check: 全部字形都有点阵 OK")

    # 单张 sparse cmap：range_start = 最小码位，unicode_list = 相对偏移
    cps_sorted = [g[0] for g in glyphs]
    range_start = cps_sorted[0]
    span = cps_sorted[-1] - range_start + 1
    if span > 0xFFFF:
        print("ERR: 码位跨度 %d 超出 sparse cmap 的 uint16 表示范围，需要拆分 cmap" % span)
        return 2

    name = args.name
    guard = re.sub(r"[^A-Za-z0-9]", "_", os.path.splitext(os.path.basename(args.out_c))[0]).upper() + "_H"

    with open(args.out_c, "w", encoding="utf-8") as f:
        f.write("/* 由 tools/gen_ark_font.py 自动生成，不要手改。\n")
        f.write(" * 字体: Ark Pixel %dpx (%s)\n" % (args.size, os.path.basename(args.ttf)))
        f.write(" * 格式: LVGL 原生 fmt_txt / bpp=1 / 字形 %d 个 (CJK %d) / 位图 %d 字节\n"
                % (len(glyphs), len(cjk), len(blob)))
        f.write(" * 走 lv_font_get_glyph_dsc_fmt_txt + lv_font_get_bitmap_fmt_txt（与自带字体同一条路） */\n\n")
        f.write('#include "lvgl.h"\n')
        f.write('#include "%s"\n\n' % os.path.basename(args.out_h))

        f.write("/*-----------------\n *    BITMAPS (1bpp, 行间不补齐, 跨行连续)\n *----------------*/\n")
        f.write("static const uint8_t glyph_bitmap[] = {\n")
        for i in range(0, len(blob), 16):
            f.write("    " + ", ".join("0x%02x" % b for b in blob[i:i + 16]) + ",\n")
        f.write("};\n\n")

        f.write("/*-----------------\n *    GLYPH DESCRIPTION\n *----------------*/\n")
        f.write("static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {\n")
        f.write("    {.bitmap_index = 0, .adv_w = 0, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0},"
                " /* id = 0 reserved */\n")
        for (cp, idx, adv_w, w, h, ox, oy) in glyphs:
            ch = chr(cp) if cp >= 0x20 else "?"
            disp = ch if 0x20 <= cp < 0x7F else "U+%04X" % cp
            f.write("    {.bitmap_index = %5d, .adv_w = %4d, .box_w = %2d, .box_h = %2d, "
                    ".ofs_x = %3d, .ofs_y = %3d}, /* %s */\n"
                    % (idx, adv_w, w, h, ox, oy, disp))
        f.write("};\n\n")

        f.write("/*-----------------\n *    UNICODE -> GLYPH ID\n *----------------*/\n")
        f.write("static const uint16_t unicode_list[] = {\n")
        rcp = [cp - range_start for cp in cps_sorted]
        for i in range(0, len(rcp), 10):
            f.write("    " + ", ".join("0x%04x" % v for v in rcp[i:i + 10]) + ",\n")
        f.write("};\n\n")

        f.write("static const lv_font_fmt_txt_cmap_t cmaps[] = {\n")
        f.write("    {\n")
        f.write("        .range_start = %d, .range_length = %d, .glyph_id_start = 1,\n"
                % (range_start, span))
        f.write("        .unicode_list = unicode_list, .glyph_id_ofs_list = NULL,\n")
        f.write("        .list_length = %d, .type = LV_FONT_FMT_TXT_CMAP_SPARSE_TINY\n" % len(rcp))
        f.write("    }\n};\n\n")

        f.write("static const lv_font_fmt_txt_dsc_t font_dsc = {\n")
        f.write("    .glyph_bitmap = glyph_bitmap,\n")
        f.write("    .glyph_dsc = glyph_dsc,\n")
        f.write("    .cmaps = cmaps,\n")
        f.write("    .kern_dsc = NULL,\n")
        f.write("    .kern_scale = 0,\n")
        f.write("    .cmap_num = 1,\n")
        f.write("    .bpp = 1,\n")
        f.write("    .kern_classes = 0,\n")
        f.write("    .bitmap_format = 0,   /* LV_FONT_FMT_TXT_PLAIN */\n")
        f.write("    .stride = 0,          /* 0 = 行间不补齐 */\n")
        f.write("};\n\n")

        f.write("const lv_font_t %s = {\n" % name)
        f.write("    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,\n")
        f.write("    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,\n")
        f.write("    .line_height = %d,\n" % line_height)
        f.write("    .base_line = %d,\n" % base_line)
        f.write("    .subpx = LV_FONT_SUBPX_NONE,\n")
        f.write("    .kerning = LV_FONT_KERNING_NONE,\n")
        f.write("    .static_bitmap = 0,   /* 交给 LVGL 把 1bpp 展开成 A8 */\n")
        f.write("    .underline_position = -2,\n")
        f.write("    .underline_thickness = 1,\n")
        f.write("    .dsc = &font_dsc,\n")
        f.write("};\n")

    with open(args.out_h, "w", encoding="utf-8") as f:
        f.write("/* 由 tools/gen_ark_font.py 自动生成，不要手改。 */\n")
        f.write("#ifndef %s\n#define %s\n\n" % (guard, guard))
        f.write('#include "lvgl.h"\n\n')
        f.write("LV_FONT_DECLARE(%s);\n\n" % name)
        f.write("#endif /* %s */\n" % guard)

    print("written: %s (%d glyphs, %d bytes bitmap, bpp=1)"
          % (args.out_c, len(glyphs), len(blob)))
    print("advance: 'A'=%d/16px  '长'=%d/16px"
          % (next(g[2] for g in glyphs if g[0] == ord("A")),
             next((g[2] for g in glyphs if g[0] == ord("长")), -1)))
    print("ofs_y 示例: 'A'=%d  '长'=%d  '.'=%d"
          % (next(g[6] for g in glyphs if g[0] == ord("A")),
             next((g[6] for g in glyphs if g[0] == ord("长")), -99),
             next(g[6] for g in glyphs if g[0] == ord("."))))
    return 0


if __name__ == "__main__":
    sys.exit(main())
