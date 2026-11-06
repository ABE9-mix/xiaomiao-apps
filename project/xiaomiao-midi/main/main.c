/*
 * XiaoMiao ESP32-WROVER-B — MIDI Player 固件
 *
 * 硬件：ST7735 SPI TFT(160x128, 旋转90)、MicroSD(SPI2 共用)、GPIO14 无源蜂鸣器(LEDC PWM)、
 *       6 键(UP=2 DOWN=13 LEFT=27 RIGHT=35 A=34 B=12, 按下为低)。
 *
 * 功能：
 *   - 从 /sdcard 读取 *.mid（SMF format 0/1），解析 tempo/note on/off，用蜂鸣器播放；
 *   - 无 SD 或没有 .mid 时，用内置 DEMO（FamilyMart 入店音，原文件 40 个事件原样嵌入）；
 *   - LVGL 128x160 界面：曲名、当前音名/频率、和弦标记、进度条、状态提示；
 *   - 按键短按：UP/DOWN/LEFT/RIGHT 选曲，A 播放/暂停，B 停止；
 *   - 按键长按：A 切换 和弦(琶音) / 单音(取最高音)，B 循环换音速度 {4,2,8,16,32,64}ms。
 *
 * 说明：单只无源蜂鸣器同一时刻只能发一个频率，所以"和弦"用轮换换音近似（琶音）：
 *   统计当前按下的所有音高，由一个 1ms 的 esp_timer 每隔 s_arp_ms 轮换一个音。
 *   实测 <16ms 时低音只剩不到一个周期、音高糊掉，所以档位下限固定为 16ms(≈63Hz)：
 *     16 / 24 / 32 / 48 / 64 / 96 ms，默认 16ms。再慢就是分解和弦。
 *   和弦/单音 两个开关会写进 NVS，重启后保留。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <math.h>
#include <unistd.h>
#include <assert.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "driver/sdspi_host.h"
#include "esp_lcd_panel_io.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "lvgl.h"
#include "sdkconfig.h"
#include "nvs_flash.h"
#include "nvs.h"

/* ---------------- 板级常量（与官方固件一致） ---------------- */
#define LCD_HOST                    SPI2_HOST
#define LCD_PIXEL_CLOCK_HZ          (60 * 1000 * 1000)
#define LCD_NATIVE_H_RES            128
#define LCD_NATIVE_V_RES            160
#define LCD_H_RES                   160
#define LCD_V_RES                   128
#define LCD_DRAW_BUF_LINES          LCD_V_RES
#define LCD_DRAW_BUF_COUNT          3
#define LCD_DPI                     60
#define LCD_CMD_BITS                8
#define LCD_PARAM_BITS              8
#define LCD_X_GAP                   0
#define LCD_Y_GAP                   0

#define PIN_NUM_LCD_SCLK            GPIO_NUM_18
#define PIN_NUM_LCD_MOSI            GPIO_NUM_23
#define PIN_NUM_LCD_MISO            GPIO_NUM_19
#define PIN_NUM_LCD_CS              GPIO_NUM_5
#define PIN_NUM_LCD_DC              GPIO_NUM_4
#define PIN_NUM_SD_CS               GPIO_NUM_22
#define PIN_NUM_BUZZER              GPIO_NUM_14

#define BUTTON_ACTIVE_LEVEL         0
#define BUTTON_DEBOUNCE_MS          20

#define BUZZER_LEDC_MODE            LEDC_LOW_SPEED_MODE
#define BUZZER_LEDC_TIMER           LEDC_TIMER_0
#define BUZZER_LEDC_CHANNEL         LEDC_CHANNEL_0
#define BUZZER_DUTY                 128     /* 8bit 50% */

#define LVGL_TICK_PERIOD_MS         1
#define LVGL_TASK_STACK_SIZE        (12 * 1024)
#define LVGL_TASK_PRIORITY          5
#define LVGL_TASK_MIN_DELAY_MS      2
#define LVGL_TASK_MAX_DELAY_MS      10

#define SD_SPI_MAX_FREQ_KHZ         10000
#define MAX_TRACKS                  32
#define MIDI_MAX_EVENTS             4096
#define MIDI_MAX_FILE_SIZE          (192 * 1024)

/* ST7735 命令 */
#define ST7735_SWRESET              0x01
#define ST7735_SLPOUT               0x11
#define ST7735_NORON                0x13
#define ST7735_INVOFF               0x20
#define ST7735_DISPOFF              0x28
#define ST7735_DISPON               0x29
#define ST7735_CASET                0x2A
#define ST7735_RASET                0x2B
#define ST7735_RAMWR                0x2C
#define ST7735_MADCTL               0x36
#define ST7735_COLMOD               0x3A
#define ST7735_FRMCTR1              0xB1
#define ST7735_FRMCTR2              0xB2
#define ST7735_FRMCTR3              0xB3
#define ST7735_INVCTR               0xB4
#define ST7735_PWCTR1               0xC0
#define ST7735_PWCTR2               0xC1
#define ST7735_PWCTR3               0xC2
#define ST7735_PWCTR4               0xC3
#define ST7735_PWCTR5              0xC4
#define ST7735_VMCTR1               0xC5
#define ST7735_GMCTRP1              0xE0
#define ST7735_GMCTRN1              0xE1
#define MADCTL_MY                   0x80
#define MADCTL_MX                   0x40
#define MADCTL_MV                   0x20
#define MADCTL_RGB                  0x00

static const char *TAG = "xiaomiao_midi";

/* ---------------- 按键 ---------------- */
typedef enum { KEY_NONE = -1, KEY_UP = 0, KEY_DOWN, KEY_LEFT, KEY_RIGHT, KEY_A, KEY_B } key_id_t;
typedef struct { gpio_num_t gpio; key_id_t id; } btn_t;
static const btn_t s_btns[] = {
    {GPIO_NUM_2,  KEY_UP},
    {GPIO_NUM_13, KEY_DOWN},
    {GPIO_NUM_27, KEY_LEFT},
    {GPIO_NUM_35, KEY_RIGHT},
    {GPIO_NUM_34, KEY_A},
    {GPIO_NUM_12, KEY_B},
};

