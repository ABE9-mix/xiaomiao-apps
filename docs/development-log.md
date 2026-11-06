# 小喵掌机 (xueersi-xiaomiao) 刷固件记录

> 目标设备：学而思 ESP32 小喵掌机（ESP32-D0WD rev1.0 + 8MB PSRAM + 4MB Flash，MAC `90:38:0c:d1:f1:d8`）
> 串口：`COM6` = `USB 串行设备 (GD32F3X0-1.0.0-7Z8X9YER)`，VID_28E9&PID_018A（板载 GD32F350 做 USB-CDC 串口桥 + 自动下载/复位）
> 刷写时间：2026-11-06

---

## 一、从 Hermes 转进的项目记忆里查到的内容

### 1) `F:\AIchat\hermes_migrate`（迁移整理好的 ESP32 知识库，2026-11-01）
- `00-README.md` / `MIGRATION_SCOPE.md`：28 个 Hermes 会话、3 个 profile 记忆、7 个技能的迁移地图。
- `raw\msldf2djh1f0ap.md`（2026-08-09）是唯一直接提到小喵掌机的原始会话：*"你记得之前的 xiaomiao 掌机和墨水屏两个设备吗，我想试试移植 steam 令牌到上面……先画个 LVGL 显示五位验证码……令牌文件在 F:\Hermes works\Steam(ABE922).mpo"*。
- 注意：这份迁移的知识库主体是**微雪 ESP32-S3-LCD-2**（空气检测/电机力反馈/墨水屏/停车引导），与小喵掌机是不同板子。

### 2) Hermes 运行库 `.hermes-web-ui\.ekko\ekko.db`（未导出，本次直接查原始库）
- 会话 `msq0zzj9rrv2l1`（2026-08-12）**就是小喵掌机的项目记忆**：用户当时想给掌机移植 Meshtastic LoRa，给的仓库是 **`https://github.com/pysn2012/xueersi-xiaomiao`**。
- 该会话确认的硬件事实：**屏幕 ST7735 128×160、4MB Flash**；"这板子能被 ESPHome 刷，说明 esptool 通道是通的"；还确认了对 DIO0/Ra-02(SX1278) 的处理思路。
- 那个仓库 `bins/` 里有现成固件（已随本次下载到 `community/` 和 `dist/`）：
  - `ESP32_SPIRAM_1.24_SDCARDST7735.bin`（1.63MB）
  - `lvgl9.3_mpy_ESP32_GENERIC_SPIRAM_4MB_st7735.bin`（2.58MB）
  - 两者均为**合并镜像，写到 0x1000**（文件头 E9 引导、0x7000 处是分区表）。

### 3) 本机实际工程（真正要刷的固件源码）
- `F:\Hermes works\xiaomiao_steam` —— 就是 `github.com/ZyoungInc/xueersi-idf` 的浅克隆（分支 zyoung/main，HEAD `aabf45e`），ESP-IDF + LVGL 9.5 的掌机固件工程，附 `xueersi-xiaomiao-schematic.pdf`。

> 结论：掌机存在两套固件生态 —— 社区的 **MicroPython + LVGL**（pysn2012）与本机在做的 **ESP-IDF + LVGL**（ZyoungInc）。本次按你选择，刷的是 **ZyoungInc ESP-IDF 工程重新编译的固件**。

---

## 二、刷机前的状态（已整片备份）

刷之前设备里跑的是 **2026-07-13 本地 dirty 编译的 ESP-IDF 固件**（project `xiaomiao`、App version `stable-6-gaabf45e-dirty`、UI 是「AC 遥控 v2」、IR TX GPIO25），**这份 AC 遥控源码不在仓库里**。

- 整片 4MB 备份：`backup\xiaomiao-stock-4MB.bin`（SHA256 见文件哈希，可用下面的回滚命令原样写回）。

---

## 三、本次编译 & 刷入的固件

