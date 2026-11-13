/*
 * XiaoMiao ESP32-WROVER-B — 节拍器 / 调音器(规划) 固件
 *
 * 硬件：ST7735 160x128(旋转90) / GPIO14 无源蜂鸣器(LEDC) / 6 键
 *   UP=2 DOWN=13 LEFT=27 RIGHT=35 A=34 B=12（按下为低）
 *   板载 LED1/LED2 由 GD32F350 经 I2C(0x40, A0/A1) 控制
 *
 * 拍点引擎：1ms esp_timer + 微秒相位累加（不累积漂移，高 BPM 时每毫秒可出多个拍）
 *   beat_us = 60_000_000 / BPM；gap_us = beat_us / 细分
 *
 * 操作：
 *   A        开始/停止        长按 A = 设置页
 *   B        打拍定速(tap)    长按 B = 切到调音器页（预留）
 *   U/D      BPM -1/+1       长按 U/D = 拍数 -1/+1
 *   L/R      BPM -5/+5       长按 L/R = 细分循环 1→2→3→4
 *
 * 字体：方舟像素 ark12(界面) / ark24(大数字/拍号)，由 tools/gen_ark_font.py 生成
 *
 * 调音器扩展（硬件未到，先留槽）：
 *   计划 I2S MEMS 麦克风（INMP441/ICS-43434），接线用本板空闲的 EXT 口：
 *     BCK=GPIO25  WS=GPIO26  DATA=GPIO32  VDD=3V3  GND
 *   采样 16kHz 单声道 16bit，1024~2048 点；单音基频用自相关(或 YIN)，
 *   不用 FFT 峰值（谐波强时会上八度）。流程：分帧→去直流/加窗→自相关→
 *   归一化找第一个主峰→抛物线插值→f = fs/lag→f→音名+音分。
 *   界面：音名 + cents(-50..+50) 指针。代码里 MODE_TUNER 已留好位置，
 *   到时只需实现 tuner_tick()/tuner_ui_refresh()，其余不用动。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/i2c_master.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "lvgl.h"
#include "sdkconfig.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "ark12_font.h"
#include "ark24_font.h"

/* ---------------- 板级 ---------------- */
#define LCD_HOST                    SPI2_HOST
#define LCD_PIXEL_CLOCK_HZ          (60 * 1000 * 1000)
#define LCD_NATIVE_H_RES            128
#define LCD_NATIVE_V_RES            160
#define LCD_H_RES                   160
#define LCD_V_RES                   128
#define LCD_DRAW_BUF_LINES          LCD_V_RES

#define PIN_LCD_SCLK                GPIO_NUM_18
#define PIN_LCD_MOSI                GPIO_NUM_23
#define PIN_LCD_MISO                GPIO_NUM_19
#define PIN_LCD_CS                  GPIO_NUM_5
#define PIN_LCD_DC                  GPIO_NUM_4
#define PIN_BUZZER                  GPIO_NUM_14

#define PIN_I2C_SCL                 GPIO_NUM_15
#define PIN_I2C_SDA                 GPIO_NUM_21
#define GD32_ADDR                   0x40
#define GD32_LED1_REG               0xA0
#define GD32_LED2_REG               0xA1

/* 预留：I2S 麦克风（调音器用，硬件未接） */
#define PIN_MIC_BCK                 GPIO_NUM_25
#define PIN_MIC_WS                  GPIO_NUM_26
#define PIN_MIC_DATA                GPIO_NUM_32

#define KEY_UP_GPIO                 GPIO_NUM_2
#define KEY_DOWN_GPIO               GPIO_NUM_13
#define KEY_LEFT_GPIO               GPIO_NUM_27
#define KEY_RIGHT_GPIO              GPIO_NUM_35
#define KEY_A_GPIO                  GPIO_NUM_34
#define KEY_B_GPIO                  GPIO_NUM_12

#define BUTTON_ACTIVE_LEVEL         0
#define BUTTON_DEBOUNCE_MS          20
#define KEY_LONG_MS                 600

#define BUZZER_LEDC_MODE            LEDC_LOW_SPEED_MODE
#define BUZZER_LEDC_TIMER           LEDC_TIMER_0
#define BUZZER_LEDC_CHANNEL         LEDC_CHANNEL_0
#define BUZZER_DUTY                 128

#define LVGL_TICK_PERIOD_MS         1
#define LVGL_TASK_STACK_SIZE        (12 * 1024)
#define LVGL_TASK_PRIORITY          5
#define LVGL_TASK_MIN_DELAY_MS      2
#define LVGL_TASK_MAX_DELAY_MS      10
#define UI_REFRESH_MS               40

/* 版面 */
#define HDR_H       18
#define CARD_X      4
#define CARD_W      152
#define CARD_Y      21
#define CARD_H      38
#define INFO_Y      62
#define BAR_Y       78
#define BAR_H       6
#define DOTS_Y      88
#define STATE_Y     100
#define HINT_Y      114
#define PAD_X       6

/* ST7735 */
#define ST7735_SWRESET 0x01
#define ST7735_SLPOUT  0x11
#define ST7735_NORON   0x13
#define ST7735_INVOFF  0x20
#define ST7735_DISPOFF 0x28
#define ST7735_DISPON  0x29
#define ST7735_CASET   0x2A
#define ST7735_RASET   0x2B
#define ST7735_RAMWR   0x2C
#define ST7735_MADCTL  0x36
#define ST7735_COLMOD  0x3A
#define ST7735_FRMCTR1 0xB1
#define ST7735_FRMCTR2 0xB2
#define ST7735_FRMCTR3 0xB3
#define ST7735_INVCTR  0xB4
#define ST7735_PWCTR1  0xC0
#define ST7735_PWCTR2  0xC1
#define ST7735_PWCTR3  0xC2
#define ST7735_PWCTR4  0xC3
#define ST7735_PWCTR5  0xC4
#define ST7735_VMCTR1  0xC5
#define ST7735_GMCTRP1 0xE0
#define ST7735_GMCTRN1 0xE1
#define MADCTL_MY      0x80
#define MADCTL_MX      0x40
#define MADCTL_MV      0x20
#define MADCTL_RGB     0x00