/* ---------------- MIDI 事件 ---------------- */
typedef struct { uint32_t t_ms; uint8_t note; uint8_t vel; } midi_ev_t; /* vel==0 = note off */
static midi_ev_t *s_ev = NULL;   /* 运行时从 PSRAM 分配，避免占内部 DRAM */
static int s_ev_count = 0;

/* ---------------- 曲目 ---------------- */
typedef struct { char name[64]; bool demo; } track_t;
static track_t s_tracks[MAX_TRACKS];
static int s_track_count = 0;
static int s_track_sel = 0;

/* ---------------- 状态 ---------------- */
static bool s_sd_mounted = false;
static sdmmc_card_t *s_sd_card = NULL;
static bool s_buz_ready = false;
static bool s_playing = false;
static bool s_paused = false;
static int  s_ev_i = 0;
static uint32_t s_play_base = 0;      /* 本次播放起点 tick */
static uint32_t s_base_elapsed = 0;   /* 暂停前累计毫秒 */
static int  s_cur_note = -1;          /* 正在响的音符 */
static uint32_t s_cur_freq = 0;

/* 复音跟踪 + 快速换音模拟和弦（单蜂鸣器不可能同时发两个频率） */
static uint8_t s_note_active[128];    /* 每个音高的叠音计数 */
static int  s_sounding = -1;          /* 当前实际发声的音高 */
static int  s_chord_n = 0;            /* 当前同时按下的音数（>1 即和弦） */

static int  s_chord_on = 1;           /* 1=和弦(快速换音近似)  0=单音(取最高音声部) */
static int  s_arp_ms   = 16;          /* 换音间隔 ms；实测 <16ms 音高发糊，故下限 16ms */
#define ARP_MS_MIN 16                 /* 琶音档位下限：16ms(≈63Hz) 起，再快就成噪声 */
static volatile int s_arp_n = 0;      /* 当前和弦音数；<2 时定时器不动作 */
static volatile uint8_t s_arp_notes[32];
static int  s_arp_idx = 0;

/* UI */
static lv_obj_t *s_lbl_file;
static lv_obj_t *s_lbl_note;
static lv_obj_t *s_lbl_status;
static lv_obj_t *s_lbl_hint;
static lv_obj_t *s_lbl_mode;
static lv_obj_t *s_bar;

/* ---------------- 工具 ---------------- */
static uint32_t be32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static uint16_t be16(const uint8_t *p) { return (uint16_t)(((uint16_t)p[0] << 8) | p[1]); }

static uint32_t note_to_freq(int n)
{
    double f = 440.0 * pow(2.0, (n - 69) / 12.0);
    if (f < 60.0) f = 60.0;
    if (f > 12000.0) f = 12000.0;
    return (uint32_t)(f + 0.5);
}

static void note_to_name(int n, char *buf, size_t sz)
{
    static const char *NN[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    if (sz == 0) return;
    if (n < 0) { buf[0] = '-'; if (sz > 1) buf[1] = '-'; if (sz > 2) buf[2] = 0; else buf[sz - 1] = 0; return; }
    int pc = n % 12;
    int oct = n / 12 - 1;
    char num[16];
    snprintf(num, sizeof(num), "%d", oct);
    size_t k = 0;
    if (k < sz - 1) buf[k++] = NN[pc][0];
    if (NN[pc][1] && k < sz - 1) buf[k++] = NN[pc][1];
    for (size_t i = 0; num[i] && k < sz - 1; ++i) buf[k++] = num[i];
    buf[k] = 0;
}

/* ---------------- 设置持久化（NVS：和弦开关 + 琶音档位） ---------------- */
#define NVS_NS "midi"

static void settings_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    int32_t v = 0;
    if (nvs_get_i32(h, "chord", &v) == ESP_OK) s_chord_on = v ? 1 : 0;
    if (nvs_get_i32(h, "arpms", &v) == ESP_OK) s_arp_ms = (int)v;
    nvs_close(h);
    if (s_arp_ms < ARP_MS_MIN) s_arp_ms = ARP_MS_MIN;   /* 旧档位/异常值一律抬到下限 */
}

static void settings_save(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_i32(h, "chord", s_chord_on);
    nvs_set_i32(h, "arpms", s_arp_ms);
    nvs_commit(h);
    nvs_close(h);
}

