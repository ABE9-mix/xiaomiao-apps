# 学而思小喵掌机 (Xueersi XiaoMiao) ESP32 第三方固件

给学而思「小喵」ESP32 掌机写的第三方固件集，以及配套的**方舟像素字体流水线**和完整的构建/刷写脚本。

原厂固件是一个「AC 遥控 v2」（源码未公开）。本仓库是逆向硬件后重写的独立固件，全部用 ESP-IDF + LVGL 9.5 从零实现。

---

## 固件一览

| 固件 | 说明 | 产物 |
|---|---|---|
| **摩斯码训练器** | 5 个模式：查询 / 顺序学 / 听辨 / 发报 / 设置。中文界面（方舟 12px + 24px 主显）、开机字体自检、设置写 NVS、板载 LED 做按键与发报指示 | `dist/xiaomiao-morse-merged.bin` |
| **MIDI 播放器** | 内置 SMF 解析（格式 0/1、VLQ 变速、running status），事件表放 PSRAM；支持 SD 卡曲目；单声道蜂鸣器上用**轮换换音近似和弦** | `dist/xiaomiao-midi-merged.bin` |

### 摩斯码训练器操作

| 模式 | 做什么 | 按键 |
|---|---|---|
| **查询** LOOKUP | 52 项全表游标（A–Z / 0–9 / 标点），显示字符 + 点划并发声 | `U/D` ±1，`L/R` ±10，`A` 发声，`B` 隐藏字符（自测） |
| **顺序** LEARN | 按 Koch/LANL 顺序（K M R S U A P T L O W I . N J E F 0 Y V , G 5 / Q 9 Z H 3 8 B ? 4 2 7 C 1 D 6 X）逐个学 | `U/D` 上/下一个（自动发声），`A` 重播，`B` 回到第一个 |
| **听辨** TYPE | **播放一个随机字的莫斯音 → 从 5 个候选字里选**（6 个键打不了字，所以是听音选字）。带正确率/连击 | `L/R` ±1，`U/D` ±5，`A` 提交，`B` 重播 |
| **发报** KEY | 按键发报并自动解码成字符 | 单键：按住 `A` 按时长判点/划；双键：`A`=点 `B`=划；`B` 清空（单键） |
| **设置** SET | 列表：键位引导（图形 d-pad 子页）/ 声音开关 / 速率 | `U/D` 选，`A` 进入或切换，`B` 返回 |

全局：**长按 `L/R` 换模式**，**长按 `A` 循环 WPM**（5/8/10/12/15/20），发报模式下**长按 `U` 切单/双键**。
模式 / WPM / 单双键 / 学习进度 / 声音开关全部写 NVS，重启保留。
只在模式页显示「非直觉」的长按提示；上下选择、左右翻页、A 确认、B 返回这类直觉操作不占屏幕，完整键位图放在设置页。

---

## 硬件

| 项 | 参数 |
|---|---|
| MCU | ESP32-D0WD rev1.0（双核 240MHz），**8MB PSRAM**（映射 4MB），4MB flash |
| 屏幕 | ST7735 160×128，SPI，旋转 90°；SPI2 60MHz |
| 屏幕引脚 | SCK 18 / MOSI 23 / MISO 19 / CS 5 / DC 4 |
| 蜂鸣器 | GPIO 14，无源，LEDC PWM（单声道 → 同时只能一个频率） |
| 按键 | UP 2 / DOWN 13 / LEFT 27 / RIGHT 35 / A 34 / B 12（按下为低；34、35 无内部上拉） |
| SD 卡 | CS 22（SDSPI，FAT32） |
| I2C | SCL 15 / SDA 21 → **GD32F350 协处理器 @0x40**，LED1 寄存器 `0xA0`、LED2 `0xA1`（值 1 = 亮） |
| USB | GD32F350G8 USB-CDC 桥（`VID_28E9&PID_018A`），支持自动下载/复位 |
| 分区表 | `nvs@0xa000 24K` / `phy_init@0x10000 4K` / `factory@0x20000 1M` |

---

## 快速开始

### 1. 准备工具链
需要 **ESP-IDF v5.5.4**（实测版本；LVGL 9.5.0 由组件管理器自动拉取）。
把 IDF 放在仓库下的 `idf/esp-idf-v5.5.4`、工具放 `idf-tools/`，或用参数/环境变量指定：

```powershell
pwsh -File tools/build.ps1 -Project morse -IdfPath D:\esp-idf -ToolsPath D:\espressif
```

`tools/build.ps1` 会自动处理几个常见坑：临时目录/组件缓存/ccache 收进仓库 `tmp/`、
`IDF_GITHUB_ASSETS=dl.espressif.com/github_assets`（github.com 被 DNS 劫持时组件仍能下）、
以及「只有 build 缺失或目标不对时才 `set-target`」——否则每次都会清空 build 全量重编。

### 2. 构建

```powershell
pwsh -File tools/build.ps1 -Project morse     # 首次全量约 6 分钟，增量约 40 秒
pwsh -File tools/build.ps1 -Project midi
pwsh -File tools/build.ps1 -Project xueersi   # 上游 dashboard 工程
```

产物：`dist/xiaomiao-<project>-merged.bin`。

### 3. 刷入

```powershell
pwsh -File tools/flash.ps1                                            # 默认摩斯固件 → COM6
pwsh -File tools/flash.ps1 -Bin dist/xiaomiao-midi-merged.bin -Monitor
```

**刷入偏移**：`merge-bin` 出来的**合并固件从 `0x0` 整片刷**；
单个 bin 才是 `0x1000`(bootloader) / `0x9000`(分区表) / `0x20000`(app)。

### 4. 备份与回滚
刷之前先整片备份（换固件前必做）：