static const char *TAG = "xiaomiao_metro";

/* ---------------- 调色板 ---------------- */
static const uint32_t C_BG      = 0x0B1014;
static const uint32_t C_HDR     = 0x141C24;
static const uint32_t C_CARD    = 0x17212B;
static const uint32_t C_CARD2   = 0x1E2A36;
static const uint32_t C_LINE    = 0x2A3846;
static const uint32_t C_ACCENT  = 0x6FD3A0;
static const uint32_t C_ACC_DIM = 0x2E4A3C;
static const uint32_t C_CREAM   = 0xFFE9A8;
static const uint32_t C_TEXT    = 0xE6EDF3;
static const uint32_t C_DIM     = 0x7E8FA0;

/* ---------------- 状态 ---------------- */
#define BPM_MIN 30
#define BPM_MAX 300
#define BEATS_MAX 12

typedef enum { MODE_METRO = 0, MODE_TUNER, MODE_COUNT } app_mode_t;
static const char *k_mode_name[MODE_COUNT] = {"节拍器", "调音器"};
static const char *k_mode_ascii[MODE_COUNT] = {"METRO", "TUNER"};

static int  s_mode = MODE_METRO;
static int  s_bpm = 120;
static int  s_beats = 4;              /* 每小节拍数 2..12 */
static int  s_unit = 4;               /* 拍值 4 或 8（仅显示/记谱意义） */
static int  s_sub = 1;                /* 细分 1/2/3/4 */
static int  s_accent = 0;             /* 0=只有首拍重音 1=每拍都重音 */
static int  s_sound = 1;
static int  s_led = 1;
static int  s_running = 0;

static int  s_set_i = 0, s_set_view = 0;   /* 0=列表 1=键位引导 */
static bool s_in_set = false;              /* 是否在设置页 */
static int  s_beat_i = 0, s_sub_i = 0;     /* 当前拍号内位置 */
static bool s_beat_pulse = false;          /* 本毫秒刚出拍（给 UI/LED 用） */

/* 拍点引擎（微秒相位累加，无漂移） */
static uint64_t s_acc_us = 0;
static uint32_t s_gap_us = 500000;

/* 点击音 */
static int s_click_left_ms = 0;
static int64_t s_last_bar_us = 0;

/* tap tempo */
static uint32_t s_tap_ms[4];
static int s_tap_n = 0;

static bool s_buz_ready = false;

/* ---------------- 蜂鸣器 ---------------- */
static void buz_init(void)
{
    ledc_timer_config_t tc = {
        .speed_mode = BUZZER_LEDC_MODE, .duty_resolution = LEDC_TIMER_8_BIT,
        .timer_num = BUZZER_LEDC_TIMER, .freq_hz = 2000, .clk_cfg = LEDC_AUTO_CLK,
    };
    if (ledc_timer_config(&tc) != ESP_OK) { ESP_LOGW(TAG, "buzzer timer fail"); return; }
    ledc_channel_config_t cc = {
        .gpio_num = PIN_BUZZER, .speed_mode = BUZZER_LEDC_MODE, .channel = BUZZER_LEDC_CHANNEL,
        .intr_type = LEDC_INTR_DISABLE, .timer_sel = BUZZER_LEDC_TIMER, .duty = 0, .hpoint = 0,
        .sleep_mode = LEDC_SLEEP_MODE_NO_ALIVE_NO_PD,
    };
    if (ledc_channel_config(&cc) == ESP_OK) s_buz_ready = true;
    else ESP_LOGW(TAG, "buzzer channel fail");
}

static void buz_off(void)
{
    if (!s_buz_ready) return;
    ledc_set_duty(BUZZER_LEDC_MODE, BUZZER_LEDC_CHANNEL, 0);
    ledc_update_duty(BUZZER_LEDC_MODE, BUZZER_LEDC_CHANNEL);
}

/* 打一个"嘀"：freq Hz，dur_ms 毫秒。由 1ms tick 负责关断 */
static void click(int freq, int dur_ms, int duty)
{
    if (!s_sound || !s_buz_ready) return;
    ledc_set_freq(BUZZER_LEDC_MODE, BUZZER_LEDC_TIMER, (uint32_t)freq);
    ledc_set_duty(BUZZER_LEDC_MODE, BUZZER_LEDC_CHANNEL, (uint32_t)duty);
    ledc_update_duty(BUZZER_LEDC_MODE, BUZZER_LEDC_CHANNEL);
    s_click_left_ms = dur_ms;
}

/* ---------------- GD32 LED ---------------- */
static i2c_master_bus_handle_t s_i2c = NULL;
static i2c_master_dev_handle_t s_gd32 = NULL;
static bool s_gd32_ok = false;
static bool s_led1_on = false, s_led2_on = false;

static esp_err_t gd32_write_reg(uint8_t reg, uint8_t val)
{
    if (!s_gd32) return ESP_ERR_INVALID_STATE;
    uint8_t buf[2] = {reg, val};
    return i2c_master_transmit(s_gd32, buf, sizeof(buf), 50);
}