/* ---------------- 蜂鸣器 ---------------- */
static void buz_init(void)
{
    ledc_timer_config_t tc = {
        .speed_mode = BUZZER_LEDC_MODE,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .timer_num = BUZZER_LEDC_TIMER,
        .freq_hz = 880,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    if (ledc_timer_config(&tc) != ESP_OK) { ESP_LOGW(TAG, "buzzer timer fail"); return; }

    ledc_channel_config_t cc = {
        .gpio_num = PIN_NUM_BUZZER,
        .speed_mode = BUZZER_LEDC_MODE,
        .channel = BUZZER_LEDC_CHANNEL,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = BUZZER_LEDC_TIMER,
        .duty = 0,
        .hpoint = 0,
        .sleep_mode = LEDC_SLEEP_MODE_NO_ALIVE_NO_PD,
    };
    if (ledc_channel_config(&cc) == ESP_OK) s_buz_ready = true;
    else ESP_LOGW(TAG, "buzzer channel fail");
}

static void buz_tone(uint32_t freq)
{
    if (!s_buz_ready) return;
    if (freq < 60) freq = 60;
    if (freq > 12000) freq = 12000;
    ledc_set_freq(BUZZER_LEDC_MODE, BUZZER_LEDC_TIMER, freq);
    ledc_set_duty(BUZZER_LEDC_MODE, BUZZER_LEDC_CHANNEL, BUZZER_DUTY);
    ledc_update_duty(BUZZER_LEDC_MODE, BUZZER_LEDC_CHANNEL);
}

static void buz_off(void)
{
    if (!s_buz_ready) return;
    ledc_set_duty(BUZZER_LEDC_MODE, BUZZER_LEDC_CHANNEL, 0);
    ledc_update_duty(BUZZER_LEDC_MODE, BUZZER_LEDC_CHANNEL);
}

/*
 * 和弦换音定时器（1ms 周期）。
 * 放在独立 esp_timer 里而不是 LVGL 主循环里，是因为主循环最长睡 10ms，
 * 原来那样最快也只能 25ms 换一次音（≈40Hz），听起来是分解和弦而不是和弦。
 */
static void arp_timer_cb(void *arg)
{
    (void)arg;
    static uint32_t acc = 0;
    int n = s_arp_n;
    if (!s_playing || s_paused || !s_chord_on || n < 2) { acc = 0; return; }
    if (++acc < (uint32_t)s_arp_ms) return;
    acc = 0;
    s_arp_idx = (s_arp_idx + 1) % n;
    if (s_arp_idx < 0) s_arp_idx = 0;
    int note = s_arp_notes[s_arp_idx];
    uint32_t f = note_to_freq(note);
    ledc_set_freq(BUZZER_LEDC_MODE, BUZZER_LEDC_TIMER, f);
    ledc_set_duty(BUZZER_LEDC_MODE, BUZZER_LEDC_CHANNEL, BUZZER_DUTY);
    ledc_update_duty(BUZZER_LEDC_MODE, BUZZER_LEDC_CHANNEL);
    s_sounding = note;
    s_cur_freq = f;
    s_cur_note = note;
}

/* ---------------- 按键扫描（去抖，返回当前稳定按下的键下标，-1=无） ---------------- */
static int s_last_idx = -1;
static uint32_t s_last_change = 0;
static int s_stable_idx = -1;

static int keys_scan(void)
{
    int idx = -1;
    uint32_t now = lv_tick_get();
    for (size_t i = 0; i < sizeof(s_btns) / sizeof(s_btns[0]); ++i) {
        if (gpio_get_level(s_btns[i].gpio) == BUTTON_ACTIVE_LEVEL) { idx = (int)i; break; }
    }
    if (idx != s_last_idx) { s_last_idx = idx; s_last_change = now; }
    if ((now - s_last_change) >= BUTTON_DEBOUNCE_MS) s_stable_idx = idx;
    return s_stable_idx;
}

static void buttons_init(void)
{
    uint64_t mask = 0, pull = 0;
    for (size_t i = 0; i < sizeof(s_btns) / sizeof(s_btns[0]); ++i) {
        mask |= 1ULL << s_btns[i].gpio;
        if (s_btns[i].gpio != GPIO_NUM_34 && s_btns[i].gpio != GPIO_NUM_35) pull |= 1ULL << s_btns[i].gpio;
    }
    gpio_config_t io = { .pin_bit_mask = mask, .mode = GPIO_MODE_INPUT,
                         .pull_up_en = GPIO_PULLUP_DISABLE, .pull_down_en = GPIO_PULLDOWN_DISABLE,
                         .intr_type = GPIO_INTR_DISABLE };
    ESP_ERROR_CHECK(gpio_config(&io));
    gpio_config_t iop = { .pin_bit_mask = pull, .mode = GPIO_MODE_INPUT,
                          .pull_up_en = GPIO_PULLUP_ENABLE, .pull_down_en = GPIO_PULLDOWN_DISABLE,
                          .intr_type = GPIO_INTR_DISABLE };
    ESP_ERROR_CHECK(gpio_config(&iop));
}

/* ---------------- SD 挂载（SPI2 与屏幕共用） ---------------- */
static void sd_mount(void)
{
    if (s_sd_mounted) return;
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = LCD_HOST;
    host.max_freq_khz = SD_SPI_MAX_FREQ_KHZ;

    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.host_id = LCD_HOST;
    slot.gpio_cs = PIN_NUM_SD_CS;
    slot.wait_for_miso = 20;

    esp_vfs_fat_mount_config_t mc = VFS_FAT_MOUNT_DEFAULT_CONFIG();
    mc.format_if_mount_failed = false;
    mc.max_files = 4;

    esp_err_t err = esp_vfs_fat_sdspi_mount("/sdcard", &host, &slot, &mc, &s_sd_card);
    if (err == ESP_OK && s_sd_card) {
        s_sd_mounted = true;
        ESP_LOGI(TAG, "SD mounted: %s", s_sd_card->cid.name);
    } else {
        s_sd_mounted = false;
        s_sd_card = NULL;
        ESP_LOGW(TAG, "SD mount failed: %s", esp_err_to_name(err));
    }
}

/* ---------------- MIDI 解析 ---------------- */
static int ev_cmp(const void *a, const void *b)
{
    const midi_ev_t *x = a, *y = b;
    if (x->t_ms < y->t_ms) return -1;
    if (x->t_ms > y->t_ms) return 1;
    return 0;
}

static void add_ev(uint32_t t, uint8_t note, uint8_t vel)
{
    if (!s_ev || s_ev_count >= MIDI_MAX_EVENTS) return;
    s_ev[s_ev_count].t_ms = t;
    s_ev[s_ev_count].note = note;
    s_ev[s_ev_count].vel = vel;
    s_ev_count++;
}

static int parse_midi_buf(const uint8_t *b, size_t len)
{
    s_ev_count = 0;
    if (len < 14 || memcmp(b, "MThd", 4) != 0) return -1;
    uint32_t hlen = be32(b + 4);
    if (hlen < 6) return -1;
    uint16_t ntrk = be16(b + 10);
    uint16_t div = be16(b + 12);
    if (div == 0 || (div & 0x8000)) return -1;   /* 不支持 SMPTE 时基 */

    size_t pos = 8 + hlen;
    uint32_t tempo_us = 500000;                  /* 默认 120 BPM */

    for (uint16_t t = 0; t < ntrk && pos + 8 <= len; ++t) {
        if (memcmp(b + pos, "MTrk", 4) != 0) break;
        uint32_t tlen = be32(b + pos + 4);
        size_t p = pos + 8;
        size_t end = p + tlen;
        if (end > len) end = len;

        uint32_t abst = 0;
        uint8_t running = 0;
        while (p < end) {
            /* delta time (VLQ) */
            uint32_t delta = 0;
            int guard = 0;
            while (p < end && guard++ < 5) {
                uint8_t c = b[p++];
                delta = (delta << 7) | (c & 0x7F);
                if (!(c & 0x80)) break;
            }
            abst += delta;
            if (p >= end) break;

            uint8_t st = b[p];
            if (st < 0x80) {
                st = running;
                if (!st) break;
            } else {
                p++;
                running = st;
            }

            if (st == 0xFF) {                    /* meta */
                if (p >= end) break;
                uint8_t mt = b[p++];
                uint32_t ml = 0;
                guard = 0;
                while (p < end && guard++ < 5) {
                    uint8_t c = b[p++];
                    ml = (ml << 7) | (c & 0x7F);
                    if (!(c & 0x80)) break;
                }
                if (mt == 0x51 && ml == 3 && p + 3 <= end) {
                    tempo_us = ((uint32_t)b[p] << 16) | ((uint32_t)b[p + 1] << 8) | b[p + 2];
                }
                p += ml;
                running = 0;
                continue;
            }
            if (st == 0xF0 || st == 0xF7) {      /* sysex */
                uint32_t ml = 0;
                guard = 0;
                while (p < end && guard++ < 5) {
                    uint8_t c = b[p++];
                    ml = (ml << 7) | (c & 0x7F);
                    if (!(c & 0x80)) break;
                }
                p += ml;
                running = 0;
                continue;
            }

            uint8_t type = st & 0xF0;
            if (type == 0x80 || type == 0x90) {
                if (p + 2 > end) break;
                uint8_t note = b[p++];
                uint8_t vel = b[p++];
                uint32_t ms = (uint32_t)(((uint64_t)abst * tempo_us) / ((uint64_t)div * 1000ULL));
                bool on = (type == 0x90) && (vel > 0);
                add_ev(ms, note, on ? vel : 0);
            } else if (type == 0xA0 || type == 0xB0 || type == 0xE0) {
                p += 2;
            } else if (type == 0xC0 || type == 0xD0) {
                p += 1;
            } else {
                /* 未知/系统消息，尝试前进 1 字节避免死循环 */
                p += 1;
            }
        }
        pos = end;
    }

    qsort(s_ev, s_ev_count, sizeof(s_ev[0]), ev_cmp);
    return s_ev_count;
}

static int load_midi_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) { ESP_LOGW(TAG, "open fail %s", path); return -1; }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    long sz = ftell(f);
    if (sz <= 0 || sz > MIDI_MAX_FILE_SIZE) { fclose(f); ESP_LOGW(TAG, "bad size %ld", sz); return -1; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return -1; }
    uint8_t *buf = malloc((size_t)sz);
    if (!buf) { fclose(f); return -1; }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    int n = (rd == (size_t)sz) ? parse_midi_buf(buf, (size_t)sz) : -1;
    free(buf);
    return n;
}

