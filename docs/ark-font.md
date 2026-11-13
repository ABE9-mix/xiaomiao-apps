# 方舟像素字体流水线

屏幕只有 160×128，用**方舟像素字体（Ark Pixel）**最合适。本仓库不依赖 `lv_font_conv`，
用两个脚本自己生成 LVGL 原生字体，并做离线校验。

## 生成

```powershell
# 12px 界面字：字集自动从源码字符串里扫（ASCII + 用到的汉字）
py -3.10 tools/gen_ark_font.py --ttf <ArkPixel12MonoZHHans.ttf> `
    --src apps/morse-trainer/main/main.c `
    --out-c apps/morse-trainer/main/ark12_font.c `
    --out-h apps/morse-trainer/main/ark12_font.h --size 12

# 24px 主显字：用 12px 的轮廓版按 2× 渲染，出来是完美 2 倍像素（灰度只有 0/255）
py -3.10 tools/gen_ark_font.py --ttf <ArkPixel12MonoZHHans.ttf> `
    --out-c apps/morse-trainer/main/ark24_font.c `
    --out-h apps/morse-trainer/main/ark24_font.h --size 24 --name ark24

# 离线校验：宽度是否超屏、有没有缺字（会退化成 ?）、点阵图肉眼核对
py -3.10 tools/check_layout.py --font apps/morse-trainer/main/ark12_font.c `
    --src apps/morse-trainer/main/main.c --width 148 --art "查询"
```

生成的是 **LVGL 原生 `fmt_txt` 格式**（和 Montserrat 等自带字体同构）：
`glyph_bitmap`(1bpp) + `glyph_dsc` + `unicode_list`(sparse cmap) + `font_dsc`，
`get_glyph_dsc/get_glyph_bitmap` 直接指向库里现成函数，**零自定义渲染代码**。
体积很小：12px 覆盖 150+ 字形（含几十个汉字）只用 ~1.2KB 位图，24px 95 字形 ~1.6KB。

> 字体源文件（TTF）不在仓库内（授权/体积原因）。可从 [Ark Pixel Font](https://github.com/TakWolf/ark-pixel-font) 获取
> `ArkPixel12MonoZHHans.ttf`，或直接用仓库里已生成好的 `ark12_font.c / ark24_font.c`（改文案才需要重新生成）。

## 自实现字体踩过的四个坑（都已在脚本/固件里规避）

1. **自定义 `get_glyph_bitmap` 的返回类型**：必须是 `lv_draw_buf_t *`（库末尾就是 `return draw_buf;`），
   渲染器也按这个解引用。返回裸位图指针 → 结构体解引用 → `LoadProhibited` 崩溃。
2. **静态位图快路径只认 A8**：`lv_draw_sw_letter.c` 里 `static_bitmap && format == A8` 才走裸位图，
   否则一律按 A8 处理，1bpp 裸数据会越界读。**所以干脆走官方 fmt_txt 路径**，让库自己把 1bpp 展开成 A8。
3. **`ofs_y` 是"字形底边距基线"**：`letter_coords.y1 = pos.y + (line_height - base_line) - box_h - ofs_y`，
   即 `ofs_y = ascent - y0 - box_h`。搞错符号会让整字下移被裁剪框切掉，屏上只剩零星像素。
4. **扫字集要先剥字符字面量**：源码里 `{'"', ".-..-."}` 这种字符字面量含有 `"`，
   朴素的字符串配对会被带偏，扫出来的字集全错。两个脚本都先剥注释与字符字面量。

## 固件内的自动判定

启动时做字体自检：取几个字，把 1bpp 展开成 A8 后数像素点，打印判定，例如
`ark12 U+957F box=11x11 px=34` / `font selftest: 10 ok, 0 bad -> PASS`。
像素数与离线模拟一致即说明字体链路正确，不需要肉眼看屏。