static void gd32_init(void)
{
    i2c_master_bus_config_t bc = {
        .i2c_port = I2C_NUM_0, .sda_io_num = PIN_I2C_SDA, .scl_io_num = PIN_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    if (i2c_new_master_bus(&bc, &s_i2c) != ESP_OK) { ESP_LOGW(TAG, "i2c bus fail"); return; }
    i2c_device_config_t dc = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = GD32_ADDR, .scl_speed_hz = 100000,
    };
    if (i2c_master_bus_add_device(s_i2c, &dc, &s_gd32) != ESP_OK) { ESP_LOGW(TAG, "gd32 dev fail"); return; }
    esp_err_t e1 = gd32_write_reg(GD32_LED1_REG, 0);
    esp_err_t e2 = gd32_write_reg(GD32_LED2_REG, 0);
    s_gd32_ok = (e1 == ESP_OK && e2 == ESP_OK);
    ESP_LOGI(TAG, "gd32 led: %s", s_gd32_ok ? "FOUND" : "ABSENT");
}

static void led_apply(int which, bool on)
{
    if (!s_gd32_ok) return;
    if (which == 1) {
        if (s_led1_on == on) return;
        s_led1_on = on;
        gd32_write_reg(GD32_LED1_REG, on ? 1 : 0);
    } else {
        if (s_led2_on == on) return;
        s_led2_on = on;
        gd32_write_reg(GD32_LED2_REG, on ? 1 : 0);
    }
}

/* ---------------- 拍点引擎 ---------------- */
static void engine_recalc(void)
{
    uint32_t beat_us = 60000000u / (uint32_t)s_bpm;
    uint32_t gap = beat_us / (uint32_t)s_sub;
    if (gap < 5000) gap = 5000;            /* 上限 200Hz 细分，防抖 */
    s_gap_us = gap;
    if (s_acc_us >= s_gap_us) s_acc_us = 0;
}

static void engine_reset(void)
{
    s_acc_us = 0;
    s_beat_i = 0;
    s_sub_i = 0;
    s_beat_pulse = false;
}

/* 出一个拍（含细分） */
static void engine_fire(void)
{
    const int at_beat = (s_sub_i == 0);
    if (at_beat && s_beat_i == 0) {          /* 每小节一次：实测小节间隔，便于验证 BPM */
        int64_t now = esp_timer_get_time();
        if (s_last_bar_us > 0) {
            int64_t dt = now - s_last_bar_us;
            int64_t exp = (int64_t)s_beats * (60000000 / s_bpm);
            ESP_LOGI(TAG, "bar dt=%lldms exp=%lldms", (long long)(dt / 1000), (long long)(exp / 1000));
        }
        s_last_bar_us = now;
    }
    if (at_beat) {
        const bool accent = (s_accent != 0) || (s_beat_i == 0);
        if (accent) click(2093, 30, 200);   /* C7，响一点、长一点 */
        else        click(1568, 20, 140);   /* G6 */
        s_beat_pulse = true;
    } else {
        click(1047, 12, 70);                /* C6，细分音，轻 */
    }
    /* 推进位置 */
    s_sub_i++;
    if (s_sub_i >= s_sub) {
        s_sub_i = 0;
        s_beat_i = (s_beat_i + 1) % s_beats;
    }
}

static void tick_1ms(void *arg)
{
    (void)arg;
    s_beat_pulse = false;

    /* 点击音关断 */
    if (s_click_left_ms > 0) {
        if (--s_click_left_ms == 0) buz_off();
    }

    /* 拍点推进（相位累加，不漂移） */
    if (s_running && s_mode == MODE_METRO) {
        s_acc_us += 1000;
        int guard = 0;
        while (s_acc_us >= s_gap_us && guard++ < 64) {
            s_acc_us -= s_gap_us;
            engine_fire();
            if (s_gap_us == 0) break;
        }
        if (guard >= 64) { s_acc_us = 0; }
    }

    /* LED：LED1 每拍闪 40ms，LED2 常亮=running */
    if (s_led) {
        static int pulse_left = 0;
        if (s_beat_pulse) pulse_left = 40;
        if (pulse_left > 0) { led_apply(1, true); pulse_left--; }
        else led_apply(1, false);
    } else {
        led_apply(1, false);
    }
    led_apply(2, s_running && s_mode == MODE_METRO);
}

/* ---------------- 按键 ---------------- */
typedef struct { gpio_num_t gpio; } btn_t;
static const btn_t s_btns[] = {
    {KEY_UP_GPIO}, {KEY_DOWN_GPIO}, {KEY_LEFT_GPIO},
    {KEY_RIGHT_GPIO}, {KEY_A_GPIO}, {KEY_B_GPIO},
};
#define BTN_N ((int)(sizeof(s_btns) / sizeof(s_btns[0])))

static int s_last_idx = -1, s_stable_idx = -1;
static uint32_t s_last_change = 0;

static int keys_scan(void)
{
    int idx = -1;
    uint32_t now = lv_tick_get();
    for (int i = 0; i < BTN_N; ++i) {
        if (gpio_get_level(s_btns[i].gpio) == BUTTON_ACTIVE_LEVEL) { idx = i; break; }
    }
    if (idx != s_last_idx) { s_last_idx = idx; s_last_change = now; }
    if ((now - s_last_change) >= BUTTON_DEBOUNCE_MS) s_stable_idx = idx;
    return s_stable_idx;
}