/*
 * 内置 DEMO：FamilyMart 入店音（Panasonic EC-5347 Melody Sign）
 * 源自 "Japanese Family Mart 入店音 Entrance Sound_Jingle ... .mid"
 *   SMF format 0 / 1 track("Synth Bass") / division 384 / tempo 714285us/qn(≈84BPM) / 全长 5357ms
 * 这里**原样嵌入该文件解析出的全部 40 个事件**（不再做单音归约，避免丢音）：
 *   vel>0 = note on，vel==0 = note off。和弦（同时按下的多个音）由播放器做快速琶音近似。
 */
static const midi_ev_t k_demo[] = {
    {    0, 79, 50 }, {  357, 79,  0 }, {  357, 75, 50 }, {  714, 75,  0 },
    {  714, 70, 50 }, {  714, 67, 50 }, {  714, 70, 50 }, { 1071, 70,  0 },
    { 1071, 75, 50 }, { 1428, 75,  0 }, { 1428, 70,  0 }, { 1428, 67,  0 },
    { 1428, 77, 50 }, { 1428, 74, 50 }, { 1428, 77, 50 }, { 1785, 77,  0 },
    { 1785, 82, 50 }, { 2321, 77,  0 }, { 2321, 74,  0 }, { 2499, 82,  0 },
    { 2499, 65, 50 }, { 2857, 65,  0 }, { 2857, 77, 50 }, { 2857, 70, 50 },
    { 3214, 77,  0 }, { 3214, 79, 50 }, { 3571, 79,  0 }, { 3571, 70,  0 },
    { 3571, 77, 50 }, { 3571, 70, 50 }, { 3928, 70,  0 }, { 3928, 77,  0 },
    { 3928, 70, 50 }, { 3928, 70, 50 }, { 4285, 70,  0 }, { 4285, 70,  0 },
    { 4285, 67, 50 }, { 4285, 75, 50 }, { 5357, 75,  0 }, { 5357, 67,  0 },
};

static void build_demo(void)
{
    s_ev_count = 0;
    for (size_t i = 0; i < sizeof(k_demo) / sizeof(k_demo[0]); ++i) {
        add_ev(k_demo[i].t_ms, k_demo[i].note, k_demo[i].vel);
    }
}

/* ---------------- 曲目列表 ---------------- */
static void scan_playlist(void)
{
    s_track_count = 0;
    if (s_sd_mounted) {
        DIR *d = opendir("/sdcard");
        if (d) {
            struct dirent *e;
            while ((e = readdir(d)) != NULL && s_track_count < MAX_TRACKS - 1) {
                const char *n = e->d_name;
                size_t l = strlen(n);
                if (l > 4 && strcasecmp(n + l - 4, ".mid") == 0) {
                    strncpy(s_tracks[s_track_count].name, n, sizeof(s_tracks[0].name) - 1);
                    s_tracks[s_track_count].name[sizeof(s_tracks[0].name) - 1] = 0;
                    s_tracks[s_track_count].demo = false;
                    s_track_count++;
                }
            }
            closedir(d);
        }
    }
    if (s_track_count < MAX_TRACKS) {
        strcpy(s_tracks[s_track_count].name, "[DEMO] FamilyMart");
        s_tracks[s_track_count].demo = true;
        s_track_count++;
    }
    if (s_track_sel >= s_track_count) s_track_sel = 0;
}

