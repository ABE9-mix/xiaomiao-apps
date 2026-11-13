# XIAOMIAO 掌机程序合集

学而思「小喵」ESP32 掌机的第三方程序合集。原厂固件是一个「AC 遥控 v2」（源码未公开），
这里全部是逆向硬件后重写的独立固件，用 ESP-IDF v5.5.4 + LVGL 9.5 从零实现。

> ⚠️ **每个程序是独立的整片固件（从 `0x0` 整片刷），彼此互斥** —— 刷一个就覆盖上一个。
> 它们不是同一个固件里的多个页面；想换功能就重刷对应的 bin。日常不会同时存在。

## 程序一览

| 程序 | 目录 | 一句话 | 固件 |
|---|---|---|---|
| **摩斯码训练器** | [apps/morse-trainer](apps/morse-trainer/) | 5 模式：查询 / 顺序学 / 听辨(听音选字) / 发报(单双键) / 设置；中文界面、开机字体自检 | `dist/xiaomiao-morse-merged.bin` |
| **MIDI 播放器** | [apps/midi-player](apps/midi-player/) | 内置 SMF 解析播放；单声道蜂鸣器上用轮换换音近似和弦；支持 SD 曲目 | `dist/xiaomiao-midi-merged.bin` |
| **节拍器** | [apps/metronome](apps/metronome/) | BPM 30–300、拍号 2–12、细分 ×1–4、重音、打拍定速、拍点+LED；**调音器已留槽** | `dist/xiaomiao-metronome-merged.bin` |

每个目录里有各自的 README（操作说明、实现要点、编译命令、验证状态）。

## 硬件（所有程序共用）

| 项 | 参数 |
|---|---|
| MCU | ESP32-D0WD rev1.0（双核 240MHz），**8MB PSRAM**（映射 4MB），4MB flash |
| 屏幕 | ST7735 160×128，SPI，旋转 90°；SPI2 60MHz |
| 屏幕引脚 | SCK 18 / MOSI 23 / MISO 19 / CS 5 / DC 4 |
| 蜂鸣器 | GPIO 14，无源，LEDC PWM（单声道，同时只能一个频率） |
| 按键 | UP 2 / DOWN 13 / LEFT 27 / RIGHT 35 / A 34 / B 12（按下为低；34、35 无内部上拉） |
| SD 卡 | CS 22（SDSPI，FAT32） |
| I2C | SCL 15 / SDA 21 → GD32F350 协处理器 @0x40，LED1 `0xA0`、LED2 `0xA1`（1=亮） |
| USB | GD32F350G8 USB-CDC 桥（`VID_28E9&PID_018A`），支持自动下载/复位 |
| 分区表 | `nvs@0xa000 24K` / `phy_init@0x10000 4K` / `factory@0x20000 1M` |

## 快速开始

```powershell
# 1) 构建（需 ESP-IDF v5.5.4；默认找仓库下 idf/ 与 idf-tools/，可用 -IdfPath 指定）
pwsh -File tools/build.ps1 -Project morse        # 或 midi / metronome
#    首次全量约 6 分钟，增量约 40 秒；产物 dist/xiaomiao-<project>-merged.bin

# 2) 刷入（合并固件从 0x0 整片刷）+ 抓启动日志
pwsh -File tools/flash.ps1 -Bin dist/xiaomiao-metronome-merged.bin -Port COM6 -Monitor
```

**刷入偏移**：`merge-bin` 的**合并固件从 `0x0` 整片刷**；单个 bin 才是 `0x1000`(bootloader) / `0x9000`(分区表) / `0x20000`(app)。

**备份**（换固件前必做）：

```powershell
python -m esptool --port COM6 --chip esp32 --baud 460800 read_flash 0 0x400000 backup/stock-4MB.bin
```

## 公共基础

- **方舟像素字体流水线**（12px 界面 + 24px 主显，含 tofu 检查与四个踩坑）→ [docs/ark-font.md](docs/ark-font.md)
- **构建 / 刷写脚本** → [tools/](tools/)（`build.ps1` 参数化路径、`flash.ps1` 带启动日志抓取）
- **逐条开发记录**（硬件逆向、LVGL 三缓冲、白屏事故 addr2line 复盘、和弦物理极限等）→ [docs/development-log.md](docs/development-log.md)

## 目录结构

```
├─ apps/
│  ├─ morse-trainer/      # 摩斯码训练器（含生成的 ark12/ark24 字体）
│  ├─ midi-player/        # MIDI 播放器
│  ├─ metronome/          # 节拍器（+ 调音器占位）
│  └─ xueersi-idf/        # 上游 dashboard 工程（本地参考，未入库）
├─ tools/
│  ├─ gen_ark_font.py     # TTF → LVGL fmt_txt 字体（含 tofu 检查）
│  ├─ check_layout.py     # 离线排版/缺字/点阵校验
│  ├─ build.ps1           # 构建（可移植）
│  └─ flash.ps1           # 刷入 + 抓启动日志
├─ dist/                  # 三个程序的合并固件（0x0 整片刷）
└─ docs/                  # 字体流水线 + 开发日志
```

## 上游与相关项目
- [ZyoungInc/xueersi-idf](https://github.com/ZyoungInc/xueersi-idf) —— 小喵掌机 ESP-IDF 工程与原理图（本仓库未包含其源码，仅本地保留用于构建 dashboard）
- [pysn2012/xueersi-xiaomiao](https://github.com/pysn2012/xueersi-xiaomiao) —— 社区资料与预编译固件
- 社区 MicroPython 固件（LVGL 9.3 / SPI 屏版本）见开发日志

## 许可
本仓库自有代码可自由使用。方舟像素字体（Ark Pixel Font）为 SIL OFL；`apps/xueersi-idf/` 等第三方内容遵循各自原始许可。