```powershell
python -m esptool --port COM6 --chip esp32 --baud 460800 read-flash 0 0x400000 backup/stock-4MB.bin
```

回滚就是再整片刷回那份备份（也是 `0x0`）。

---

## 方舟像素字体流水线

屏幕只有 160×128，用**方舟像素字体（Ark Pixel）**最合适。本仓库不依赖 `lv_font_conv`，
而是用两个脚本自己生成 LVGL 原生字体：

```powershell
# 12px 界面字：字集自动从源码字符串里扫（ASCII + 用到的汉字）
py -3.10 tools/gen_ark_font.py --ttf <ArkPixel12MonoZHHans.ttf> `
    --src project/xiaomiao-morse/main/main.c `
    --out-c project/xiaomiao-morse/main/ark12_font.c `
    --out-h project/xiaomiao-morse/main/ark12_font.h --size 12

# 24px 主显字：用 12px 的轮廓版按 2× 渲染，出来是完美 2 倍像素（灰度只有 0/255）
py -3.10 tools/gen_ark_font.py --ttf <ArkPixel12MonoZHHans.ttf> `
    --out-c project/xiaomiao-morse/main/ark24_font.c `
    --out-h project/xiaomiao-morse/main/ark24_font.h --size 24 --name ark24

# 离线校验：宽度是否超屏、有没有缺字（会退化成 ?）、点阵图肉眼核对
py -3.10 tools/check_layout.py --font project/xiaomiao-morse/main/ark12_font.c `
    --src project/xiaomiao-morse/main/main.c --width 148 --art "查询"
```

生成的是 **LVGL 原生 `fmt_txt` 格式**（和 Montserrat 等自带字体同构）：
`glyph_bitmap`(1bpp) + `glyph_dsc` + `unicode_list`(sparse cmap) + `font_dsc`，
`get_glyph_dsc/get_glyph_bitmap` 直接指向库里现成函数，**零自定义渲染代码**。
当前 12px 字体覆盖 169 字形（71 汉字）只用 **1519 字节**位图，24px 95 字形 **1570 字节**。

### 自实现字体踩过的四个坑（都已在脚本/固件里规避）
1. **自定义 `get_glyph_bitmap` 的返回类型**：必须是 `lv_draw_buf_t *`（库末尾就是 `return draw_buf;`），
   渲染器也按这个解引用。返回裸位图指针 → 结构体解引用 → `LoadProhibited`。
2. **静态位图快路径只认 A8**：`lv_draw_sw_letter.c` 里 `static_bitmap && format == A8` 才走裸位图，
   否则一律按 A8 处理，1bpp 裸数据会越界读。**所以干脆走官方 fmt_txt 路径**。
3. **`ofs_y` 是"字形底边距基线"**：`letter_coords.y1 = pos.y + (line_height - base_line) - box_h - ofs_y`，
   即 `ofs_y = ascent - y0 - box_h`。搞错符号会让整字下移被裁剪框切掉，屏上只剩零星像素。
4. **扫字集要先剥字符字面量**：源码里 `{'"', ".-..-."}` 这种字符字面量含有 `"`，
   朴素的字符串配对会被带偏，扫出来的字集全错。两个脚本都先剥注释与字符字面量。

固件启动时会做**字体自检**（取几个字，把 1bpp 展开成 A8 后数像素点）并打印判定，
例如 `ark12 U+957F box=11x11 px=34` / `font selftest: 10 ok, 0 bad -> PASS`。

---

## 目录结构

```
├─ project/
│  ├─ xiaomiao-morse/     # 摩斯码训练器（含生成的 ark12/ark24 字体）
│  ├─ xiaomiao-midi/      # MIDI 播放器
│  └─ xueersi-idf/        # 上游 dashboard 工程（参考）
├─ tools/
│  ├─ gen_ark_font.py     # TTF → LVGL fmt_txt 字体（含 tofu 检查）
│  ├─ check_layout.py     # 离线排版/缺字/点阵校验
│  ├─ build.ps1           # 构建（可移植，参数化路径）
│  └─ flash.ps1           # 刷入 + 抓启动日志
├─ dist/                  # 合并固件（可直接 0x0 整片刷）
└─ docs/development-log.md  # 完整开发日志（踩坑过程、实测数据、事故复盘）
```

`docs/development-log.md` 是逐条推进的记录：硬件逆向、USB 桥与下载复位、
分区与 PSRAM 分配、LVGL 三缓冲 DMA、MIDI 和弦的物理极限（<16ms 一个间隔不到一个周期）、
白屏事故的 addr2line 复盘等。想知道"为什么这么写"就翻它。

---

## 上游与相关项目
- [ZyoungInc/xueersi-idf](https://github.com/ZyoungInc/xueersi-idf) —— 小喵掌机的 ESP-IDF 工程与原理图（`project/xueersi-idf/` 即其拷贝，非本仓库原创）
- [pysn2012/xueersi-xiaomiao](https://github.com/pysn2012/xueersi-xiaomiao) —— 社区资料与预编译固件
- 社区 MicroPython 固件（LVGL 9.3 / SPI 屏版本）不在仓库内，见开发日志中的记录

## 已知问题 / 待办
- [ ] 使用体验细节（实际把玩后再定：动画、长按手感、模式切换反馈）
- [ ] 听辨模式可考虑增加「看字打码」阶段（给字 → 你敲莫斯 → 判对错）
- [ ] SD 卡曲目列表页（MIDI 固件目前只有序号行，插入 FAT32 卡后可选真实文件）
- [ ] USB 桥偶发断连导致刷写中断（这是 GD32 桥的问题，非固件）

## 许可
本仓库自有代码可自由使用。`project/xueersi-idf/`、方舟像素字体（Ark Pixel Font, SIL OFL）等
第三方内容遵循各自原始许可。