static int load_track(int idx)
{
    if (idx < 0 || idx >= s_track_count) return -1;
    if (s_tracks[idx].demo) {
        build_demo();
        return s_ev_count;
    }
    char path[96];
    snprintf(path, sizeof(path), "/sdcard/%s", s_tracks[idx].name);
    return load_midi_file(path);
}

/* ---------------- 播放控制 ---------------- */
static void play_start(void)
{
    if (load_track(s_track_sel) <= 0) {
        ESP_LOGW(TAG, "track load failed");
        s_playing = false;
        return;
    }
    s_ev_i = 0;
    s_cur_note = -1;
    s_sounding = -1;
    s_arp_idx = -1;
    s_arp_n = 0;
    s_chord_n = 0;
    memset(s_note_active, 0, sizeof(s_note_active));
    s_base_elapsed = 0;
    s_play_base = lv_tick_get();
    s_paused = false;
    s_playing = true;
}

static void play_stop(void)
{
    s_playing = false;
    s_paused = false;
    s_ev_i = 0;
    s_base_elapsed = 0;
    s_cur_note = -1;
    s_sounding = -1;
    s_chord_n = 0;
    s_arp_n = 0;
    memset(s_note_active, 0, sizeof(s_note_active));
    buz_off();
}

static void play_toggle_pause(void)
{
    if (!s_playing) { play_start(); return; }
    if (!s_paused) {
        s_base_elapsed = s_base_elapsed + (lv_tick_get() - s_play_base);
        s_paused = true;
        buz_off();
    } else {
        s_play_base = lv_tick_get();
        s_paused = false;
    }
}

static void select_track(int delta)
{
    if (s_track_count <= 0) return;
    s_track_sel = (s_track_sel + delta + s_track_count) % s_track_count;
    if (s_playing || s_paused) play_start();   /* 选曲时直接切歌播放 */
}

static void midi_service(void)
{
    if (!s_playing || s_paused) return;

    uint32_t elapsed = s_base_elapsed + (lv_tick_get() - s_play_base);
    while (s_ev_i < s_ev_count && s_ev[s_ev_i].t_ms <= elapsed) {
        midi_ev_t *e = &s_ev[s_ev_i];
        if (e->vel > 0) {
            if (s_note_active[e->note] < 255) s_note_active[e->note]++;
        } else if (s_note_active[e->note] > 0) {
            s_note_active[e->note]--;
        }
        s_ev_i++;
    }
    if (s_ev_i >= s_ev_count) {
        s_playing = false;
        s_paused = false;
        memset(s_note_active, 0, sizeof(s_note_active));
        s_sounding = -1;
        s_cur_note = -1;
        s_chord_n = 0;
        buz_off();
        return;
    }

    /* 收集当前仍按下的不同音高 */
    int notes[32];
    int n = 0;
    for (int i = 0; i < 128 && n < 32; ++i) {
        if (s_note_active[i]) notes[n++] = i;
    }
    s_chord_n = n;

    if (n == 0) {                                  /* 全松 -> 静音 */
        s_arp_n = 0;
        if (s_sounding >= 0) { buz_off(); s_sounding = -1; }
        s_cur_note = -1;
        return;
    }

    if (n == 1 || !s_chord_on) {
        /* 只有一个音，或者用户切成单音模式：直接发最高音声部（旋律最清楚） */
        int pick = notes[n - 1];
        s_arp_n = 0;
        if (s_sounding != pick) {
            s_sounding = pick;
            s_cur_freq = note_to_freq(pick);
            buz_tone(s_cur_freq);
        }
        s_cur_note = pick;
        return;
    }

    /* 和弦：把音高表交给 1ms 定时器快速轮换（见 arp_timer_cb） */
    if (s_arp_n != n) s_arp_idx = -1;             /* 音数变了就重新起拍 */
    for (int i = 0; i < n; ++i) s_arp_notes[i] = (uint8_t)notes[i];
    s_arp_n = n;
    s_cur_note = s_sounding;
}

/* ---------------- LCD / LVGL 底层（与官方固件一致） ---------------- */
static esp_lcd_panel_io_handle_t s_lcd_io = NULL;
static bool s_lcd_on = false;
static volatile bool s_first_flush_done = false;
static lv_draw_buf_t s_draw_buf3;

static bool flush_ready_cb(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *ed, void *ctx)
{
    (void)io; (void)ed;
    s_first_flush_done = true;
    lv_display_flush_ready((lv_display_t *)ctx);
    return false;
}

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px)
{
    esp_lcd_panel_io_handle_t io = lv_display_get_user_data(disp);
    int w = area->x2 - area->x1 + 1;
    int h = area->y2 - area->y1 + 1;
    uint16_t xs = area->x1 + LCD_X_GAP, xe = area->x2 + LCD_X_GAP;
    uint16_t ys = area->y1 + LCD_Y_GAP, ye = area->y2 + LCD_Y_GAP;
    uint8_t caset[] = {xs >> 8, xs & 0xFF, xe >> 8, xe & 0xFF};
    uint8_t raset[] = {ys >> 8, ys & 0xFF, ye >> 8, ye & 0xFF};
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io, ST7735_CASET, caset, sizeof(caset)));
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io, ST7735_RASET, raset, sizeof(raset)));
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_color(io, ST7735_RAMWR, px, w * h * sizeof(uint16_t)));
}

static void lvgl_tick_cb(void *arg) { (void)arg; lv_tick_inc(LVGL_TICK_PERIOD_MS); }

static void st_tx(esp_lcd_panel_io_handle_t io, int cmd, const void *p, size_t n) { ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io, cmd, p, n)); }
static void st_delay(uint32_t ms) { vTaskDelay(pdMS_TO_TICKS(ms)); }

