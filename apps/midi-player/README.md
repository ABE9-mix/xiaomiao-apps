# MIDI 播放器

单文件整片固件：`dist/xiaomiao-midi-merged.bin`（从 `0x0` 刷）
源码：`main/main.c`（无外部字体依赖，用 LVGL 自带 Montserrat）

## 功能
- 内置 SMF（.mid）解析：格式 0/1、VLQ 变长量、running status、meta tempo。
- 事件表放 **PSRAM**（8MB，映射 4MB），不占内部 DRAM。
- **单声道蜂鸣器的"和弦"近似**：无源蜂鸣器同一时刻只能一个频率，用 1ms 定时器**轮换换音**（琶音）模拟；
  实测 **16ms 是听感下限**（再快低音一个间隔不到一个周期，糊成噪声），档位固定 16/24/32/48/64/96 ms。
- 支持 **SD 卡曲目**：FAT32，根目录 `*.mid`；无卡时播放内置 demo（Family Mart 入店音，40 事件原谱）。
- 设置写 NVS（模式/琶音档位）。

## 操作
| 键 | 作用 |
|---|---|
| 短按 `A` / `B` | 播放/暂停 / 停止 |
| `U/D/L/R` | 选曲 |
| 长按 `A` | CHORD(琶音) ⇄ MONO(取最高音声部) |
| 长按 `B` | 循环琶音档位 16→24→32→48→64→96 ms |

## 编译
```powershell
pwsh -File tools/build.ps1 -Project midi
```

## 状态
已上机验证：`settings: mode=CHORD arp=16ms`、`MIDI event table in PSRAM`、UI 正常启动。
（无 SD 卡时 `sdmmc_card_init` 会报 `0x107` 超时，属正常，回退到内置 demo。）
