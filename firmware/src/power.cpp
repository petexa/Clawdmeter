#include "power.h"
#include <Arduino.h>

// S3-BOX-3: no AXP2101 PMU. LCD runs on 3.3V system rail always-on.
// Mute button (GPIO1) acts as middle button to cycle screens.
#define BTN_MID 1

static bool pwr_pressed_flag = false;
static bool btn_prev = false;

void power_init(void) {
    pinMode(BTN_MID, INPUT_PULLUP);
    // No PMU to init — LCD/touch power is always-on 3.3V from USB.
}

void power_tick(void) {
    bool btn_now = (digitalRead(BTN_MID) == LOW);
    if (btn_now && !btn_prev) pwr_pressed_flag = true;
    btn_prev = btn_now;
}

int  power_battery_pct(void)  { return -1; }
bool power_is_charging(void)  { return false; }

bool power_pwr_pressed(void) {
    if (pwr_pressed_flag) { pwr_pressed_flag = false; return true; }
    return false;
}
