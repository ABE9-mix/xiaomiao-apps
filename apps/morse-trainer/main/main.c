/*
 * XiaoMiao ESP32-WROVER-B — Morse Code Trainer (摩斯码训练器) 固件  v3
 *
 * 硬件：ST7735 SPI TFT 160x128(旋转90)、GPIO14 无源蜂鸣器(LEDC)、
 *   6 键：UP=2 DOWN=13 LEFT=27 RIGHT=35 A=34 B=12（按下为低）
 *   板载 LED1/LED2 由 GD32F350 经 I2C(0x40, 0xA0/0xA1) 控制
 *
 * 字体：方舟像素 Ark Pixel —— ark12(界面) / ark24(主显, 由 12px 设计 2x 渲染)
 *   均由 tools/gen_ark_font.py 生成为 LVGL 原生 fmt_txt 格式（1bpp）
 *
 * 五个模式（长按 LEFT / RIGHT 切换）：
 *   LOOKUP 查询：翻表看码并发声
 *   LEARN  顺序：按 Koch 顺序逐个学
 *   TYPE   听辨：**播放莫斯音 → 从候选字里选**（6 个键打不了字，所以是听音选字）
 *   KEY    发报：单键(按时长判点划) / 双键(A=点 B=划)，自动解码
 *   SET    设置：列表（键位引导子页 / 声音开关 / 速率），A 进入/切换，B 返回
 *
 * 版面：header(18) → 卡片(38) → 信息行 → 进度条 → 状态 → 两行提示
 * 提示只留"非直觉"的长按操作；上下/左右/A确认/B返回 这些交给设置页的键位引导。
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
#include "esp_random.h"
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

/* ---------------- 板级常量 ---------------- */
#define LCD_HOST                    SPI2_HOST
#define LCD_PIXEL_CLOCK_HZ          (60 * 1000 * 1000)
#define LCD_NATIVE_H_RES            128
#define LCD_NATIVE_V_RES            160
#define LCD_H_RES                   160
#define LCD_V_RES                   128
#define LCD_DRAW_BUF_LINES          LCD_V_RES
#define LCD_DPI                     60

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
#define SIDETONE_HZ                 700

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
#define INFO_Y      63
#define BAR_Y       78
#define BAR_H       6
#define MSG_Y       88
#define H1_Y        102
#define H2_Y        115
#define PAD_X       6

/* ST7735 */
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
#define ST7735_PWCTR5               0xC4
#define ST7735_VMCTR1               0xC5
#define ST7735_GMCTRP1              0xE0
#define ST7735_GMCTRN1              0xE1
#define MADCTL_MY                   0x80
#define MADCTL_MX                   0x40
#define MADCTL_MV                   0x20
#define MADCTL_RGB                  0x00

static const char *TAG = "xiaomiao_morse";

/* ---------------- 调色板 ---------------- */
static const uint32_t C_BG       = 0x0B1014;
static const uint32_t C_HDR      = 0x141C24;
static const uint32_t C_CARD     = 0x17212B;
static const uint32_t C_CARD2    = 0x1E2A36;
static const uint32_t C_LINE     = 0x2A3846;
static const uint32_t C_ACCENT   = 0x6FD3A0;
static const uint32_t C_ACC_DIM  = 0x2E4A3C;
static const uint32_t C_CREAM    = 0xFFE9A8;
static const uint32_t C_TEXT     = 0xE6EDF3;
static const uint32_t C_DIM      = 0x7E8FA0;
static const uint32_t C_ERR      = 0xE0645A;

/* ---------------- 莫斯码表 ---------------- */
typedef struct { char ch; const char *code; } morse_t;
static const morse_t k_morse[] = {
    {'A', ".-"},      {'B', "-..."},    {'C', "-.-."},    {'D', "-.."},
    {'E', "."},       {'F', "..-."},    {'G', "--."},     {'H', "...."},
    {'I', ".."},      {'J', ".---"},    {'K', "-.-"},     {'L', ".-.."},
    {'M', "--"},      {'N', "-."},      {'O', "---"},     {'P', ".--."},
    {'Q', "--.-"},    {'R', ".-."},     {'S', "..."},     {'T', "-"},
    {'U', "..-"},     {'V', "...-"},    {'W', ".--"},     {'X', "-..-"},
    {'Y', "-.--"},    {'Z', "--.."},
    {'0', "-----"},   {'1', ".----"},   {'2', "..---"},   {'3', "...--"},
    {'4', "....-"},   {'5', "....."},   {'6', "-...."},   {'7', "--..."},
    {'8', "---.."},   {'9', "----."},
    {'.', ".-.-.-"},  {',', "--..--"},  {'?', "..--.."},  {'/', "-..-."},
    {'=', "-...-"},   {'+', ".-.-."},   {'-', "-....-"},  {':', "---..."},
    {';', "-.-.-."},  {'(', "-.--."},   {')', "-.--.-"},  {'@', ".--.-."},
    {'\'', ".----."}, {'"', ".-..-."},  {'!', "-.-.--"},  {'$', "...-..-"},
};
#define MORSE_N ((int)(sizeof(k_morse) / sizeof(k_morse[0])))