- 源码：`project\xueersi-idf\main\main.c`（UI 为「Xiaomiao LVGL 9.5 硬件分页/dashboard」）
- 工具链：ESP-IDF **v5.5.4** + xtensa-esp-elf 14.2.0（离线手动安装，工具走 `dl.espressif.com/github_assets` 镜像）
- 产物：`dist\xiaomiao-merged.bin`（825,568 B / 0.79 MB，从 0x0 整片刷入）
- 分区表：`nvs@0xa000(24K)` / `phy_init@0x10000(4K)` / `factory@0x20000(1M)`，4MB Flash，PSRAM 8MB

### 刷入结果（上电启动日志实测）
```
boot: ESP-IDF v5.5.4-dirty 2nd stage bootloader   (compile Nov 6 2026)
boot.esp32: SPI Speed : 80MHz   Mode: QIO   Flash Size: 4MB
quad_psram: This chip is ESP32-D0WD / Found 8MB PSRAM device / SPI SRAM memory test OK
app_init: Project name: xiaomiao   App version: 1   ESP-IDF: v5.5.4-dirty
xiaomiao_dash: Xiaomiao LVGL 9.5 dashboard boot
xiaomiao_dash: Initialize ST7735R panel ...
xiaomiao_dash: LVGL display: 160x128, dpi=60, 3 full-screen DMA buffers, SPI=60 MHz
xiaomiao_dash: Start Xiaomiao hardware dashboard
```
写入时 esptool 已做 **Hash of data verified**（写入校验通过）。

---

## 四、复现命令

### 重新编译（生成 dist\xiaomiao-merged.bin）
```powershell
pwsh -File F:\AIchat\xiaomiao-flash\run-build2.ps1
```

### 刷入（COM6，整片 0x0）
```powershell
pwsh -File F:\AIchat\xiaomiao-flash\flash-xiaomiao.ps1 -Bin F:\AIchat\xiaomiao-flash\dist\xiaomiao-merged.bin
# 或手动：
uv tool run esptool --port COM6 --chip esp32 --baud 460800 ^
  --before default_reset --after hard_reset write-flash -z ^
  --flash_mode dio --flash_freq 80m --flash_size 4MB ^
  0x0 F:\AIchat\xiaomiao-flash\dist\xiaomiao-merged.bin
```

### 回滚到前一份固件（AC 遥控 dirty build，整片 4MB）
```powershell
uv tool run esptool --port COM6 --chip esp32 --baud 460800 ^
  --before default_reset --after hard_reset write-flash -z ^
  0x0 F:\AIchat\xiaomiao-flash\backup\xiaomiao-stock-4MB.bin
```

### 串口看日志
```powershell
# 115200，8N1；按 RTS 复位即可看到 boot 日志
```

---

## 五、目录说明
```
F:\AIchat\xiaomiao-flash\
├─ dist\
│   ├─ xiaomiao-merged.bin                              ← 本次编译产物（刷 0x0）
│   ├─ lvgl9.3_mpy_ESP32_GENERIC_SPIRAM_4MB_st7735.bin  ← 社区 MicroPython 备选
│   └─ ESP32_SPIRAM_1.24_SDCARDST7735.bin               ← 社区 MicroPython 备选
├─ backup\xiaomiao-stock-4MB.bin                        ← 刷前整片备份（回滚用）
├─ project\xueersi-idf\                                 ← 固件源码（ZyoungInc）
├─ idf\esp-idf-v5.5.4\                                  ← ESP-IDF 源码
├─ idf-tools\                                           ← 工具链 + python 环境
├─ community\xueersi-xiaomiao-main\                     ← pysn2012 社区仓库快照
├─ flash-xiaomiao.ps1 / build-xiaomiao.ps1 / run-build2.ps1
└─ hermes-ekko-xiaomiao.txt                             ← 从 Hermes ekko.db 导出的小喵会话原文
```

