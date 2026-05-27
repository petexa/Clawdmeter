#include <Arduino.h>
#include <lvgl.h>
#include "driver/gpio.h"
#include <ArduinoJson.h>
#include "display_cfg.h"
#include "data.h"
#include "ui.h"
#include "ble.h"
#include "power.h"
#include "imu.h"
#include "splash.h"
#include "usage_rate.h"

// Physical buttons:
//   BTN_BACK (GPIO 0) — Boot button, hold to send Space (voice push-to-talk)
//   BTN_MID  (GPIO 1) — Mute button, press to cycle screens (via power.cpp)
#define BTN_BACK 0

// ---- Bit-bang SPI (bypasses ESP32 SPI peripheral entirely) ---------------
// Hardware SPI (SPI2/SPI3) never produced any color change.
// Bit-bang directly tests whether GPIO6/7/5/4 are wired to the display.

static inline void cs_lo()   { gpio_set_level((gpio_num_t)LCD_CS,   0); }
static inline void cs_hi()   { gpio_set_level((gpio_num_t)LCD_CS,   1); }
static inline void dc_cmd()  { gpio_set_level((gpio_num_t)LCD_DC,   0); }
static inline void dc_data() { gpio_set_level((gpio_num_t)LCD_DC,   1); }

static void bb_send_byte(uint8_t b) {
    for (int i = 7; i >= 0; i--) {
        gpio_set_level((gpio_num_t)LCD_SCLK, 0);
        gpio_set_level((gpio_num_t)LCD_MOSI, (b >> i) & 1);
        gpio_set_level((gpio_num_t)LCD_SCLK, 1);
    }
    gpio_set_level((gpio_num_t)LCD_SCLK, 0);
}

static void lcd_cmd(uint8_t cmd) {
    dc_cmd(); cs_lo();
    bb_send_byte(cmd);
    cs_hi();
}

static void lcd_cmd_param(uint8_t cmd, const uint8_t* d, size_t n) {
    dc_cmd(); cs_lo();
    bb_send_byte(cmd);
    dc_data();
    for (size_t i = 0; i < n; i++) bb_send_byte(d[i]);
    cs_hi();
}

static void lcd_write_pixels(const uint8_t* px, size_t len) {
    dc_cmd(); cs_lo();
    bb_send_byte(0x2C);  // RAMWR
    dc_data();
    for (size_t i = 0; i < len; i++) bb_send_byte(px[i]);
    cs_hi();
}

// ---- ILI9342C init -------------------------------------------------------

// Minimal init compatible with both ST7789 and ILI9342C.
// Espressif BOX-3 BSP calls esp_lcd_panel_invert_color(true) after init.
static void lcd_init(void) {
    // BOX-3 reset is active-HIGH (GPIO48 HIGH = assert reset, LOW = normal).
    // Previous code had this backwards — commands were sent while in reset.
    gpio_reset_pin((gpio_num_t)LCD_RESET);
    gpio_set_direction((gpio_num_t)LCD_RESET, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)LCD_RESET, 1); delay(10);   // assert reset
    gpio_set_level((gpio_num_t)LCD_RESET, 0); delay(120);  // release, stabilise

    lcd_cmd(0x01); delay(150);  // SWRESET
    lcd_cmd(0x11); delay(500);  // SLPOUT

    { const uint8_t d[] = {0x55}; lcd_cmd_param(0x3A, d, 1); } // COLMOD: 16-bit RGB565
    { const uint8_t d[] = {0xC8}; lcd_cmd_param(0x36, d, 1); } // MADCTL: MY|MX|BGR — matches BSP mirror(true,true)

    lcd_cmd(0x29);  // DISPON
    delay(100);
}

// ---- Diagnostic: fill entire screen with one color -----------------------
// Sends CASET, RASET, then RAMWR + solid fill row-by-row.
// Used to confirm SPI reaches the panel before LVGL starts.

static void lcd_fill(uint16_t rgb565_be) {
    { const uint8_t d[] = {0x00, 0x00, 0x01, 0x3F}; lcd_cmd_param(0x2A, d, 4); }
    { const uint8_t d[] = {0x00, 0x00, 0x00, 0xEF}; lcd_cmd_param(0x2B, d, 4); }
    uint8_t hi = rgb565_be >> 8, lo = rgb565_be & 0xFF;
    dc_cmd(); cs_lo();
    bb_send_byte(0x2C);  // RAMWR
    dc_data();
    for (int r = 0; r < 240; r++)
        for (int c = 0; c < 320; c++) { bb_send_byte(hi); bb_send_byte(lo); }
    cs_hi();
}

// ---- Touch driver object (init skipped — GPIO48 conflict with LCD_RESET) --
TouchDrvGT911 touch;

static UsageData usage = {};

// Touch state (no ISR — touch not yet initialized)
static volatile bool     touch_pressed = false;
static volatile uint16_t touch_x = 0;
static volatile uint16_t touch_y = 0;

// ---- LVGL render buffers (PSRAM-backed, partial render) ------------------
#define BUF_LINES 20
static uint16_t *buf1 = nullptr;
static uint16_t *buf2 = nullptr;