static void buttons_init(void)
{
    uint64_t mask = 0, pull = 0;
    for (int i = 0; i < BTN_N; ++i) {
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

/* ---------------- NVS ---------------- */
#define NVS_NS "metro"

static void settings_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    int32_t v = 0;
    if (nvs_get_i32(h, "bpm", &v) == ESP_OK && v >= BPM_MIN && v <= BPM_MAX) s_bpm = (int)v;
    if (nvs_get_i32(h, "beats", &v) == ESP_OK && v >= 2 && v <= BEATS_MAX) s_beats = (int)v;
    if (nvs_get_i32(h, "unit", &v) == ESP_OK && (v == 4 || v == 8)) s_unit = (int)v;
    if (nvs_get_i32(h, "sub", &v) == ESP_OK && v >= 1 && v <= 4) s_sub = (int)v;
    if (nvs_get_i32(h, "accent", &v) == ESP_OK) s_accent = v ? 1 : 0;
    if (nvs_get_i32(h, "sound", &v) == ESP_OK) s_sound = v ? 1 : 0;
    if (nvs_get_i32(h, "led", &v) == ESP_OK) s_led = v ? 1 : 0;
    nvs_close(h);
}

static void settings_save(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_i32(h, "bpm", s_bpm);
    nvs_set_i32(h, "beats", s_beats);
    nvs_set_i32(h, "unit", s_unit);
    nvs_set_i32(h, "sub", s_sub);
    nvs_set_i32(h, "accent", s_accent);
    nvs_set_i32(h, "sound", s_sound);
    nvs_set_i32(h, "led", s_led);
    nvs_commit(h);
    nvs_close(h);
}

/* ---------------- LCD / LVGL ---------------- */
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
    int w = area->x2 - area->x1 + 1, h = area->y2 - area->y1 + 1;
    uint16_t xs = (uint16_t)area->x1, xe = (uint16_t)area->x2;
    uint16_t ys = (uint16_t)area->y1, ye = (uint16_t)area->y2;
    uint8_t caset[] = {xs >> 8, xs & 0xFF, xe >> 8, xe & 0xFF};
    uint8_t raset[] = {ys >> 8, ys & 0xFF, ye >> 8, ye & 0xFF};
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io, ST7735_CASET, caset, sizeof(caset)));
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io, ST7735_RASET, raset, sizeof(raset)));
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_color(io, ST7735_RAMWR, px, w * h * sizeof(uint16_t)));
}

static void lvgl_tick_cb(void *arg) { (void)arg; lv_tick_inc(LVGL_TICK_PERIOD_MS); }
static void st_tx(esp_lcd_panel_io_handle_t io, int cmd, const void *p, size_t n)
{
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io, cmd, p, n));
}

static void st_clear_black(esp_lcd_panel_io_handle_t io)
{
    static uint16_t line[LCD_H_RES * 8];
    const uint8_t caset[] = {0x00, 0x00, 0x00, LCD_H_RES - 1};
    memset(line, 0, sizeof(line));
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io, ST7735_CASET, caset, sizeof(caset)));
    for (uint16_t y = 0; y < LCD_V_RES; y += 8) {
        uint16_t y2 = (uint16_t)((y + 7 < LCD_V_RES) ? (y + 7) : (LCD_V_RES - 1));
        const uint8_t raset[] = {y >> 8, y & 0xFF, y2 >> 8, y2 & 0xFF};
        ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io, ST7735_RASET, raset, sizeof(raset)));
        ESP_ERROR_CHECK(esp_lcd_panel_io_tx_color(io, ST7735_RAMWR, line,
                                                  (y2 - y + 1) * LCD_H_RES * sizeof(uint16_t)));
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

    ESP_LOGI(TAG, "ST7735 init");
    st_tx(io, ST7735_DISPOFF, NULL, 0);
    st_tx(io, ST7735_SWRESET, NULL, 0);
    vTaskDelay(pdMS_TO_TICKS(150));
    st_tx(io, ST7735_SLPOUT, NULL, 0);
    vTaskDelay(pdMS_TO_TICKS(500));
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
    vTaskDelay(pdMS_TO_TICKS(10));
    st_tx(io, ST7735_MADCTL, madctl_rot90, sizeof(madctl_rot90));
    st_clear_black(io);
}