## 六、踩坑记录（复现时必看）
1. **GitHub 主站被 DNS 污染**（`github.com`→127.0.0.1），但 `codeload.github.com` / `raw.githubusercontent.com` / `dl.espressif.com` 正常；ESP-IDF 官方离线安装器需要管理员权限，非管理员会静默 exit=1，故改为**手动解压源码 + 工具链走 `IDF_GITHUB_ASSETS=dl.espressif.com/github_assets`**。
2. **pip 默认走 IPv6**，本机 IPv6 黑洞会卡死；用 `uv pip install`（IPv4 正常）装 IDF 依赖。
3. 沙箱限制子进程写系统 TEMP/ccache/组件缓存 → 把 `TMP/TEMP`、`IDF_COMPONENT_CACHE_PATH`、`CCACHE_DIR` 全部指到工作区，并关闭 ccache。
4. `CONFIG_PARTITION_TABLE_OFFSET=0x9000` 使默认分区表整体后移（nvs@0xa000…），与原先固件一致。

---

# 追加固件：MIDI 播放器（2026-11-06）

## 功能
- 从 SD 卡 `/sdcard` 读取 `*.mid`（SMF format 0/1），解析 tempo / note on / off，用 GPIO14 无源蜂鸣器单音播放（最新音符优先近似复音；note-off 只关正在响的音）。
- 无 SD 卡或没有 .mid 时自动进入内置 DEMO：**FamilyMart 入店音**（Panasonic EC-5347 Melody Sign，源自 `Japanese Family Mart 入店音 Entrance Sound_Jingle (Panasonic EC-5347 Melody Sign Original).mid`）。
  - 原曲 SMF fmt0 / div=384 / tempo=714285us/qn(≈84BPM) / 5.36s，都是 Synth Bass 和弦；固件里按**最高音声部归约为单音旋律**后再喂给蜂鸣器：
    `G5 - D#5 - A#4 - D#5 - F5 - A#5 - F4 - F5 - G5 - F5 - (休) - D#5 - (休)`，时间轴与原文件逐事件一致。
  - 表格在 `project\xiaomiao-midi\main\main.c` 的 `k_demo[]`，改曲子只改这张表。
- LVGL 160×128 界面：曲名(序号/总数)、当前音名+频率、进度条、播放状态。
- 按键：`UP/DOWN` 选曲，`A` 播放/暂停，`B` 停止，`LEFT/RIGHT` 上一首/下一首。
- 开机自动响一段 C5-E5-G5 提示音，确认蜂鸣器工作。

