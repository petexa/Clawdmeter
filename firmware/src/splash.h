#pragma once
#include <stdint.h>
#include <lvgl.h>

// Allocate the canvas pixel buffer. Call BEFORE ble_init() so BLE doesn't
// exhaust the heap first. Returns true on success.
bool splash_preinit(void);

// Initialize splash module. Creates the canvas widget inside `parent`.
// splash_preinit() must be called first to secure the buffer.
void splash_init(lv_obj_t *parent);

// Advance animation frame if hold time elapsed. Call from main loop.
void splash_tick(void);

// Cycle to the next animation in the catalog.
void splash_next(void);

// Show/hide the splash container.
void splash_show(void);
void splash_hide(void);

// Pick the next animation matching the current usage-rate group.
// Called automatically by splash_show(); also exposed so other modules can
// trigger a re-pick when the rate group changes mid-display.
void splash_pick_for_current_rate(void);

// True when splash is currently rendering (used to gate re-picks).
bool splash_is_active(void);

// Root container (so ui.cpp can attach a click event).
lv_obj_t* splash_get_root(void);

// Update the time/date overlays shown around the sprite.
void splash_update_time(const char* time_h, const char* time_d);
