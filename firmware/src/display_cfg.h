#pragma once

#include <TouchDrvGT911.hpp>
#include <Wire.h>

// ---- Display resolution ----
#define LCD_WIDTH   320
#define LCD_HEIGHT  240

// ---- SPI display pins (ILI9342C) ----
#define LCD_CS      5
#define LCD_DC      4
#define LCD_MOSI    6
#define LCD_SCLK    7
#define LCD_BL      47
#define LCD_RESET   48

// ---- Touch pins (GT911 via I2C) ----
#define IIC_SDA     8
#define IIC_SCL     18
#define TP_INT      3
#define TP_RST      48

// ---- GT911 I2C address (INT low during reset → 0x5D) ----
#define GT911_ADDR  0x5D

// ---- Touch driver (defined in main.cpp; init skipped — GPIO48 conflict) ----
extern TouchDrvGT911 touch;