## 工程与产物
- 源码：`project\xiaomiao-midi\`（独立 app，复用官方已验证的 ST7735 / SPI2 / 蜂鸣器 / SD 初始化；CMake 工程名 `xiaomiao_midi`）
- 产物：`dist\xiaomiao-midi-merged.bin`（787,824 B，从 0x0 刷；含 FamilyMart 入店音 demo）
- 构建脚本：`run-build-midi.ps1`

## 已实测启动日志
```
xiaomiao_midi: MIDI event table in PSRAM
ST7735 init (MicroPython init(2) compatible)
LVGL display 160x128, 3 full-screen DMA buffers, SPI 60 MHz
(vfs_fat: sdmmc_card_init failed 0x107 = 当前未插 SD 卡)
xiaomiao_midi: Start MIDI player UI
```

## 放置 MIDI 文件
SD 卡（FAT32）根目录放 `*.mid` 即可，重新上电会自动扫描；选择到非 DEMO 曲目按 A 播放。

## 踩坑
- 事件表 32KB 若放静态 BSS 会挤占内部 DRAM，导致 `spi_bus_dma_memory_alloc` 失败（`assert b1&&b2&&b3`）→ 改放 PSRAM。
- 必须先建 LVGL 显示 DMA 缓冲、再挂 SD，否则 SD 先占内部 DMA 内存导致显示缓冲分配失败。

## 回滚
- 回「硬件 dashboard」固件：刷 `dist\xiaomiao-merged.bin`
- 回原厂/AC 遥控固件：刷 `backup\xiaomiao-stock-4MB.bin`

### 和弦（琶音）与档位（2026-11-06 定稿）
- 单只无源蜂鸣器同一时刻只能一个频率，"和弦"用**轮换换音近似**：统计当前按下的所有音高，由独立 **1 ms esp_timer**（`arp_timer_cb`）每 `s_arp_ms` 换一个音。
- 换音必须挂独立定时器：LVGL 主循环最长睡 10 ms，原来挂在那里最快只能 25 ms。
- **实测下限 16 ms**：<16 ms 时低音（A#4=466 Hz）一个间隔里不到一个周期，音高糊成噪声，所以档位只保留 `16 / 24 / 32 / 48 / 64 / 96 ms`，默认 **16 ms**。
- **长按 A**：CHORD(琶音) ⇄ MONO(取最高音声部)。**长按 B**：循环上述档位。
- 两个设置写进 **NVS**（命名空间 `midi`，键 `chord` / `arpms`），重启保留；启动日志会打 `settings: mode=... arp=...ms`。
- 短按功能不变：A 播放/暂停、B 停止、U/D/L/R 选曲。

### 刷入日志（2026-11-06 实测）
```
settings: mode=CHORD arp=16ms
MIDI event table in PSRAM
LVGL display 160x128, 3 full-screen DMA buffers, SPI 60 MHz
(vfs_fat: sdmmc_card_init failed 0x107 = 未插 SD 卡)
Start MIDI player UI
```

---

# 追加固件：摩斯码训练器（2026-11-06）

工程：`project\xiaomiao-morse\`（独立 app，工程名 `xiaomiao_morse`，复用官方已验证的 ST7735 / SPI2 / 蜂鸣器 / 按键 / NVS）
产物：`dist\xiaomiao-morse-merged.bin`（740,256 B，从 0x0 刷）
构建：`pwsh -File run-build-morse.ps1`

## 四个模式（长按 LEFT / RIGHT 切换）
| 模式 | 干什么 | 操作 |
|---|---|---|
| **LOOKUP 查询** | 全表游标浏览（A-Z、0-9、26 个标点，共 52 项），显示字符+点划 | U/D ±1、L/R ±10、A 发声、B 切换 HIDE 自测 |
| **LEARN 顺序练** | 按 Koch/LANL 顺序（K M R S U A P T L O W I . N J E F 0 Y V , G 5 / Q 9 Z H 3 8 B ? 4 2 7 C 1 D 6 X）逐个学，带进度条 | U 下一个(自动发声)、D 上一个、A 重播、B 回到第一个 |
| **TYPE 打字练** | 听莫斯选字符，实时正确率/连击；答错 1.2s 后自动下一题 | L/R ±1、U/D ±6、A 提交、B 重播 |
| **KEY 发报练** | 按键发报并**自动解码**成字符，屏幕显示点划串+解出的字符+已解码文本 | 单键：按住 A 按时长判点/划；双键：A=点 B=划；长按 U 切 SGL/DUAL；B 清空(单键) |

## 全局
- **长按 A**：循环 WPM `5 / 8 / 10 / 12 / 15 / 20`（KEY 模式除外，因为 A 是发报键）。点时长 `dot = 1200/wpm` ms，划 = 3×点，符号内间隙 = 1×点，字符间隙 = 3×点。
- **长按 LEFT/RIGHT**：上/下一个模式。
- 模式 / WPM / 单双键 / 学习进度全部写 **NVS**（命名空间 `morse`），重启保留；启动日志打 `settings: mode=... wpm=... dual=... koch=...`。
- 定时：**1ms esp_timer** 统一驱动「莫斯发声队列」和「发报键采样」，与 LVGL 主循环解耦。
- TYPE 练习字符集 = 已学过的 Koch 前缀（`max(6, koch_i+1)`），跟着学习进度自动扩。

## 已做的静态校验
- 52 条莫斯码逐条与标准表比对：**0 错、0 重复**（解码查找无歧义）。
- Koch 顺序串 40 字符、无重复、全部在码表内。
- 二进制内确认含模式名 / Koch 串 / NVS 日志 / 各模式提示串。

## 刷机前必看
- 本固件**开机不会响**（不像 MIDI 固件有提示音）；进 LEARN 模式会自动发当前字符，或按 A 发声验证蜂鸣器。
- 参数记录型设置写 NVS，和 MIDI 固件用的是**同一颗芯片的 NVS 分区**，但命名空间不同（`morse` vs `midi`），互不干扰。

---

# 摩斯固件 v2：方舟字体 + 设置页 + LED 键提示（2026-11-07）

产物：`dist\xiaomiao-morse-merged.bin`（736,960 B，sha256 FE14BB3F…F8AB，从 0x0 刷）

## 1. 方舟像素字体（Ark Pixel 12px）
- 字体源：`F:\AIchat\epd_esp_driver\assets\ark12\*.ms.bitmap.ttf`（12px 位图版；本机也有 `ArkPixel12MonoZHHansBitmap.ttf`）
- **不用 lv_font_conv**：`tools/gen_ark_font.py` 用 Pillow/FreeType 把每个字渲染成 1bpp 位图，直接生成一份自带字形表 + LVGL 9 三个回调（`get_glyph_dsc` / `get_glyph_bitmap` / `release_glyph`）的 C 文件。像素字体渲染出来是 **纯 1bit（灰度级只有 {0,255}）**，不需要抗锯齿。
- 字集**自动从源码字符串字面量扫**（ASCII 0x20–0x7E + 用到的 CJK + `--extra`），当前 158 字形（63 CJK），位图仅 **2068 字节**。
- 指标：`ascent=10 descent=2 line_height=12`，拉丁 advance 6px、中文 12px。
- **LVGL 9.5 的坑**：字形索引字段是联合体 **`gid.index`**，不是 `index`。
- `tools/check_layout.py` 离线三连校验（不用上机）：
  1) **宽度**：把 main.c 里所有文案按 advance 累加，超过 148px 报警 → 本次 222 条 **0 超标**（最宽 144px：`长按U单双键 长按LR换模式`）
  2) **缺字**：文案里出现的非 ASCII 字不在字形表里 → 报警（会退化成 `?`）→ 本次 **全覆盖**
  3) **点阵核对**：把界面真字符串渲染成 ASCII 点阵，肉眼可核字形
- 生成器自带 **tofu 检查**（方舟 12px 中文子集不全的老坑）：本次"全部字形都有点阵 OK"。
- ⚠️ 踩坑：Morse 表里有 `{'"', ".-..-."}` 这种**字符字面量**，朴素的字符串配对会被里面的 `"` 带偏，把字集扫成全错的 → 生成器/校验器都必须先剥注释和字符字面量。
- 固件启动时做 **字体自检并打印判定**（不靠肉眼）：对 `A 0 . - 长 按 码` 逐个取 dsc+位图，统计置位数，日志形如
  `font U+957F box=11x11 adv=192 bits=34 … font selftest: 7 ok, 0 bad -> PASS`。