static void st_clear_black(esp_lcd_panel_io_handle_t io)
{
    static uint16_t line[LCD_H_RES * 8];
    const uint8_t caset[] = {0x00, 0x00, 0x00, LCD_H_RES - 1};
    memset(line, 0, sizeof(line));
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io, ST7735_CASET, caset, sizeof(caset)));
    for (uint16_t y = 0; y < LCD_V_RES; y += 8) {
        uint16_t y2 = (y + 7 < LCD_V_RES) ? (y + 7) : (LCD_V_RES - 1);
        const uint8_t raset[] = {y >> 8, y & 0xFF, y2 >> 8, y2 & 0xFF};
        ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io, ST7735_RASET, raset, sizeof(raset)));
        ESP_ERROR_CHECK(esp_lcd_panel_io_tx_color(io, ST7735_RAMWR, line, (y2 - y + 1) * LCD_H_RES * sizeof(uint16_t)));
    }
}

static void st7735_init(esp_lcd_panel_io_handle_t io)
{
    const uint8_t frmctr[] = {0x01, 0x2C, 0x2D};
    const uint8_t frmctr3[] = {0x01, 0x2C, 0x2D, 0x01, 0x2C, 0x2D};
    const uint8_t invctr[] = {0x07};
    const uint8_t pwctr1[] = {0xA2, 0x02, 0x84};
    const uint8_t pwctr2[] = {0xC5};
    const uint8_t pwctr3[] = {0x0A, 0x00};
    const uint8_t pwctr4[] = {0x8A, 0x2A};
    const uint8_t pwctr5[] = {0x8A, 0xEE};
    const uint8_t vmctr1[] = {0x0E};
    const uint8_t madctl_default[] = {MADCTL_MX | MADCTL_MY | MADCTL_RGB};
    const uint8_t colmod[] = {0x05};
    const uint8_t caset[] = {0x00, 0x00, 0x00, LCD_NATIVE_H_RES - 1};
    const uint8_t raset[] = {0x00, 0x00, 0x00, LCD_NATIVE_V_RES - 1};
    const uint8_t gp[] = {0x02,0x1C,0x07,0x12,0x37,0x32,0x29,0x2D,0x29,0x25,0x2B,0x39,0x00,0x01,0x03,0x10};
    const uint8_t gn[] = {0x03,0x1D,0x07,0x06,0x2E,0x2C,0x29,0x2D,0x2E,0x2E,0x37,0x3F,0x00,0x00,0x02,0x10};
    const uint8_t madctl_rot90[] = {MADCTL_MX | MADCTL_MV | MADCTL_RGB};

    ESP_LOGI(TAG, "ST7735 init (MicroPython init(2) compatible)");
    st_tx(io, ST7735_DISPOFF, NULL, 0);
    st_tx(io, ST7735_SWRESET, NULL, 0);
    st_delay(150);
    st_tx(io, ST7735_SLPOUT, NULL, 0);
    st_delay(500);
    st_tx(io, ST7735_FRMCTR1, frmctr, sizeof(frmctr));
    st_tx(io, ST7735_FRMCTR2, frmctr, sizeof(frmctr));
    st_tx(io, ST7735_FRMCTR3, frmctr3, sizeof(frmctr3));
    st_tx(io, ST7735_INVCTR, invctr, sizeof(invctr));
    st_tx(io, ST7735_PWCTR1, pwctr1, sizeof(pwctr1));
    st_tx(io, ST7735_PWCTR2, pwctr2, sizeof(pwctr2));
    st_tx(io, ST7735_PWCTR3, pwctr3, sizeof(pwctr3));
    st_tx(io, ST7735_PWCTR4, pwctr4, sizeof(pwctr4));
    st_tx(io, ST7735_PWCTR5, pwctr5, sizeof(pwctr5));
    st_tx(io, ST7735_VMCTR1, vmctr1, sizeof(vmctr1));
    st_tx(io, ST7735_INVOFF, NULL, 0);
    st_tx(io, ST7735_MADCTL, madctl_default, sizeof(madctl_default));
    st_tx(io, ST7735_COLMOD, colmod, sizeof(colmod));
    st_tx(io, ST7735_CASET, caset, sizeof(caset));
    st_tx(io, ST7735_RASET, raset, sizeof(raset));
    st_tx(io, ST7735_GMCTRP1, gp, sizeof(gp));
    st_tx(io, ST7735_GMCTRN1, gn, sizeof(gn));
    st_tx(io, ST7735_NORON, NULL, 0);
    st_delay(10);
    st_tx(io, ST7735_MADCTL, madctl_rot90, sizeof(madctl_rot90));
    st_clear_black(io);
}

static void lcd_display_on(void)
{
    if (s_lcd_on || !s_lcd_io) return;
    st_tx(s_lcd_io, ST7735_DISPON, NULL, 0);
    st_delay(20);
    s_lcd_on = true;
}

static esp_lcd_panel_io_handle_t lcd_init(void)
{
    ESP_LOGI(TAG, "Init SPI bus for ST7735 TFT");
    spi_bus_config_t buscfg = {
        .sclk_io_num = PIN_NUM_LCD_SCLK,
        .mosi_io_num = PIN_NUM_LCD_MOSI,
        .miso_io_num = PIN_NUM_LCD_MISO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_H_RES * LCD_DRAW_BUF_LINES * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO));

    ESP_LOGI(TAG, "Install panel IO");
    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_spi_config_t ioc = {
        .dc_gpio_num = PIN_NUM_LCD_DC,
        .cs_gpio_num = PIN_NUM_LCD_CS,
        .pclk_hz = LCD_PIXEL_CLOCK_HZ,
        .lcd_cmd_bits = LCD_CMD_BITS,
        .lcd_param_bits = LCD_PARAM_BITS,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &ioc, &io));
    s_lcd_io = io;
    st7735_init(io);
    return io;
}