static uint32_t my_tick(void) { return millis(); }

// ---- LVGL flush via raw SPI ----------------------------------------------

static int flush_count = 0;

static void my_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
    const int32_t x1 = area->x1, y1 = area->y1, x2 = area->x2, y2 = area->y2;
    if (flush_count < 5) {
        Serial.printf("flush#%d (%d,%d)-(%d,%d)\n", flush_count, x1, y1, x2, y2);
        flush_count++;
    }

    const uint8_t caset[4] = {(uint8_t)(x1>>8),(uint8_t)x1,(uint8_t)(x2>>8),(uint8_t)x2};
    const uint8_t raset[4] = {(uint8_t)(y1>>8),(uint8_t)y1,(uint8_t)(y2>>8),(uint8_t)y2};
    lcd_cmd_param(0x2A, caset, 4);
    lcd_cmd_param(0x2B, raset, 4);
    lcd_write_pixels(px_map, (size_t)(x2 - x1 + 1) * (y2 - y1 + 1) * 2);

    lv_display_flush_ready(disp);
}

static void my_touch_cb(lv_indev_t *indev, lv_indev_data_t *data) {
    if (touch_pressed) {
        data->point.x = touch_x;
        data->point.y = touch_y;
        data->state   = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

// ---- JSON parse ----------------------------------------------------------

static bool parse_json(const char *json, UsageData *out) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, json);
    if (err) { Serial.printf("JSON parse error: %s\n", err.c_str()); return false; }
    out->session_pct        = doc["s"]  | 0.0f;
    out->session_reset_mins = doc["sr"] | -1;
    out->weekly_pct         = doc["w"]  | 0.0f;
    out->weekly_reset_mins  = doc["wr"] | -1;
    strlcpy(out->status, doc["st"] | "unknown", sizeof(out->status));
    strlcpy(out->time_h, doc["th"] | "",        sizeof(out->time_h));
    strlcpy(out->time_d, doc["td"] | "",        sizeof(out->time_d));
    out->ok    = doc["ok"] | false;
    out->valid = true;
    return true;
}

// ---- Serial command: screenshot ------------------------------------------

#define CMD_BUF_SIZE 64
static char cmd_buf[CMD_BUF_SIZE];
static int  cmd_pos = 0;

static void send_screenshot() {
    const uint32_t w = LCD_WIDTH, h = LCD_HEIGHT;
    const uint32_t buf_size = w * h * 2;
    uint8_t *sbuf = (uint8_t*)heap_caps_malloc(buf_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!sbuf) { Serial.println("SCREENSHOT_ERR"); return; }
    lv_draw_buf_t draw_buf;
    lv_draw_buf_init(&draw_buf, w, h, LV_COLOR_FORMAT_RGB565, w * 2, sbuf, buf_size);
    lv_result_t res = lv_snapshot_take_to_draw_buf(lv_screen_active(),
                                                    LV_COLOR_FORMAT_RGB565, &draw_buf);
    if (res != LV_RESULT_OK) { heap_caps_free(sbuf); Serial.println("SCREENSHOT_ERR"); return; }
    Serial.printf("SCREENSHOT_START %lu %lu %lu\n",
                  (unsigned long)w, (unsigned long)h, (unsigned long)buf_size);
    Serial.flush();
    Serial.write(sbuf, buf_size);
    Serial.flush();
    Serial.println();
    Serial.println("SCREENSHOT_END");
    heap_caps_free(sbuf);
}

static void check_serial_cmd() {
    while (Serial.available()) {
        char c = Serial.read();
        if (c == '\n' || c == '\r') {
            cmd_buf[cmd_pos] = '\0';
            if (strcmp(cmd_buf, "screenshot") == 0) send_screenshot();
            else if (strcmp(cmd_buf, "clearbonds") == 0) { ble_clear_bonds(); Serial.println("bonds cleared"); }
            cmd_pos = 0;
        } else if (cmd_pos < CMD_BUF_SIZE - 1) {
            cmd_buf[cmd_pos++] = c;
        }
    }
}

// ---- setup ---------------------------------------------------------------