## 2. 界面文案（中文 + 只留非直觉提示）
- 长的/直觉的提示全部删掉（上下选择、左右翻页、A确认、B返回都不写），模式页只保留两行必要提示：
  - `长按LR换模式` / `长按A调速`（发报模式第二行换成 `长按U单双键`）
  - 打字模式保留 `B重播`；发报单键保留 `B清空`
- 完整的键位说明挪进**设置页的图形键位引导**（那里有整屏空间）。

## 3. 设置页（第 5 个模式：设置）
- **图形键位引导**：3×3 方向键方块图（U/L/中/R/D，只有存在的方向画格子）+ 两行说明 `上下:选择 左右:翻页`、`A:确认 B:返回`
- **声音开关**：开/关（关掉后所有发声静默，含发报侧音）
- **速率**：5/8/10/12/15/20 WPM，实时显示点长 ms
- 全部写 NVS（`morse` 命名空间：mode/wpm/dual/koch/sound），重启保留

## 4. 板载 LED 派给按键提示
- 板子 LED1/LED2 挂在 GD32F350 协处理器上，走 **I2C 0x40，寄存器 0xA0/0xA1**（值 1=亮）
- 固件里新加 I2C master 初始化 + **协处理器探测**：试写一次，失败就标记 ABSENT 并整场不再打扰总线（不拖慢主循环）
- 用途：**LED1 = 任意键按下时亮**；**LED2 = 正在发声/发报时亮**
- 启动日志会打 `gd32 led: FOUND` 或 `ABSENT (led1=… led2=…)`，一眼可知板子这版有没有协处理器