static lv_display_t *lvgl_display_init(esp_lcd_panel_io_handle_t io)
{
    lv_display_t *disp = lv_display_create(LCD_H_RES, LCD_V_RES);
    assert(disp);
    const lv_color_format_t cf = LV_COLOR_FORMAT_RGB565_SWAPPED;
    const uint32_t stride = lv_draw_buf_width_to_stride(LCD_H_RES, cf);
    const size_t buf_sz = stride * LCD_DRAW_BUF_LINES;
    void *b1 = spi_bus_dma_memory_alloc(LCD_HOST, buf_sz, 0);
    void *b2 = spi_bus_dma_memory_alloc(LCD_HOST, buf_sz, 0);
    void *b3 = spi_bus_dma_memory_alloc(LCD_HOST, buf_sz, 0);
    assert(b1 && b2 && b3);

    lv_display_set_color_format(disp, cf);
    lv_display_set_dpi(disp, LCD_DPI);
    lv_display_set_buffers(disp, b1, b2, buf_sz, LV_DISPLAY_RENDER_MODE_FULL);
    lv_draw_buf_init(&s_draw_buf3, LCD_H_RES, LCD_DRAW_BUF_LINES, cf, stride, b3, buf_sz);
    lv_display_set_3rd_draw_buffer(disp, &s_draw_buf3);
    lv_display_set_user_data(disp, io);
    lv_display_set_flush_cb(disp, flush_cb);
    ESP_LOGI(TAG, "LVGL display 160x128, 3 full-screen DMA buffers, SPI %d MHz", LCD_PIXEL_CLOCK_HZ / 1000000);
    return disp;
}

/* ---------------- UI ---------------- */
static const uint32_t C_BG = 0x10161C;
static const uint32_t C_PANEL = 0x1B2530;
static const uint32_t C_ACCENT = 0x36C7A5;
static const uint32_t C_CREAM = 0xFFE9A8;
static const uint32_t C_DIM = 0x8FA3B5;

static lv_obj_t *mk_label(lv_obj_t *parent, const char *txt, int x, int y, uint32_t color, const lv_font_t *font)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, txt);
    lv_obj_set_pos(l, x, y);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    if (font) lv_obj_set_style_text_font(l, font, 0);
    return l;
}