static const char k_koch[] = "KMRSUAPTLOWI.NJEF0YV,G5/Q9ZH38B?427C1D6X";
#define KOCH_N ((int)(sizeof(k_koch) - 1))

static const char *morse_code_of(char c)
{
    char u = (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
    for (int i = 0; i < MORSE_N; ++i) if (k_morse[i].ch == u) return k_morse[i].code;
    return NULL;
}

static char morse_decode(const char *pat)
{
    for (int i = 0; i < MORSE_N; ++i) if (strcmp(k_morse[i].code, pat) == 0) return k_morse[i].ch;
    return '?';
}

/* ---------------- 状态 ---------------- */
typedef enum { MODE_LOOKUP = 0, MODE_LEARN, MODE_TYPE, MODE_KEY, MODE_SET, MODE_COUNT } morse_mode_t;
static const char *k_mode_name[MODE_COUNT] = {"查询", "顺序", "听辨", "发报", "设置"};
static const char *k_mode_ascii[MODE_COUNT] = {"LOOKUP", "LEARN", "TYPE", "KEY", "SET"};

static const int k_wpms[] = {5, 8, 10, 12, 15, 20};
#define WPM_N ((int)(sizeof(k_wpms) / sizeof(k_wpms[0])))

static int  s_mode = MODE_LOOKUP;
static int  s_wpm = 10, s_wpm_i = 2;
static int  s_dual = 0;
static int  s_sound = 1;
static int  s_koch_i = 0;
static int  s_look_i = 0, s_look_hide = 0;
static int  s_set_i = 0, s_set_view = 0;     /* view: 0=列表 1=键位引导 */

static int  s_type_state = 0;                /* 0=待选 1=反馈 */
static char s_type_ans = 'K', s_type_sel = 'A';
static int  s_type_ok = 0, s_type_bad = 0, s_type_streak = 0;
static int  s_type_sel_i = 0;
static uint32_t s_type_fb_ms = 0;

static char s_key_pat[10];
static int  s_key_pat_len = 0;
static char s_key_text[14];
static int  s_key_text_len = 0;
static uint32_t s_ms_now = 0, s_last_symbol_ms = 0;
static int  s_a_down_ms = 0;
static bool s_a_prev = false, s_b_prev = false;

static bool s_buz_ready = false;

/* ---------------- 蜂鸣器 ---------------- */
static void buz_init(void)
{
    ledc_timer_config_t tc = {
        .speed_mode = BUZZER_LEDC_MODE, .duty_resolution = LEDC_TIMER_8_BIT,
        .timer_num = BUZZER_LEDC_TIMER, .freq_hz = SIDETONE_HZ, .clk_cfg = LEDC_AUTO_CLK,
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

static void buz_on(void)
{
    if (!s_buz_ready || !s_sound) return;
    ledc_set_freq(BUZZER_LEDC_MODE, BUZZER_LEDC_TIMER, SIDETONE_HZ);
    ledc_set_duty(BUZZER_LEDC_MODE, BUZZER_LEDC_CHANNEL, BUZZER_DUTY);
    ledc_update_duty(BUZZER_LEDC_MODE, BUZZER_LEDC_CHANNEL);
}

static void buz_off(void)
{
    if (!s_buz_ready) return;
    ledc_set_duty(BUZZER_LEDC_MODE, BUZZER_LEDC_CHANNEL, 0);
    ledc_update_duty(BUZZER_LEDC_MODE, BUZZER_LEDC_CHANNEL);
}

/* ---------------- GD32 板载 LED ---------------- */
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
        .i2c_port = I2C_NUM_0,
        .sda_io_num = PIN_I2C_SDA,
        .scl_io_num = PIN_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
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
    ESP_LOGI(TAG, "gd32 led: %s (led1=%s led2=%s)", s_gd32_ok ? "FOUND" : "ABSENT",
             esp_err_to_name(e1), esp_err_to_name(e2));
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

/* ---------------- 发声队列 ---------------- */
static const char *s_play_code = NULL;
static int  s_play_i = 0, s_play_left = 0;
static bool s_play_tone = false;
static volatile bool s_play_busy = false;

static int dot_ms(void) { int d = 1200 / s_wpm; return d < 20 ? 20 : d; }

static void morse_play(const char *code)
{
    if (!code) return;
    s_play_code = code;
    s_play_i = 0; s_play_left = 0; s_play_tone = false; s_play_busy = true;
}

static void play_tick(void)
{
    if (!s_play_busy) return;
    if (s_play_left > 0) { s_play_left--; return; }
    char c = s_play_code[s_play_i];
    if (c == 0) {
        if (s_play_tone) { buz_off(); s_play_tone = false; }
        s_play_busy = false;
        return;
    }
    if (s_play_tone) { buz_off(); s_play_tone = false; s_play_left = dot_ms(); s_play_i++; return; }
    buz_on();
    s_play_tone = true;
    s_play_left = (c == '-') ? dot_ms() * 3 : dot_ms();
}

/* ---------------- 发报键 / LED ---------------- */
static void key_push_sym(char sym)
{
    if (s_key_pat_len >= (int)sizeof(s_key_pat) - 1) return;
    s_key_pat[s_key_pat_len++] = sym;
    s_key_pat[s_key_pat_len] = 0;
    s_last_symbol_ms = s_ms_now;
}

static void key_flush_char(void)
{
    if (s_key_pat_len == 0) return;
    char ch = morse_decode(s_key_pat);
    if (s_key_text_len >= (int)sizeof(s_key_text) - 1) {
        memmove(s_key_text, s_key_text + 1, sizeof(s_key_text) - 2);
        s_key_text_len = (int)sizeof(s_key_text) - 2;
    }
    s_key_text[s_key_text_len++] = ch;
    s_key_text[s_key_text_len] = 0;
    s_key_pat_len = 0;
    s_key_pat[0] = 0;
    ESP_LOGI(TAG, "key decoded: %c", ch);
}

static void key_clear(void)
{
    s_key_pat_len = 0; s_key_pat[0] = 0;
    s_key_text_len = 0; s_key_text[0] = 0;
}

static void tick_1ms(void *arg)
{
    (void)arg;
    s_ms_now++;

    if (s_mode == MODE_KEY) {
        const bool a = (gpio_get_level(KEY_A_GPIO) == BUTTON_ACTIVE_LEVEL);
        const bool b = (gpio_get_level(KEY_B_GPIO) == BUTTON_ACTIVE_LEVEL);
        if (s_dual) {
            if (a && !s_a_prev) key_push_sym('.');
            if (b && !s_b_prev) key_push_sym('-');
        } else {
            if (a && !s_a_prev) s_a_down_ms = (int)s_ms_now;
            if (!a && s_a_prev) key_push_sym(((int)s_ms_now - s_a_down_ms) < dot_ms() * 2 ? '.' : '-');
        }
        s_a_prev = a; s_b_prev = b;
        if (s_key_pat_len > 0 && (s_ms_now - s_last_symbol_ms) > (uint32_t)(dot_ms() * 3)) key_flush_char();
    } else {
        s_a_prev = false; s_b_prev = false;
    }

    play_tick();

    static const gpio_num_t pads[6] = {KEY_UP_GPIO, KEY_DOWN_GPIO, KEY_LEFT_GPIO,
                                       KEY_RIGHT_GPIO, KEY_A_GPIO, KEY_B_GPIO};
    bool any = false;
    for (int i = 0; i < 6; ++i) if (gpio_get_level(pads[i]) == BUTTON_ACTIVE_LEVEL) { any = true; break; }
    led_apply(1, any);
    led_apply(2, s_play_busy || s_key_pat_len > 0);
}

/* ---------------- 按键扫描 ---------------- */
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
#define NVS_NS "morse"

static void settings_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    int32_t v = 0;
    if (nvs_get_i32(h, "mode", &v) == ESP_OK && v >= 0 && v < MODE_COUNT) s_mode = (int)v;
    if (nvs_get_i32(h, "wpm", &v) == ESP_OK) for (int i = 0; i < WPM_N; ++i) if (k_wpms[i] == v) { s_wpm_i = i; s_wpm = v; }
    if (nvs_get_i32(h, "dual", &v) == ESP_OK) s_dual = v ? 1 : 0;
    if (nvs_get_i32(h, "koch", &v) == ESP_OK && v >= 0 && v < KOCH_N) s_koch_i = (int)v;
    if (nvs_get_i32(h, "sound", &v) == ESP_OK) s_sound = v ? 1 : 0;
    nvs_close(h);
}

static void settings_save(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_i32(h, "mode", s_mode);
    nvs_set_i32(h, "wpm", s_wpm);
    nvs_set_i32(h, "dual", s_dual);
    nvs_set_i32(h, "koch", s_koch_i);
    nvs_set_i32(h, "sound", s_sound);
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
    lv_display_set_dpi(disp, LCD_DPI);
    lv_display_set_buffers(disp, b1, b2, buf_sz, LV_DISPLAY_RENDER_MODE_FULL);
    lv_draw_buf_init(&s_draw_buf3, LCD_H_RES, LCD_DRAW_BUF_LINES, cf, stride, b3, buf_sz);
    lv_display_set_3rd_draw_buffer(disp, &s_draw_buf3);
    lv_display_set_user_data(disp, io);
    lv_display_set_flush_cb(disp, flush_cb);
    lv_display_set_default(disp);
    ESP_LOGI(TAG, "LVGL display 160x128 ready");
    return disp;
}

/* ---------------- UI 组件 ---------------- */
static lv_obj_t *s_lbl_hdr, *s_lbl_right, *s_lbl_big, *s_lbl_code, *s_lbl_info;
static lv_obj_t *s_lbl_msg, *s_lbl_h1, *s_lbl_h2, *s_bar, *s_card;
static lv_obj_t *s_lbl_qtitle;          /* 听辨页卡片内的小标题 */
static lv_obj_t *s_cand[5];             /* 听辨页候选字格 */
static lv_obj_t *s_pip[MODE_COUNT];     /* header 模式指示点 */
static lv_obj_t *s_guide;               /* 键位引导子页 */
static lv_obj_t *s_rows;                /* 设置列表 */
static lv_obj_t *s_row[MODE_COUNT];     /* 设置列表 3 行 */
static lv_obj_t *s_row_name[MODE_COUNT];
static lv_obj_t *s_row_val[MODE_COUNT];
#define SET_ROWS 3

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

    /* header */
    mk_panel(scr, 0, 0, LCD_H_RES, HDR_H, C_HDR, 0, 0);
    mk_panel(scr, 0, HDR_H - 1, LCD_H_RES, 1, C_ACC_DIM, 0, 0);
    s_lbl_hdr = mk_label(scr, "", 6, 3, C_ACCENT, &ark12);

    for (int i = 0; i < MODE_COUNT; ++i) {
        s_pip[i] = mk_panel(scr, 46 + i * 9, 7, 5, 5, C_CARD2, 0, 2);
    }
    s_lbl_right = mk_label(scr, "", 96, 3, C_DIM, &ark12);
    lv_obj_set_width(s_lbl_right, 58);
    lv_obj_set_style_text_align(s_lbl_right, LV_TEXT_ALIGN_RIGHT, 0);

    /* 卡片 */
    s_card = mk_panel(scr, CARD_X, CARD_Y, CARD_W, CARD_H, C_CARD, C_LINE, 6);
    s_lbl_big  = mk_label(s_card, "", 4, 7, C_CREAM, &ark24);
    s_lbl_code = mk_label(s_card, "", 52, 7, C_ACCENT, &ark24);
    s_lbl_qtitle = mk_label(s_card, "听音选字", 4, 2, C_DIM, &ark12);
    for (int i = 0; i < 5; ++i) {
        s_cand[i] = mk_panel(s_card, 16 + i * 24, 15, 22, 18, C_CARD2, C_LINE, 3);
        lv_obj_t *l = lv_label_create(s_cand[i]);
        lv_label_set_text(l, "");
        lv_obj_set_style_text_font(l, &ark12, 0);
        lv_obj_set_style_text_color(l, lv_color_hex(C_TEXT), 0);
        lv_obj_center(l);
    }

    /* 信息 / 进度 / 状态 */
    s_lbl_info = mk_label(scr, "", PAD_X, INFO_Y, C_DIM, &ark12);
    lv_obj_set_width(s_lbl_info, LCD_H_RES - 2 * PAD_X);

    s_bar = lv_bar_create(scr);
    lv_obj_set_pos(s_bar, PAD_X, BAR_Y);
    lv_obj_set_size(s_bar, LCD_H_RES - 2 * PAD_X, BAR_H);
    lv_bar_set_range(s_bar, 0, 100);
    lv_obj_set_style_bg_color(s_bar, lv_color_hex(C_CARD2), 0);
    lv_obj_set_style_bg_color(s_bar, lv_color_hex(C_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_bar, 3, 0);
    lv_obj_set_style_radius(s_bar, 3, LV_PART_INDICATOR);

    s_lbl_msg = mk_label(scr, "", PAD_X, MSG_Y, C_CREAM, &ark12);
    lv_obj_set_width(s_lbl_msg, LCD_H_RES - 2 * PAD_X);
    s_lbl_h1 = mk_label(scr, "", PAD_X, H1_Y, C_DIM, &ark12);
    s_lbl_h2 = mk_label(scr, "", PAD_X, H2_Y, C_DIM, &ark12);

    /* 设置列表（3 行） */
    s_rows = lv_obj_create(scr);
    lv_obj_remove_style_all(s_rows);
    lv_obj_set_pos(s_rows, CARD_X, CARD_Y);
    lv_obj_set_size(s_rows, CARD_W, 3 * 22);
    lv_obj_set_style_bg_opa(s_rows, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(s_rows, LV_OBJ_FLAG_SCROLLABLE);
    static const char *row_names[SET_ROWS] = {"键位引导", "声音", "速率"};
    for (int i = 0; i < SET_ROWS; ++i) {
        s_row[i] = mk_panel(s_rows, 0, i * 22, CARD_W, 20, C_CARD, 0, 5);
        s_row_name[i] = mk_label(s_row[i], row_names[i], 8, 4, C_TEXT, &ark12);
        s_row_val[i]  = mk_label(s_row[i], "", 100, 4, C_CREAM, &ark12);
        lv_obj_set_width(s_row_val[i], CARD_W - 108);
        lv_obj_set_style_text_align(s_row_val[i], LV_TEXT_ALIGN_RIGHT, 0);
    }

    /* 键位引导子页 */
    s_guide = lv_obj_create(scr);
    lv_obj_remove_style_all(s_guide);
    lv_obj_set_pos(s_guide, 0, CARD_Y);
    lv_obj_set_size(s_guide, LCD_H_RES, LCD_V_RES - CARD_Y);
    lv_obj_set_style_bg_opa(s_guide, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(s_guide, LV_OBJ_FLAG_SCROLLABLE);
    static const char *cell_txt[9] = {NULL, "U", NULL, "L", "中", "R", NULL, "D", NULL};
    for (int i = 0; i < 9; ++i) {
        int col = i % 3, row = i / 3;
        lv_obj_t *c = mk_panel(s_guide, 45 + col * 24, 3 + row * 24, 22, 22, C_CARD, C_LINE, 4);
        if (cell_txt[i]) {
            lv_obj_t *l = lv_label_create(c);
            lv_label_set_text(l, cell_txt[i]);
            lv_obj_set_style_text_font(l, &ark12, 0);
            lv_obj_set_style_text_color(l, lv_color_hex(C_CREAM), 0);
            lv_obj_center(l);
        }
    }
    mk_label(s_guide, "上下:选择 左右:翻页", 6, 78, C_DIM, &ark12);
    mk_label(s_guide, "A:确认 B:返回", 6, 94, C_DIM, &ark12);
    lv_obj_add_flag(s_guide, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_rows, LV_OBJ_FLAG_HIDDEN);
}

/* 统一控制主内容/列表/引导的显隐——避免上次那种"切走还留着叠在一起" */
static void show_obj(lv_obj_t *o, bool show)
{
    if (!o) return;
    if (show) lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
    else      lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}

static void apply_view(void)
{
    const bool is_set = (s_mode == MODE_SET);
    const bool guide  = is_set && s_set_view == 1;
    const bool rows   = is_set && s_set_view == 0;
    const bool main   = !is_set;

    show_obj(s_card, main);
    show_obj(s_lbl_info, main || rows);
    show_obj(s_bar, main || rows);
    show_obj(s_lbl_msg, main || rows);
    show_obj(s_rows, rows);
    show_obj(s_guide, guide);

    /* 卡片里按模式切换子元素 */
    const bool type_mode = (s_mode == MODE_TYPE);
    show_obj(s_lbl_big, main && !type_mode);
    show_obj(s_lbl_code, main && !type_mode);
    show_obj(s_lbl_qtitle, main && type_mode);
    for (int i = 0; i < 5; ++i) show_obj(s_cand[i], main && type_mode);

    /* 提示行：引导子页只留 B返回 */
    if (guide) {
        lv_label_set_text(s_lbl_h1, "B 返回");
        lv_label_set_text(s_lbl_h2, "");
    }
}

/* ---------------- 听辨页 ---------------- */
static int type_set_size(void)
{
    int n = s_koch_i + 1;
    if (n < 5) n = 5;
    if (n > KOCH_N) n = KOCH_N;
    return n;
}

static void type_new_question(void)
{
    const int n = type_set_size();
    s_type_ans = k_koch[esp_random() % (uint32_t)n];
    s_type_sel_i = 0;                     /* 光标回到集合首位 */
    s_type_sel = k_koch[0];
    s_type_state = 0;
    morse_play(morse_code_of(s_type_ans));
}

static void type_move(int delta)
{
    const int n = type_set_size();
    s_type_sel_i = (s_type_sel_i + delta + n * 4) % n;
    s_type_sel = k_koch[s_type_sel_i];
}

/* ---------------- 刷新 ---------------- */
static void ui_refresh(void)
{
    char buf[96];

    apply_view();

    lv_label_set_text(s_lbl_hdr, k_mode_name[s_mode]);
    for (int i = 0; i < MODE_COUNT; ++i) {
        lv_obj_set_style_bg_color(s_pip[i],
            lv_color_hex(i == s_mode ? C_ACCENT : C_CARD2), 0);
    }

    if (s_mode == MODE_KEY)      snprintf(buf, sizeof(buf), "%s", s_dual ? "双键" : "单键");
    else if (!s_sound)           snprintf(buf, sizeof(buf), "静音");
    else                         snprintf(buf, sizeof(buf), "%dWPM", s_wpm);
    lv_label_set_text(s_lbl_right, buf);

    switch (s_mode) {
    case MODE_LOOKUP: {
        char ch = k_morse[s_look_i].ch;
        buf[0] = ch; buf[1] = 0;
        lv_label_set_text(s_lbl_big, s_look_hide ? "?" : buf);
        lv_label_set_text(s_lbl_code, k_morse[s_look_i].code);
        snprintf(buf, sizeof(buf), "%02d/%d  %s", s_look_i + 1, MORSE_N, s_look_hide ? "隐藏" : "显示");
        lv_label_set_text(s_lbl_info, buf);
        lv_bar_set_value(s_bar, (s_look_i + 1) * 100 / MORSE_N, LV_ANIM_OFF);
        lv_label_set_text(s_lbl_msg, s_play_busy ? "发送中" : "");
        lv_label_set_text(s_lbl_h1, "长按LR换模式");
        lv_label_set_text(s_lbl_h2, "长按A调速");
        break;
    }
    case MODE_LEARN: {
        char ch = k_koch[s_koch_i];
        buf[0] = ch; buf[1] = 0;
        lv_label_set_text(s_lbl_big, buf);
        lv_label_set_text(s_lbl_code, morse_code_of(ch) ? morse_code_of(ch) : "?");
        snprintf(buf, sizeof(buf), "顺序 %02d/%d", s_koch_i + 1, KOCH_N);
        lv_label_set_text(s_lbl_info, buf);
        lv_bar_set_value(s_bar, (s_koch_i + 1) * 100 / KOCH_N, LV_ANIM_OFF);
        lv_label_set_text(s_lbl_msg, s_play_busy ? "发送中" : "");
        lv_label_set_text(s_lbl_h1, "长按LR换模式");
        lv_label_set_text(s_lbl_h2, "长按A调速");
        break;
    }
    case MODE_TYPE: {
        const int n = type_set_size();
        /* 中间格 = 当前选择，左右各两格预览 */
        for (int i = 0; i < 5; ++i) {
            int off = i - 2;
            int idx = ((s_type_sel_i + off) % n + n) % n;
            char c = k_koch[idx];
            char t[2] = {c, 0};
            lv_obj_t *l = lv_obj_get_child(s_cand[i], 0);
            lv_label_set_text(l, t);
            const bool sel = (off == 0);
            lv_obj_set_style_bg_color(s_cand[i], lv_color_hex(sel ? C_ACCENT : C_CARD2), 0);
            lv_obj_set_style_border_color(s_cand[i], lv_color_hex(sel ? C_ACCENT : C_LINE), 0);
            lv_obj_set_style_text_color(l, lv_color_hex(sel ? 0x0B1014 : C_DIM), 0);
        }
        snprintf(buf, sizeof(buf), "对 %d  错 %d  连 %d", s_type_ok, s_type_bad, s_type_streak);
        lv_label_set_text(s_lbl_info, buf);
        {
            int tot = s_type_ok + s_type_bad;
            lv_bar_set_value(s_bar, tot ? (s_type_ok * 100 / tot) : 0, LV_ANIM_OFF);
        }
        if (s_type_state == 0) {
            lv_label_set_text(s_lbl_msg, s_play_busy ? "正在播放…" : "听不清？B 重播");
            lv_obj_set_style_text_color(s_lbl_msg, lv_color_hex(C_DIM), 0);
            lv_label_set_text(s_lbl_h1, "B重播");
        } else {
            const int ok = (s_type_sel == s_type_ans);
            if (ok) {
                lv_label_set_text(s_lbl_msg, "正确");
            } else {
                snprintf(buf, sizeof(buf), "错了 答案 %c", s_type_ans);
                lv_label_set_text(s_lbl_msg, buf);
            }
            lv_obj_set_style_text_color(s_lbl_msg, lv_color_hex(ok ? C_ACCENT : C_ERR), 0);
            lv_label_set_text(s_lbl_h1, "");
        }
        lv_label_set_text(s_lbl_h2, "长按LR换模式 长按A调速");
        break;
    }
    case MODE_KEY: {
        lv_label_set_text(s_lbl_big, s_key_pat_len ? s_key_pat : "");
        if (s_key_pat_len) {
            buf[0] = morse_decode(s_key_pat); buf[1] = 0;
            lv_label_set_text(s_lbl_code, buf);
        } else {
            lv_label_set_text(s_lbl_code, "");
        }
        snprintf(buf, sizeof(buf), "文本 %s", s_key_text_len ? s_key_text : "—");
        lv_label_set_text(s_lbl_info, buf);
        lv_bar_set_value(s_bar, s_key_pat_len * 12, LV_ANIM_OFF);
        lv_label_set_text(s_lbl_msg, s_dual ? "双键：A 点   B 划" : "单键：按住 A 发报");
        lv_label_set_text(s_lbl_h1, s_dual ? "" : "B 清空");
        lv_label_set_text(s_lbl_h2, "长按U单双键 长按LR换模式");
        break;
    }
    case MODE_SET: {
        if (s_set_view == 1) break;      /* 引导子页由 apply_view 处理 */
        for (int i = 0; i < SET_ROWS; ++i) {
            const bool sel = (i == s_set_i);
            lv_obj_set_style_bg_color(s_row[i], lv_color_hex(sel ? C_CARD2 : C_CARD), 0);
            lv_obj_set_style_text_color(s_row_name[i], lv_color_hex(sel ? C_ACCENT : C_TEXT), 0);
        }
        lv_label_set_text(s_row_val[0], ">");
        lv_label_set_text(s_row_val[1], s_sound ? "开" : "关");
        snprintf(buf, sizeof(buf), "%dWPM", s_wpm);
        lv_label_set_text(s_row_val[2], buf);
        snprintf(buf, sizeof(buf), "点长 %dms  ·  %d/6 档", dot_ms(), s_wpm_i + 1);
        lv_label_set_text(s_lbl_info, buf);
        lv_bar_set_value(s_bar, (s_set_i + 1) * 100 / SET_ROWS, LV_ANIM_OFF);
        lv_label_set_text(s_lbl_msg, "");
        lv_label_set_text(s_lbl_h1, "上下选 左右调 A确认");
        lv_label_set_text(s_lbl_h2, "长按LR换模式");
        break;
    }
    default: break;
    }
}

/* ---------------- 按键行为 ---------------- */
static void mode_step(int d)
{
    s_mode = (s_mode + d + MODE_COUNT) % MODE_COUNT;
    s_set_view = 0;
    if (s_mode == MODE_TYPE) type_new_question();
    if (s_mode == MODE_LEARN) morse_play(morse_code_of(k_koch[s_koch_i]));
    if (s_mode == MODE_KEY) key_clear();
    settings_save();
    ESP_LOGI(TAG, "mode -> %s", k_mode_ascii[s_mode]);
}

static void key_short(int idx)
{
    switch (s_mode) {
    case MODE_LOOKUP:
        if (idx == 0) s_look_i = (s_look_i - 1 + MORSE_N) % MORSE_N;
        else if (idx == 1) s_look_i = (s_look_i + 1) % MORSE_N;
        else if (idx == 2) s_look_i = (s_look_i - 10 + MORSE_N * 2) % MORSE_N;
        else if (idx == 3) s_look_i = (s_look_i + 10) % MORSE_N;
        else if (idx == 4) morse_play(k_morse[s_look_i].code);
        else if (idx == 5) s_look_hide = !s_look_hide;
        break;

    case MODE_LEARN:
        if (idx == 0 || idx == 1) {
            s_koch_i = (idx == 0) ? (s_koch_i + 1) % KOCH_N : (s_koch_i - 1 + KOCH_N) % KOCH_N;
            settings_save();
            morse_play(morse_code_of(k_koch[s_koch_i]));
        } else if (idx == 4) {
            morse_play(morse_code_of(k_koch[s_koch_i]));
        } else if (idx == 5) {
            s_koch_i = 0;
            settings_save();
        }
        break;

    case MODE_TYPE:
        if (idx == 4) {
            if (s_type_state == 0) {
                s_type_state = 1;
                s_type_fb_ms = lv_tick_get();
                if (s_type_sel == s_type_ans) { s_type_ok++; s_type_streak++; }
                else { s_type_bad++; s_type_streak = 0; }
            } else {
                type_new_question();
            }
        } else if (idx == 5) {
            morse_play(morse_code_of(s_type_ans));
        } else if (s_type_state == 0) {
            if (idx == 0) type_move(-1);
            else if (idx == 1) type_move(+1);
            else if (idx == 2) type_move(-5);
            else if (idx == 3) type_move(+5);
        }
        break;

    case MODE_KEY:
        if (idx == 5 && !s_dual) key_clear();
        break;

    case MODE_SET:
        if (s_set_view == 1) {                       /* 引导子页：任意键返回 */
            if (idx == 5 || idx == 4) s_set_view = 0;
            break;
        }
        if (idx == 0) s_set_i = (s_set_i + SET_ROWS - 1) % SET_ROWS;
        else if (idx == 1) s_set_i = (s_set_i + 1) % SET_ROWS;
        else if (idx == 4) {
            if (s_set_i == 0) s_set_view = 1;
            else if (s_set_i == 1) { s_sound = !s_sound; if (!s_sound) buz_off(); settings_save(); }
            else { s_wpm_i = (s_wpm_i + 1) % WPM_N; s_wpm = k_wpms[s_wpm_i]; settings_save(); }
        } else if (idx == 2) {
            if (s_set_i == 1) { s_sound = 0; buz_off(); settings_save(); }
            else if (s_set_i == 2) { s_wpm_i = (s_wpm_i - 1 + WPM_N) % WPM_N; s_wpm = k_wpms[s_wpm_i]; settings_save(); }
        } else if (idx == 3) {
            if (s_set_i == 1) { s_sound = 1; settings_save(); }
            else if (s_set_i == 2) { s_wpm_i = (s_wpm_i + 1) % WPM_N; s_wpm = k_wpms[s_wpm_i]; settings_save(); }
        } else if (idx == 5) {
            mode_step(-1);
        }
        break;
    default: break;
    }
}

static void key_long(int idx)
{
    if (idx == 2) { mode_step(-1); return; }
    if (idx == 3) { mode_step(+1); return; }

    if (idx == 4 && s_mode != MODE_KEY) {
        s_wpm_i = (s_wpm_i + 1) % WPM_N;
        s_wpm = k_wpms[s_wpm_i];
        settings_save();
        ESP_LOGI(TAG, "wpm -> %d (dot %d ms)", s_wpm, dot_ms());
        return;
    }
    if (idx == 0 && s_mode == MODE_KEY) {
        s_dual = !s_dual;
        key_clear();
        settings_save();
        ESP_LOGI(TAG, "key mode -> %s", s_dual ? "DUAL" : "SINGLE");
        return;
    }
}

/* ---------------- 字体自检（自动判定） ---------------- */
static int utf8_next(const char *s, uint32_t *cp)
{
    const uint8_t *p = (const uint8_t *)s;
    if (p[0] < 0x80) { *cp = p[0]; return 1; }
    if ((p[0] & 0xE0) == 0xC0) { *cp = ((p[0] & 0x1F) << 6) | (p[1] & 0x3F); return 2; }
    if ((p[0] & 0xF0) == 0xE0) {
        *cp = ((p[0] & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
        return 3;
    }
    *cp = p[0];
    return 1;
}

static int glyph_px(const lv_font_t *font, const char *s, int *out_box)
{
    uint32_t cp = 0;
    utf8_next(s, &cp);
    lv_font_glyph_dsc_t dsc;
    memset(&dsc, 0, sizeof(dsc));
    if (!lv_font_get_glyph_dsc(font, &dsc, cp, 0)) return -1;
    static uint8_t a8buf[64 * 64];
    lv_draw_buf_t db;
    lv_draw_buf_init(&db, 64, 64, LV_COLOR_FORMAT_A8, 64, a8buf, sizeof(a8buf));
    const void *bm = lv_font_get_glyph_bitmap(&dsc, &db);
    int px = 0;
    uint32_t st = lv_draw_buf_width_to_stride(dsc.box_w, LV_COLOR_FORMAT_A8);
    const uint8_t *pix = NULL;
    if (bm == &db) pix = (const uint8_t *)db.data;
    else if (bm) pix = (const uint8_t *)bm;
    if (pix) {
        for (int y = 0; y < dsc.box_h; ++y)
            for (int x = 0; x < dsc.box_w; ++x)
                if (pix[y * st + x] > 127) px++;
    }
    if (out_box) *out_box = dsc.box_w * 100 + dsc.box_h;
    return px;
}

static void font_selftest(void)
{
    static const char *p12 = "A0.长按码";
    static const char *p24 = "A0.-";
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
    for (const char *p = p24; *p; ) {
        char one[2] = {*p, 0};
        int px = glyph_px(&ark24, one, &box);
        ESP_LOGI(TAG, "ark24 '%c' box=%dx%d px=%d", *p, box / 100, box % 100, px);
        if (px > 0) ok++; else bad++;
        p++;
    }
    ESP_LOGI(TAG, "font selftest: %d ok, %d bad -> %s", ok, bad, bad ? "FAIL" : "PASS");
}

/* ---------------- 主循环 ---------------- */
static void app_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "Start morse trainer UI");
    ui_create();
    s_first_flush_done = false;
    lv_refr_now(NULL);
    for (int i = 0; i < 100 && !s_first_flush_done; ++i) vTaskDelay(pdMS_TO_TICKS(1));
    lcd_display_on();

    if (s_mode == MODE_LEARN) morse_play(morse_code_of(k_koch[s_koch_i]));

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

        if (s_mode == MODE_TYPE && s_type_state == 1 &&
            lv_tick_elaps(s_type_fb_ms) >= 1600 && !s_play_busy) {
            type_new_question();
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
    ESP_LOGI(TAG, "Xiaomiao Morse trainer boot");

    buttons_init();
    buz_init();

    esp_err_t nvserr = nvs_flash_init();
    if (nvserr == ESP_ERR_NVS_NO_FREE_PAGES || nvserr == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvserr = nvs_flash_init();
    }
    if (nvserr == ESP_OK) {
        settings_load();
        ESP_LOGI(TAG, "settings: mode=%s wpm=%d dual=%d sound=%d koch=%d",
                 k_mode_ascii[s_mode], s_wpm, s_dual, s_sound, s_koch_i);
    } else {
        ESP_LOGW(TAG, "nvs init failed (%s)", esp_err_to_name(nvserr));
    }

    gd32_init();

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

    const esp_timer_create_args_t tk2 = { .callback = tick_1ms, .name = "morse_1ms" };
    esp_timer_handle_t t2 = NULL;
    ESP_ERROR_CHECK(esp_timer_create(&tk2, &t2));
    ESP_ERROR_CHECK(esp_timer_start_periodic(t2, 1000));

    s_ms_now = 0;
    key_clear();

    BaseType_t ok = xTaskCreate(app_task, "morse", LVGL_TASK_STACK_SIZE, NULL, LVGL_TASK_PRIORITY, NULL);
    ESP_ERROR_CHECK(ok == pdPASS ? ESP_OK : ESP_FAIL);
}