## 5. 已做的静态验证
- 字体：158 字形、tofu 全过、缺字 0、排版 0 超标、点阵图人工核对无误
- 二进制：`k_ark12_bits=0x814(2068B)`、`k_ark12_glyphs=0x9e0(158×16)` 已确认在镜像里（用 `xtensa-esp32-elf-nm -S` 查的，不是猜的）
- app 分区占用 0x93ec0 / 0x100000（**42% 剩余**）

---

# 事故复盘：白屏 = LoadProhibited 重启循环（2026-11-07 已修）

## 现象
刷入方舟字体版后屏幕持续白屏、按键无反应。

## 定位（靠日志，不靠猜）
串口日志显示不是死机而是**崩溃重启循环**：
```
font selftest: 7 ok, 0 bad -> PASS
Start morse trainer UI
Guru Meditation Error: Core 0 panic'ed (LoadProhibited)
EXCVADDR: 0xe07f0042
```
用 `xtensa-esp32-elf-addr2line -pfiaC -e build/xiaomiao_morse.elf <背栈地址...>` 解栈：
```
lv_draw_sw_blend_color_to_rgb565_swapped  ← 崩点
lv_draw_sw_letter.c:189  draw_letter_cb
```
→ 崩在**首帧画文字**。字体数据本身是好的（自检 7/7 PASS，只数了置位点），坏在**格式**。

## 根因：LVGL 9.5 的静态位图快路径只认 A8
`lv_draw_sw_letter.c` 里：
```c
if(lv_font_has_static_bitmap(f) && g->format == LV_FONT_GLYPH_FORMAT_A8) {
    const void * bitmap = lv_font_get_glyph_static_bitmap(g);   // 裸 A8 位图，正确路径
    blend_dsc.mask_buf = bitmap; blend_dsc.mask_stride = g->stride;
}
else {
    glyph_data = lv_font_get_glyph_bitmap(g, _draw_buf);
    const lv_draw_buf_t * draw_buf = glyph_data;   // ★ 把回调返回值当 lv_draw_buf_t* 解引用
    blend_dsc.mask_buf = draw_buf->data;           // ★ 读出垃圾指针
    mask_area.x2 = ... LV_COLOR_FORMAT_A8 ...      // ★ 并且一律按 A8 算
}
```
我最初给的是 **1bpp A1 裸位图**（省内存，2068 B），格式判等不中 → 落进 else 分支 →
裸指针被当结构体解引用 → `mask_buf` 变成 `0xe07f0042` → 逐像素读越界 → LoadProhibited。

**结论：自定义 LVGL 9.5 字体如果要走"静态位图"路径，`format` 必须是 `LV_FONT_GLYPH_FORMAT_A8`，
每像素 1 字节（0/255），`stride = box_w`。** A1/A2/A4 在这条路径上是不被支持的（会被当 A8）。

## 修法
`tools/gen_ark_font.py` 改为输出 A8：
- 位图从 2068 B → **10752 B**（1 字节/像素），app 分区仍只占 0x93ec0/0x100000（剩 42%）
- `dsc->format = LV_FONT_GLYPH_FORMAT_A8`、`dsc->stride = g->w`、`font.static_bitmap = 1`
- 固件里的自检也改成按 A8 数像素（`b[i] > 127`），并打印 `fmt=` 与 `stride=` 便于一眼核对

修复后启动日志：
```
settings: mode=LOOKUP wpm=10 ...
gd32 led: FOUND (led1=ESP_OK led2=ESP_OK)
font U+957F box=11x11 adv=192 stride=11 fmt=8 px=34
font selftest: 7 ok, 0 bad -> PASS
Start morse trainer UI
Returned from app_main()      ← 无 panic、无重启
```

## 顺带修的两个坑
1. **串口日志里的中文模式名打成 `??????`** → 日志改用 ASCII 名（`LOOKUP/LEARN/TYPE/KEY/SET`），屏幕上仍用中文。
2. **`idf.py set-target` 会清空 build 目录**，导致每次"看起来增量"其实全量 6 分钟；
   且构建日志是追加的，判断是否完成时容易读到**上一轮的残留行**（我自己踩了两次）。
   `run-build-morse.ps1` 已改为：只有 `build/CMakeCache.txt` 缺失或 `IDF_TARGET` 不是 esp32 时才跑 set-target，
   其余走增量；**判断构建结果请看产物 mtime / `xtensa-esp32-elf-nm -S` 的符号尺寸，不要只看日志最后一行**。