static void ui_create(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *hdr = lv_obj_create(scr);
    lv_obj_remove_style_all(hdr);
    lv_obj_set_pos(hdr, 0, 0);
    lv_obj_set_size(hdr, LCD_H_RES, 18);
    lv_obj_set_style_bg_color(hdr, lv_color_hex(C_PANEL), 0);
    lv_obj_set_style_bg_opa(hdr, LV_OPA_COVER, 0);
    lv_obj_remove_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *t = mk_label(hdr, "MIDI PLAYER", 6, 1, C_ACCENT, &lv_font_montserrat_14);
    (void)t;

    s_lbl_file   = mk_label(scr, "[DEMO] FamilyMart", 6, 22, C_CREAM, &lv_font_montserrat_12);
    lv_obj_set_width(s_lbl_file, 106);
    lv_label_set_long_mode(s_lbl_file, LV_LABEL_LONG_MODE_CLIP);
    s_lbl_note   = mk_label(scr, "--  ---Hz", 6, 40, 0xFFFFFF, &lv_font_montserrat_14);
    s_lbl_status = mk_label(scr, "STOP", 118, 22, C_DIM, &lv_font_montserrat_12);

    s_bar = lv_bar_create(scr);
    lv_obj_set_pos(s_bar, 6, 62);
    lv_obj_set_size(s_bar, LCD_H_RES - 12, 8);
    lv_bar_set_range(s_bar, 0, 100);
    lv_bar_set_value(s_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_bar, lv_color_hex(C_PANEL), 0);
    lv_obj_set_style_bg_color(s_bar, lv_color_hex(C_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_bar, 3, 0);
    lv_obj_set_style_radius(s_bar, 3, LV_PART_INDICATOR);

    s_lbl_hint = mk_label(scr, "U/D track   A play/pause\nB stop     L/R prev/next", 6, 74, C_DIM, &lv_font_montserrat_10);
    lv_obj_set_width(s_lbl_hint, LCD_H_RES - 12);

    s_lbl_mode = mk_label(scr, "hold A: CHORD\nhold B: 4ms", 6, 100, C_ACCENT, &lv_font_montserrat_10);
}

static void ui_refresh(void)
{
    char buf[96];

    if (s_track_count > 0) {
        snprintf(buf, sizeof(buf), "%d/%d %s", s_track_sel + 1, s_track_count, s_tracks[s_track_sel].name);
        lv_label_set_text(s_lbl_file, buf);
    }

    if (s_cur_note >= 0) {
        char nm[8];
        note_to_name(s_cur_note, nm, sizeof(nm));
        if (s_chord_n > 1) snprintf(buf, sizeof(buf), "%s %luHz CH%d", nm, (unsigned long)s_cur_freq, s_chord_n);
        else               snprintf(buf, sizeof(buf), "%s  %luHz", nm, (unsigned long)s_cur_freq);
    } else {
        snprintf(buf, sizeof(buf), "--  ---Hz");
    }
    lv_label_set_text(s_lbl_note, buf);

    const char *st = s_playing ? (s_paused ? "PAUSE" : "PLAY") : "STOP";
    lv_label_set_text(s_lbl_status, st);
    lv_obj_set_style_text_color(s_lbl_status,
                                lv_color_hex(s_playing ? (s_paused ? C_CREAM : C_ACCENT) : C_DIM), 0);

    if (s_chord_on) snprintf(buf, sizeof(buf), "hold A: CHORD\nhold B: %dms", s_arp_ms);
    else            snprintf(buf, sizeof(buf), "hold A: MONO\nhold B: %dms", s_arp_ms);
    lv_label_set_text(s_lbl_mode, buf);

    int pct = 0;
    if (s_ev_count > 0) pct = (s_ev_i * 100) / s_ev_count;
    lv_bar_set_value(s_bar, pct, LV_ANIM_OFF);
}

/* ---------------- 主循环 ---------------- */
#define KEY_LONG_MS 600

static void handle_key_short(key_id_t k)
{
    switch (k) {
    case KEY_UP:    select_track(-1); break;
    case KEY_DOWN:  select_track(+1); break;
    case KEY_LEFT:  select_track(-1); break;
    case KEY_RIGHT: select_track(+1); break;
    case KEY_A:     play_toggle_pause(); break;
    case KEY_B:     play_stop(); break;
    default: break;
    }
}

/* 琶音档位：全部 >= 16ms（<16ms 听感破碎，已去掉 2/4/8ms 档） */
static const int k_arp_speeds[] = {16, 24, 32, 48, 64, 96};
static int s_arp_sp_i = 0;

static void handle_key_long(key_id_t k)
{
    switch (k) {
    case KEY_A:
        s_chord_on = !s_chord_on;
        ESP_LOGI(TAG, "mode: %s", s_chord_on ? "CHORD(arp)" : "MONO(top voice)");
        settings_save();
        break;
    case KEY_B:
        s_arp_sp_i = (s_arp_sp_i + 1) % (int)(sizeof(k_arp_speeds) / sizeof(k_arp_speeds[0]));
        s_arp_ms = k_arp_speeds[s_arp_sp_i];
        ESP_LOGI(TAG, "arp interval: %d ms (%d Hz)", s_arp_ms, 1000 / s_arp_ms);
        settings_save();
        break;
    default: break;
    }
}

static void app_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "Start MIDI player UI");
    ui_create();
    s_first_flush_done = false;
    lv_refr_now(NULL);
    for (int i = 0; i < 100 && !s_first_flush_done; ++i) vTaskDelay(pdMS_TO_TICKS(1));
    lcd_display_on();

    uint32_t last_ui = 0;
    int  held = -1;
    uint32_t down_ms = 0;
    bool long_done = false;

    while (true) {
        int cur = keys_scan();
        if (cur != held) {
            if (held >= 0 && !long_done) handle_key_short(s_btns[held].id);
            held = cur;
            down_ms = lv_tick_get();
            long_done = false;
        } else if (cur >= 0 && !long_done && (lv_tick_get() - down_ms) >= KEY_LONG_MS) {
            long_done = true;
            handle_key_long(s_btns[cur].id);
        }

        midi_service();

        if (lv_tick_elaps(last_ui) >= 40) {
            last_ui = lv_tick_get();
            ui_refresh();
        }

        uint32_t d = lv_timer_handler();
        if (d < LVGL_TASK_MIN_DELAY_MS) d = LVGL_TASK_MIN_DELAY_MS;
        if (d > LVGL_TASK_MAX_DELAY_MS) d = LVGL_TASK_MAX_DELAY_MS;
        usleep(d * 1000);
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "Xiaomiao MIDI player boot");

    buttons_init();
    buz_init();

    /* 取回上次的长按设置（和弦开关 / 琶音档位） */
    esp_err_t nvserr = nvs_flash_init();
    if (nvserr == ESP_ERR_NVS_NO_FREE_PAGES || nvserr == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvserr = nvs_flash_init();
    }
    if (nvserr == ESP_OK) {
        settings_load();
        ESP_LOGI(TAG, "settings: mode=%s arp=%dms", s_chord_on ? "CHORD" : "MONO", s_arp_ms);
    } else {
        ESP_LOGW(TAG, "nvs init failed (%s), using defaults", esp_err_to_name(nvserr));
    }

    /* MIDI 事件表放 PSRAM（4MB 可用），不占内部 DRAM/DMA 区 */
    s_ev = heap_caps_malloc(MIDI_MAX_EVENTS * sizeof(midi_ev_t), MALLOC_CAP_SPIRAM);
    if (!s_ev) s_ev = heap_caps_malloc(MIDI_MAX_EVENTS * sizeof(midi_ev_t), MALLOC_CAP_8BIT);
    if (!s_ev) { ESP_LOGE(TAG, "no memory for MIDI event table"); }
    else ESP_LOGI(TAG, "MIDI event table in %s", "PSRAM");

    esp_lcd_panel_io_handle_t io = lcd_init();

    /* 先建 LVGL 显示缓冲（需要内部 DMA 内存），再挂 SD，避免 SD 先吃掉 DMA 内存 */
    lv_init();
    lv_display_t *disp = lvgl_display_init(io);

    esp_lcd_panel_io_callbacks_t cbs = { .on_color_trans_done = flush_ready_cb };
    ESP_ERROR_CHECK(esp_lcd_panel_io_register_event_callbacks(io, &cbs, disp));

    const esp_timer_create_args_t args = { .callback = lvgl_tick_cb, .name = "lvgl_tick" };
    esp_timer_handle_t tt = NULL;
    ESP_ERROR_CHECK(esp_timer_create(&args, &tt));
    ESP_ERROR_CHECK(esp_timer_start_periodic(tt, LVGL_TICK_PERIOD_MS * 1000));

    /* 和弦换音定时器：1ms 周期（LVGL 主循环最长睡 10ms，扛不住快速换音） */
    const esp_timer_create_args_t arp_args = { .callback = arp_timer_cb, .name = "arp" };
    esp_timer_handle_t arp_timer = NULL;
    ESP_ERROR_CHECK(esp_timer_create(&arp_args, &arp_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(arp_timer, 1000));

    sd_mount();
    scan_playlist();

    /* 开机提示音（C5-E5-G5），直接确认蜂鸣器工作 */
    {
        const uint32_t ch[] = {523, 659, 784};
        for (int i = 0; i < 3; ++i) { buz_tone(ch[i]); vTaskDelay(pdMS_TO_TICKS(120)); }
        buz_off();
    }

    BaseType_t ok = xTaskCreate(app_task, "midi", LVGL_TASK_STACK_SIZE, NULL, LVGL_TASK_PRIORITY, NULL);
    ESP_ERROR_CHECK(ok == pdPASS ? ESP_OK : ESP_FAIL);
}