static void lcd_display_on(void)
{
    if (s_lcd_on || !s_lcd_io) return;
    st_tx(s_lcd_io, ST7735_DISPON, NULL, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    s_lcd_on = true;
}

static esp_lcd_panel_io_handle_t lcd_init(void)
{
    spi_bus_config_t buscfg = {
        .sclk_io_num = PIN_LCD_SCLK, .mosi_io_num = PIN_LCD_MOSI, .miso_io_num = PIN_LCD_MISO,
        .quadwp_io_num = -1, .quadhd_io_num = -1,
        .max_transfer_sz = LCD_H_RES * LCD_DRAW_BUF_LINES * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_spi_config_t ioc = {
        .dc_gpio_num = PIN_LCD_DC, .cs_gpio_num = PIN_LCD_CS, .pclk_hz = LCD_PIXEL_CLOCK_HZ,
        .lcd_cmd_bits = 8, .lcd_param_bits = 8, .spi_mode = 0, .trans_queue_depth = 10,
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
    lv_display_set_dpi(disp, 60);
    lv_display_set_buffers(disp, b1, b2, buf_sz, LV_DISPLAY_RENDER_MODE_FULL);
    lv_draw_buf_init(&s_draw_buf3, LCD_H_RES, LCD_DRAW_BUF_LINES, cf, stride, b3, buf_sz);
    lv_display_set_3rd_draw_buffer(disp, &s_draw_buf3);
    lv_display_set_user_data(disp, io);
    lv_display_set_flush_cb(disp, flush_cb);
    lv_display_set_default(disp);
    ESP_LOGI(TAG, "LVGL display 160x128 ready");
    return disp;
}

/* ---------------- UI ---------------- */
static lv_obj_t *s_lbl_hdr, *s_lbl_right, *s_lbl_bpm, *s_lbl_bpmunit, *s_lbl_sig;
static lv_obj_t *s_lbl_info, *s_lbl_state, *s_lbl_hint, *s_bar, *s_card;
static lv_obj_t *s_dot[BEATS_MAX];
static lv_obj_t *s_rows, *s_row[6], *s_row_name[6], *s_row_val[6];
static lv_obj_t *s_guide;
#define SET_ROWS 6

static lv_obj_t *mk_label(lv_obj_t *parent, const char *txt, int x, int y,
                          uint32_t color, const lv_font_t *font)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, txt);
    lv_obj_set_pos(l, x, y);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_obj_set_style_text_font(l, font ? font : &ark12, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_CLIP);
    return l;
}

static lv_obj_t *mk_panel(lv_obj_t *parent, int x, int y, int w, int h,
                          uint32_t bg, uint32_t border, int radius)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    if (border) {
        lv_obj_set_style_border_color(o, lv_color_hex(border), 0);
        lv_obj_set_style_border_width(o, 1, 0);
    }
    lv_obj_set_style_radius(o, radius, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

static void ui_create(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    mk_panel(scr, 0, 0, LCD_H_RES, HDR_H, C_HDR, 0, 0);
    mk_panel(scr, 0, HDR_H - 1, LCD_H_RES, 1, C_ACC_DIM, 0, 0);
    s_lbl_hdr = mk_label(scr, "节拍器", 6, 3, C_ACCENT, &ark12);
    s_lbl_right = mk_label(scr, "", 96, 3, C_DIM, &ark12);
    lv_obj_set_width(s_lbl_right, 58);
    lv_obj_set_style_text_align(s_lbl_right, LV_TEXT_ALIGN_RIGHT, 0);

    s_card = mk_panel(scr, CARD_X, CARD_Y, CARD_W, CARD_H, C_CARD, C_LINE, 6);
    s_lbl_bpm = mk_label(s_card, "120", 8, 5, C_CREAM, &ark24);
    s_lbl_bpmunit = mk_label(s_card, "BPM", 8, 26, C_DIM, &ark12);
    s_lbl_sig = mk_label(s_card, "4/4", 118, 10, C_ACCENT, &ark12);
    lv_obj_set_width(s_lbl_sig, 30);
    lv_obj_set_style_text_align(s_lbl_sig, LV_TEXT_ALIGN_RIGHT, 0);

    s_lbl_info = mk_label(scr, "", PAD_X, INFO_Y, C_DIM, &ark12);
    lv_obj_set_width(s_lbl_info, LCD_H_RES - 2 * PAD_X);

    s_bar = lv_bar_create(scr);
    lv_obj_set_pos(s_bar, PAD_X, BAR_Y);
    lv_obj_set_size(s_bar, LCD_H_RES - 2 * PAD_X, BAR_H);
    lv_bar_set_range(s_bar, 0, 1000);
    lv_obj_set_style_bg_color(s_bar, lv_color_hex(C_CARD2), 0);
    lv_obj_set_style_bg_color(s_bar, lv_color_hex(C_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_bar, 3, 0);

    /* 拍点指示（最多 12 个） */
    for (int i = 0; i < BEATS_MAX; ++i) {
        s_dot[i] = mk_panel(scr, 0, DOTS_Y, 7, 7, C_CARD2, 0, 3);
    }

    s_lbl_state = mk_label(scr, "", PAD_X, STATE_Y, C_DIM, &ark12);
    s_lbl_hint  = mk_label(scr, "", PAD_X, HINT_Y, C_DIM, &ark12);

    /* 设置列表 */
    s_rows = lv_obj_create(scr);
    lv_obj_remove_style_all(s_rows);
    lv_obj_set_pos(s_rows, CARD_X, CARD_Y);
    lv_obj_set_size(s_rows, CARD_W, SET_ROWS * 17);
    lv_obj_set_style_bg_opa(s_rows, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(s_rows, LV_OBJ_FLAG_SCROLLABLE);
    static const char *names[SET_ROWS] = {"拍号", "细分", "重音", "闪灯", "声音", "键位引导"};
    for (int i = 0; i < SET_ROWS; ++i) {
        s_row[i] = mk_panel(s_rows, 0, i * 17, CARD_W, 15, C_CARD, 0, 4);
        s_row_name[i] = mk_label(s_row[i], names[i], 6, 1, C_TEXT, &ark12);
        s_row_val[i] = mk_label(s_row[i], "", 96, 1, C_CREAM, &ark12);
        lv_obj_set_width(s_row_val[i], CARD_W - 102);
        lv_obj_set_style_text_align(s_row_val[i], LV_TEXT_ALIGN_RIGHT, 0);
    }

    /* 键位引导子页 */
    s_guide = lv_obj_create(scr);
    lv_obj_remove_style_all(s_guide);
    lv_obj_set_pos(s_guide, 0, CARD_Y);
    lv_obj_set_size(s_guide, LCD_H_RES, LCD_V_RES - CARD_Y);
    lv_obj_set_style_bg_opa(s_guide, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(s_guide, LV_OBJ_FLAG_SCROLLABLE);
    mk_label(s_guide, "A   开始/停止", 6, 2, C_TEXT, &ark12);
    mk_label(s_guide, "B   打拍定速", 6, 15, C_TEXT, &ark12);
    mk_label(s_guide, "U/D BPM ±1", 6, 28, C_TEXT, &ark12);
    mk_label(s_guide, "L/R BPM ±5", 6, 41, C_TEXT, &ark12);
    mk_label(s_guide, "长按U/D 拍数", 6, 54, C_DIM, &ark12);
    mk_label(s_guide, "长按L/R 细分", 6, 67, C_DIM, &ark12);
    mk_label(s_guide, "长按B 调音器", 6, 80, C_DIM, &ark12);
    mk_label(s_guide, "B 返回列表", 6, 93, C_ACCENT, &ark12);
    lv_obj_add_flag(s_guide, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_rows, LV_OBJ_FLAG_HIDDEN);
}

static void show_obj(lv_obj_t *o, bool show)
{
    if (!o) return;
    if (show) lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
    else      lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}

/* 每帧统一裁决显隐，避免切页残留 */
static void apply_view2(void)
{
    const bool guide = s_in_set && s_set_view == 1;
    const bool rows  = s_in_set && s_set_view == 0;
    const bool main  = !s_in_set;                 /* 节拍器 / 调音器都用主区 */

    show_obj(s_card, main);
    show_obj(s_lbl_info, main);
    show_obj(s_bar, main);
    show_obj(s_lbl_state, main);
    show_obj(s_lbl_hint, main);
    for (int i = 0; i < BEATS_MAX; ++i)
        show_obj(s_dot[i], main && s_mode == MODE_METRO);
    show_obj(s_rows, rows);
    show_obj(s_guide, guide);
}

static void ui_refresh(void)
{
    char buf[96];
    apply_view2();

    lv_label_set_text(s_lbl_hdr, s_in_set ? "设置" : k_mode_name[s_mode]);

    if (s_in_set) {
        lv_label_set_text(s_lbl_right, "");
    } else if (s_mode == MODE_TUNER) {
        lv_label_set_text(s_lbl_right, "无麦克风");
    } else if (!s_sound) {
        lv_label_set_text(s_lbl_right, "静音");
    } else {
        lv_label_set_text(s_lbl_right, s_running ? "▶ 运行" : "■ 停止");
    }

    if (s_mode == MODE_TUNER && !s_in_set) {
        /* 调音器占位页 */
        show_obj(s_card, true);
        lv_label_set_text(s_lbl_bpm, "---");
        lv_label_set_text(s_lbl_bpmunit, "Hz");
        lv_label_set_text(s_lbl_sig, "--");
        lv_label_set_text(s_lbl_info, "未接麦克风");
        lv_bar_set_value(s_bar, 0, LV_ANIM_OFF);
        lv_label_set_text(s_lbl_state, "I2S BCK25 WS26 DAT32");
        lv_label_set_text(s_lbl_hint, "长按B 返回节拍器");
        return;
    }

    /* 节拍器主界面 */
    snprintf(buf, sizeof(buf), "%d", s_bpm);
    lv_label_set_text(s_lbl_bpm, buf);
    snprintf(buf, sizeof(buf), "%d/%d", s_beats, s_unit);
    lv_label_set_text(s_lbl_sig, buf);

    snprintf(buf, sizeof(buf), "细分 ×%d  重音 %s", s_sub, s_accent ? "每拍" : "首拍");
    lv_label_set_text(s_lbl_info, buf);

    /* 拍内进度（相位） */
    if (s_running && s_gap_us)
        lv_bar_set_value(s_bar, (int)((s_acc_us * 1000ull) / s_gap_us), LV_ANIM_OFF);
    else
        lv_bar_set_value(s_bar, 0, LV_ANIM_OFF);

    /* 拍点：按拍数均分，当前拍高亮；首拍用 accent 色 */
    const int n = s_beats;
    const int total = n * 7 + (n - 1) * 4;      /* 7px 点 + 4px 间隔 */
    int x = (LCD_H_RES - total) / 2;
    if (x < 4) x = 4;
    for (int i = 0; i < BEATS_MAX; ++i) {
        if (i < n) {
            lv_obj_set_pos(s_dot[i], x + i * 11, DOTS_Y);
            show_obj(s_dot[i], true);
            uint32_t c = C_CARD2;
            if (i == s_beat_i && s_running) c = (i == 0) ? C_CREAM : C_ACCENT;
            else if (i == 0) c = C_ACC_DIM;
            lv_obj_set_style_bg_color(s_dot[i], lv_color_hex(c), 0);
        } else {
            show_obj(s_dot[i], false);
        }
    }

    if (s_running) {
        snprintf(buf, sizeof(buf), "第 %d 拍", s_beat_i + 1);
        lv_label_set_text(s_lbl_state, buf);
    } else {
        lv_label_set_text(s_lbl_state, "A 开始");
    }
    lv_label_set_text(s_lbl_hint, "A 开始/停止   B 打拍");

    /* 设置列表数值 */
    if (s_in_set && s_set_view == 0) {
        for (int i = 0; i < SET_ROWS; ++i) {
            const bool sel = (i == s_set_i);
            lv_obj_set_style_bg_color(s_row[i], lv_color_hex(sel ? C_CARD2 : C_CARD), 0);
            lv_obj_set_style_text_color(s_row_name[i], lv_color_hex(sel ? C_ACCENT : C_TEXT), 0);
        }
        snprintf(buf, sizeof(buf), "%d/%d", s_beats, s_unit);
        lv_label_set_text(s_row_val[0], buf);
        snprintf(buf, sizeof(buf), "×%d", s_sub);
        lv_label_set_text(s_row_val[1], buf);
        lv_label_set_text(s_row_val[2], s_accent ? "每拍" : "首拍");
        lv_label_set_text(s_row_val[3], s_led ? "开" : "关");
        lv_label_set_text(s_row_val[4], s_sound ? "开" : "关");
        lv_label_set_text(s_row_val[5], ">");
        (void)buf;
        lv_bar_set_value(s_bar, (s_set_i + 1) * 1000 / SET_ROWS, LV_ANIM_OFF);
    }
    if (s_in_set && s_set_view == 1) {
        lv_label_set_text(s_lbl_hint, "");
    }
}

/* ---------------- 按键行为 ---------------- */
static void tap_tempo(uint32_t now)
{
    if (s_tap_n > 0 && (now - s_tap_ms[(s_tap_n - 1) % 4]) > 2000) s_tap_n = 0;
    s_tap_ms[s_tap_n % 4] = now;
    s_tap_n++;
    if (s_tap_n >= 2) {
        int cnt = (s_tap_n < 4) ? s_tap_n : 4;
        uint32_t first = s_tap_ms[(s_tap_n - cnt) % 4];
        uint32_t last = s_tap_ms[(s_tap_n - 1) % 4];
        uint32_t span = last - first;
        if (span > 0) {
            int bpm = (int)((uint32_t)(cnt - 1) * 60000u / span);
            if (bpm < BPM_MIN) bpm = BPM_MIN;
            if (bpm > BPM_MAX) bpm = BPM_MAX;
            s_bpm = bpm;
            engine_recalc();
            settings_save();
        }
    }
    ESP_LOGI(TAG, "tap -> bpm %d (n=%d)", s_bpm, s_tap_n);
}

static void set_row_change(int i, int dir)
{
    switch (i) {
    case 0:  /* 拍号 */
        s_beats += dir;
        if (s_beats < 2) s_beats = 2;
        if (s_beats > BEATS_MAX) s_beats = BEATS_MAX;
        if (s_beat_i >= s_beats) s_beat_i = 0;
        break;
    case 1:
        s_sub += dir;
        if (s_sub < 1) s_sub = 1;
        if (s_sub > 4) s_sub = 4;
        engine_recalc();
        s_sub_i = 0;
        break;
    case 2:
        s_accent = !s_accent;
        break;
    case 3:
        s_led = !s_led;
        break;
    case 4:
        s_sound = !s_sound;
        if (!s_sound) buz_off();
        break;
    default:
        break;
    }
    settings_save();
}

static void key_short(int idx)
{
    if (s_in_set) {
        if (s_set_view == 1) { if (idx == 5) s_set_view = 0; return; }
        if (idx == 0) s_set_i = (s_set_i + SET_ROWS - 1) % SET_ROWS;
        else if (idx == 1) s_set_i = (s_set_i + 1) % SET_ROWS;
        else if (idx == 2 && s_set_i < 5) set_row_change(s_set_i, -1);
        else if (idx == 3 && s_set_i < 5) set_row_change(s_set_i, +1);
        else if (idx == 4) {
            if (s_set_i == 5) s_set_view = 1;
            else set_row_change(s_set_i, +1);   /* A 当"切换"用 */
        } else if (idx == 5) {
            s_in_set = false;
            s_set_view = 0;
        }
        return;
    }

    if (s_mode == MODE_TUNER) {
        if (idx == 5 || idx == 4) s_mode = MODE_METRO;
        return;
    }

    switch (idx) {
    case 0: s_bpm += 1; break;                 /* U */
    case 1: s_bpm -= 1; break;                 /* D */
    case 2: s_bpm -= 5; break;                 /* L */
    case 3: s_bpm += 5; break;                 /* R */
    case 4:                                    /* A 开始/停止 */
        s_running = !s_running;
        if (s_running) {
            engine_recalc();
            engine_reset();
            engine_fire();                     /* 立刻出第一拍，手感更跟手 */
            s_beat_pulse = true;
        } else {
            buz_off();
            s_click_left_ms = 0;
            s_sub_i = 0;
            s_beat_i = 0;
        }
        ESP_LOGI(TAG, "running=%d bpm=%d", s_running, s_bpm);
        break;
    case 5: tap_tempo(lv_tick_get()); break;   /* B 打拍 */
    default: break;
    }

    if (s_bpm < BPM_MIN) s_bpm = BPM_MIN;
    if (s_bpm > BPM_MAX) s_bpm = BPM_MAX;
    engine_recalc();
    settings_save();
}

static void key_long(int idx)
{
    if (s_in_set) {
        if (idx == 5) { s_in_set = false; s_set_view = 0; }
        return;
    }

    if (idx == 4) {                            /* 长按 A：进设置 */
        s_in_set = true; s_set_view = 0;
        s_running = false; buz_off(); s_click_left_ms = 0;
        ESP_LOGI(TAG, "enter settings");
        return;
    }
    if (idx == 5) {                            /* 长按 B：切 节拍器/调音器 */
        s_mode = (s_mode == MODE_METRO) ? MODE_TUNER : MODE_METRO;
        s_running = false; buz_off(); s_click_left_ms = 0;
        ESP_LOGI(TAG, "mode -> %s", k_mode_ascii[s_mode]);
        return;
    }
    if (s_mode != MODE_METRO) return;

    if (idx == 0) {
        s_beats++; if (s_beats > BEATS_MAX) s_beats = 2;
        if (s_beat_i >= s_beats) s_beat_i = 0;
    } else if (idx == 1) {
        s_beats--; if (s_beats < 2) s_beats = BEATS_MAX;
        if (s_beat_i >= s_beats) s_beat_i = 0;
    } else {                                   /* 长按 L/R：细分循环 */
        s_sub = (s_sub % 4) + 1;
        engine_recalc();
        s_sub_i = 0;
    }
    settings_save();
    ESP_LOGI(TAG, "beats=%d sub=%d", s_beats, s_sub);
}

/* ---------------- 字体自检 ---------------- */
static int utf8_next(const char *s, uint32_t *cp)
{
    const uint8_t *p = (const uint8_t *)s;
    if (p[0] < 0x80) { *cp = p[0]; return 1; }
    if ((p[0] & 0xE0) == 0xC0) { *cp = ((p[0] & 0x1F) << 6) | (p[1] & 0x3F); return 2; }
    if ((p[0] & 0xF0) == 0xE0) { *cp = ((p[0] & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F); return 3; }
    *cp = p[0];
    return 1;
}

static int glyph_px(const lv_font_t *font, const char *s, int *box)
{
    uint32_t cp = 0;
    utf8_next(s, &cp);
    lv_font_glyph_dsc_t dsc;
    memset(&dsc, 0, sizeof(dsc));
    if (!lv_font_get_glyph_dsc(font, &dsc, cp, 0)) return -1;
    static uint8_t a8[64 * 64];
    lv_draw_buf_t db;
    lv_draw_buf_init(&db, 64, 64, LV_COLOR_FORMAT_A8, 64, a8, sizeof(a8));
    const void *bm = lv_font_get_glyph_bitmap(&dsc, &db);
    int px = 0;
    uint32_t st = lv_draw_buf_width_to_stride(dsc.box_w, LV_COLOR_FORMAT_A8);
    const uint8_t *pix = (bm == &db) ? (const uint8_t *)db.data : (const uint8_t *)bm;
    if (pix) for (int y = 0; y < dsc.box_h; ++y) for (int x = 0; x < dsc.box_w; ++x)
        if (pix[y * st + x] > 127) px++;
    if (box) *box = dsc.box_w * 100 + dsc.box_h;
    return px;
}

static void font_selftest(void)
{
    static const char *p12 = "节拍器A0";
    static const char *p24 = "120";
    int ok = 0, bad = 0, box = 0;
    for (const char *p = p12; *p; ) {
        char one[4] = {0};
        uint32_t cp = 0;
        int n = utf8_next(p, &cp);
        memcpy(one, p, (size_t)n);
        int px = glyph_px(&ark12, one, &box);
        ESP_LOGI(TAG, "ark12 U+%04X box=%dx%d px=%d", (unsigned)cp, box / 100, box % 100, px);
        if (px > 0) ok++; else bad++;
        p += n;
    }
    for (const char *p = p24; *p; p++) {
        char one[2] = {*p, 0};
        int px = glyph_px(&ark24, one, &box);
        ESP_LOGI(TAG, "ark24 '%c' box=%dx%d px=%d", *p, box / 100, box % 100, px);
        if (px > 0) ok++; else bad++;
    }
    ESP_LOGI(TAG, "font selftest: %d ok, %d bad -> %s", ok, bad, bad ? "FAIL" : "PASS");
}

/* ---------------- 主循环 ---------------- */
static void app_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "Start metronome UI");
    ui_create();
    s_first_flush_done = false;
    lv_refr_now(NULL);
    for (int i = 0; i < 100 && !s_first_flush_done; ++i) vTaskDelay(pdMS_TO_TICKS(1));
    lcd_display_on();

    uint32_t last_ui = 0;
    int held = -1;
    uint32_t down_ms = 0;
    bool long_done = false;

    while (true) {
        int cur = keys_scan();
        if (cur != held) {
            if (held >= 0 && !long_done) key_short(held);
            held = cur;
            down_ms = lv_tick_get();
            long_done = false;
        } else if (cur >= 0 && !long_done && (lv_tick_get() - down_ms) >= KEY_LONG_MS) {
            long_done = true;
            key_long(cur);
        }

        if (lv_tick_elaps(last_ui) >= UI_REFRESH_MS) {
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
    ESP_LOGI(TAG, "Xiaomiao Metronome boot");

    buttons_init();
    buz_init();

    esp_err_t nvserr = nvs_flash_init();
    if (nvserr == ESP_ERR_NVS_NO_FREE_PAGES || nvserr == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvserr = nvs_flash_init();
    }
    if (nvserr == ESP_OK) {
        settings_load();
        ESP_LOGI(TAG, "settings: bpm=%d beats=%d/%d sub=%d accent=%d sound=%d led=%d",
                 s_bpm, s_beats, s_unit, s_sub, s_accent, s_sound, s_led);
    } else {
        ESP_LOGW(TAG, "nvs init failed (%s)", esp_err_to_name(nvserr));
    }

    gd32_init();
    engine_recalc();

    esp_lcd_panel_io_handle_t io = lcd_init();
    lv_init();
    lv_display_t *disp = lvgl_display_init(io);
    (void)disp;

    esp_lcd_panel_io_callbacks_t cbs = { .on_color_trans_done = flush_ready_cb };
    ESP_ERROR_CHECK(esp_lcd_panel_io_register_event_callbacks(io, &cbs, lv_display_get_default()));

    font_selftest();

    const esp_timer_create_args_t tk = { .callback = lvgl_tick_cb, .name = "lv_tick" };
    esp_timer_handle_t t1 = NULL;
    ESP_ERROR_CHECK(esp_timer_create(&tk, &t1));
    ESP_ERROR_CHECK(esp_timer_start_periodic(t1, LVGL_TICK_PERIOD_MS * 1000));

    const esp_timer_create_args_t tk2 = { .callback = tick_1ms, .name = "metro_1ms" };
    esp_timer_handle_t t2 = NULL;
    ESP_ERROR_CHECK(esp_timer_create(&tk2, &t2));
    ESP_ERROR_CHECK(esp_timer_start_periodic(t2, 1000));

    BaseType_t ok = xTaskCreate(app_task, "metro", LVGL_TASK_STACK_SIZE, NULL, LVGL_TASK_PRIORITY, NULL);
    ESP_ERROR_CHECK(ok == pdPASS ? ESP_OK : ESP_FAIL);
}