---

# 字体最终方案：改用 LVGL 原生 fmt_txt（2026-11-07 01:00，**取代上面 A8 那套结论**）

## 实际有两个 bug 叠在一起（A8 只是第一个，修完仍有"零星像素"）
1. **回调返回值类型错**：`get_glyph_bitmap` 必须返回 `lv_draw_buf_t *`（`lv_font_get_bitmap_fmt_txt` 末尾就是 `return draw_buf;`），
   渲染器也这么解：`const lv_draw_buf_t * db = glyph_draw_dsc->glyph_data;`
   我返回裸位图指针 → 结构体解引用 → `mask_buf` 变垃圾 → LoadProhibited 崩溃。
2. **`ofs_y` 的坐标约定搞反**：LVGL 是
   ```c
   letter_coords.y1 = pos.y + (line_height - base_line) - box_h - ofs_y;
   ```
   即 `ofs_y = ascent - y0 - box_h`（字形**底边**距基线的像素数，正=在基线之上）。
   我按 `y0 - ascent` 给（−8），公式一算每个字整体**下移 8px**，全被标签裁剪框切掉 →
   屏上只剩标签边缘的零散像素。**这才是"只有零星像素点"的直接原因。**

## 最终做法：不自己实现回调，直接输出 LVGL 原生字体表
`tools/gen_ark_font.py` 现在生成与 Montserrat 等自带字体**完全同构**的 `fmt_txt` 字体：
```
glyph_bitmap[]  1bpp，行内不补齐、跨行连续，字形间字节对齐
glyph_dsc[]     {bitmap_index, adv_w(8.4 定点), box_w, box_h, ofs_x, ofs_y}
unicode_list[]  相对码位（sparse cmap，一张表覆盖 ASCII+CJK）
cmaps[]         LV_FONT_FMT_TXT_CMAP_SPARSE_TINY
font_dsc        bpp=1, bitmap_format=PLAIN, stride=0, kern=NULL
lv_font_t       .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt
                .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt   ← 用库现成函数，零自定义代码
```
优点：1bpp 极省（**1414 字节**覆盖 158 字形），绘制时由库自己展开成 A8，
跨步/对齐/缓存全部走和自带字体同一条代码路径；静态文案和**会变的字符**（当前字符、点划串、
成绩、WPM）都是同一套渲染，不需要"预渲染图片"那套资产。

## 验证（不上机也能判定，且实测对上了）
1. **离线精确模拟**库的读取算法（含 bpp=1 连续比特流）→ 'A' 展开后 15 像素、'.' 1、'长' 34，形状正确
2. **上机自检**打印每个探测字的 A8 像素数，实测：
```
font U+0041 box=5x8   adv=6  a8stride=5  px=15     ← 与离线模拟完全一致
font U+0030 box=5x8   adv=6  a8stride=5  px=22
font U+002E box=1x1   adv=6  a8stride=1  px=1
font U+002D box=5x1   adv=6  a8stride=5  px=5
font U+957F box=11x11 adv=12 a8stride=11 px=34
font U+6309 box=11x11 adv=12 a8stride=11 px=48
font U+7801 box=11x11 adv=12 a8stride=11 px=50
font selftest: 7 ok, 0 bad -> PASS
```
   （`adv=6/12` 是库把表里的 96/192 做 `(adv+8)>>4` 转成整像素，正常。）
3. 自检本身也踩过两个坑，记下来免得再犯：
   - 传 `NULL` 当 draw_buf → 库内空指针；
   - 把返回的 `lv_draw_buf_t*` 当像素数组数 → 数出 5/17 这种常数（我一度以为字体又错了）。

## 当前产物
`dist\xiaomiao-morse-merged.bin`，735,232 B，sha256 `2C30D90207859DBDF634B5DC4C6C9A5FC23C053F5EF271BBB8CBEAE032AEF7C6`
启动无 panic、无重启，`Start morse trainer UI` 后正常进入主循环。