void setup() {
    Serial.begin(115200);
    delay(300);
    Serial.println("{\"ready\":true}");

    Wire.begin(IIC_SDA, IIC_SCL);
    power_init();
    imu_init();

    // Release LCD SPI pins from PSRAM SPI0 IO MUX — Espressif BSP does this
    // explicitly; without it gpio_set_level is silently ignored on GPIO4-7.
    gpio_reset_pin((gpio_num_t)LCD_CS);
    gpio_reset_pin((gpio_num_t)LCD_DC);
    gpio_reset_pin((gpio_num_t)LCD_MOSI);
    gpio_reset_pin((gpio_num_t)LCD_SCLK);

    // Manual CS and DC GPIO setup (SPI device uses spics_io_num = -1)
    gpio_set_direction((gpio_num_t)LCD_CS, GPIO_MODE_OUTPUT);
    gpio_set_direction((gpio_num_t)LCD_DC, GPIO_MODE_OUTPUT);
    cs_hi();     // CS idle HIGH
    dc_data();   // DC idle HIGH (data)

    // Bit-bang SPI — MOSI and SCLK as outputs (CS and DC already configured above)
    gpio_set_direction((gpio_num_t)LCD_MOSI, GPIO_MODE_OUTPUT);
    gpio_set_direction((gpio_num_t)LCD_SCLK, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)LCD_SCLK, 0);
    gpio_set_level((gpio_num_t)LCD_MOSI, 0);
    // Display hardware init (ST7789 / ILI9342C minimal sequence)
    lcd_init();
    Serial.println("LCD init OK");

    // Backlight ON
    gpio_config_t bl_cfg = {};
    bl_cfg.pin_bit_mask = (1ULL << LCD_BL);
    bl_cfg.mode         = GPIO_MODE_OUTPUT;
    gpio_config(&bl_cfg);
    gpio_set_level((gpio_num_t)LCD_BL, 1);
    Serial.printf("BL GPIO%d HIGH\n", LCD_BL);
    delay(50);

    // ---- Diagnostic fill ------------------------------------------------
    // RED  = 0xF800 (RGB565 big-endian). Expect:
    //   - RED on screen  → INVON working, SPI OK
    //   - CYAN on screen → SPI OK, INVON not applied (panel inverted)
    //   - WHITE          → SPI not reaching panel at all
    Serial.println("DIAG: red fill");
    lcd_fill(0xF800);
    delay(3000);

    // Clear to black before LVGL takes over
    lcd_fill(0x0000);
    // ---- End diagnostic --------------------------------------------------

    // LVGL
    lv_init();
    lv_tick_set_cb(my_tick);

    buf1 = (uint16_t*)heap_caps_malloc(LCD_WIDTH * BUF_LINES * 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    buf2 = (uint16_t*)heap_caps_malloc(LCD_WIDTH * BUF_LINES * 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    Serial.printf("buf1=%p buf2=%p\n", buf1, buf2);

    lv_display_t *lvgl_disp = lv_display_create(LCD_WIDTH, LCD_HEIGHT);
    // RGB565_SWAPPED = big-endian byte order; ILI9342C expects MSB first over SPI
    lv_display_set_color_format(lvgl_disp, LV_COLOR_FORMAT_RGB565_SWAPPED);
    lv_display_set_flush_cb(lvgl_disp, my_flush_cb);
    lv_display_set_buffers(lvgl_disp, buf1, buf2, LCD_WIDTH * BUF_LINES * 2,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);

    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, my_touch_cb);

    ble_init();
    pinMode(BTN_BACK, INPUT_PULLUP);

    ui_init();
    ui_update_ble_status(ble_get_state(), ble_get_device_name(), ble_get_mac_address());
    ui_update_battery(power_battery_pct(), power_is_charging());
    ui_show_screen(SCREEN_USAGE);

    Serial.println("Dashboard ready, waiting for data on BLE...");
}

void loop() {
    lv_timer_handler();
    ui_tick_anim();
    ble_tick();
    power_tick();
    imu_tick();
    splash_tick();

    static uint32_t last_auto_toggle = 0;

    {
        static bool back_was = false;
        bool back_now = (digitalRead(BTN_BACK) == LOW);
        if (back_now != back_was) {
            if (back_now) ble_keyboard_press(0x2C, 0);
            else          ble_keyboard_release();
            back_was = back_now;
        }
        if (power_pwr_pressed()) {
            ui_toggle_splash();
            last_auto_toggle = millis();
        }
    }

    // Auto-switch between usage and splash every 30s
    {
        const uint32_t AUTO_TOGGLE_MS = 30000;
        uint32_t now = millis();
        if (now - last_auto_toggle >= AUTO_TOGGLE_MS) {
            last_auto_toggle = now;
            ui_toggle_splash();
        }
    }

    static ble_state_t last_ble_state = BLE_STATE_INIT;
    ble_state_t bs = ble_get_state();
    if (bs != last_ble_state) {
        last_ble_state = bs;
        ui_update_ble_status(bs, ble_get_device_name(), ble_get_mac_address());
    }

    static int  last_pct      = -2;
    static bool last_charging = false;
    int  pct      = power_battery_pct();
    bool charging = power_is_charging();
    if (pct != last_pct || charging != last_charging) {
        last_pct      = pct;
        last_charging = charging;
        ui_update_battery(pct, charging);
    }

    check_serial_cmd();

    if (ble_has_data()) {
        if (parse_json(ble_get_data(), &usage)) {
            int g_before = usage_rate_group();
            usage_rate_sample(usage.session_pct);
            int g_after = usage_rate_group();
            if (g_after != g_before) {
                Serial.printf("usage rate: group %d -> %d (s=%.2f%%)\n",
                    g_before, g_after, usage.session_pct);
                if (splash_is_active()) splash_pick_for_current_rate();
            }
            ui_update(&usage);
            ble_send_ack();
        } else {
            ble_send_nack();
        }
    }

    delay(5);
}
