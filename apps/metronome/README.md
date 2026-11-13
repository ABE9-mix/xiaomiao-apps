# 节拍器（+ 调音器规划）

单文件整片固件：`dist/xiaomiao-metronome-merged.bin`（从 `0x0` 刷）
源码：`main/main.c` + `main/ark12_font.c` / `main/ark24_font.c`

## 功能

| 项 | 范围 |
|---|---|
| BPM | 30–300（`U/D` ±1，`L/R` ±5，**`B` 打拍定速**） |
| 拍号 | 2–12 拍，拍值 4/8（显示如 `6/8`；设置页 `A` 切拍值） |
| 细分 | ×1 / ×2 / ×3 / ×4（长按 `L/R` 循环） |
| 重音 | 首拍重音 / 每拍重音 |
| 其它 | 闪灯开关、声音开关 |

**操作**：`A` 开始/停止（长按 `A` 进设置）· `B` 打拍定速（长按切调音器）· `U/D` BPM±1（长按改拍数）· `L/R` BPM±5（长按改细分）。
设置页 6 项：拍号 / 细分 / 重音 / 闪灯 / 声音 / 键位引导。全部写 NVS。

界面：24px 大号 BPM + 拍号 + 一排拍点圆点（当前拍高亮、首拍 cream 色）+ 拍内进度条；
板载 LED1 每拍闪 40ms、LED2 常亮表示运行。

## 实现要点
- **拍点引擎**：1ms `esp_timer` + **微秒相位累加** `acc_us += 1000; while(acc_us>=gap_us){acc_us-=gap_us; fire();}`
  —— 平均 BPM 不漂移；高 BPM/高细分（一毫秒内多拍）不丢。`beat_us = 60e6/BPM`，`gap_us = beat_us/细分`。
- 点击音：重音 2093Hz/30ms、普通 1568Hz/20ms、细分 1047Hz/12ms，由 1ms tick 倒数关断。
- 每小节打一条 `bar dt=…ms exp=…ms` 日志，**可按日志实测实际 BPM 是否准**。

## 调音器（预留，硬件未到）
- 长按 `B` 可进 `MODE_TUNER` 占位页，显示 `I2S BCK25 WS26 DAT32`；`PIN_MIC_BCK/WS/DATA = 25/26/32` 已定义。
- 计划硬件：I2S 数字麦 INMP441 / ICS-43434（10–20 元），`BCK=25 WS=26 DATA=32 VDD=3V3 GND`，L/R 接地选左声道。
- 采样 16kHz/16bit/单声道，1024–2048 点/帧；基频用**自相关 / YIN**（不用 FFT 峰值，谐波强会上八度）；
  界面：音名 + cents(−50..+50) 指针。到时只需补 `tuner_tick()` + 界面。

## 编译
```powershell
pwsh -File tools/build.ps1 -Project metronome
```

## 状态
已上机验证：`settings: bpm=120 beats=4/4 sub=1 accent=0 sound=1 led=1`、`gd32 led: FOUND`、
`font selftest: 8 ok, 0 bad -> PASS`、无 panic。BPM 精度可用 `bar dt/exp` 日志实测（待补测）。
