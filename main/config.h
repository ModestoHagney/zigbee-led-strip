#pragma once

#include "sdkconfig.h"

// -------------------------------------------------------
// BOARD CONFIG - picked by chip target (idf.py set-target)
// -------------------------------------------------------

#if CONFIG_IDF_TARGET_ESP32H2
#define LED_GPIO           GPIO_NUM_22
#define LED_COUNT          64
#define MODEL_IDENTIFIER   "\x06" "H2-LED"
#define OTA_IMAGE_TYPE     0x0001
#elif CONFIG_IDF_TARGET_ESP32C6
#define LED_GPIO           GPIO_NUM_19
#define LED_COUNT          144
#define MODEL_IDENTIFIER   "\x06" "C6-LED"
#define OTA_IMAGE_TYPE     0x0002
#else
#error "No board config for this target - add LED_GPIO, LED_COUNT, MODEL_IDENTIFIER and OTA_IMAGE_TYPE"
#endif

// -------------------------------------------------------
// FIRMWARE VERSION - for wireless updates
// -------------------------------------------------------

// Raise this for every build you send out as a wireless update.
// Home Assistant only offers a file with a higher number than the
// board is running. Use the same number for tools/make_ota.py.
#define FW_VERSION         1

// Has to match the --manufacturer value given to tools/make_ota.py
#define OTA_MANUFACTURER   0x1001

// -------------------------------------------------------
// LED STRIP CONFIG
// -------------------------------------------------------

#define LED_ENDPOINT     1

// Everything from here to the end of this section is only the default
// for a freshly flashed board. Each one is a setting on the device page
// in Home Assistant and is kept on the board after that.

// LED_COUNT (above) is the default count. LED_COUNT_MAX is the most
// the setting will accept.
#define LED_COUNT_MAX    1024

// Strip colour order: 0 GRB, 1 RGB, 2 BRG, 3 RBG, 4 GBR, 5 BGR
#define LED_COLOR_ORDER  0

// LED type: 0 = RGB (WS2812), 1 = RGBW (SK6812 with a white LED)
#define LED_TYPE         0

// Most current the strip may draw, in mA. The firmware dims the strip
// to stay under it. 0 = no limit.
#define LED_MAX_CURRENT  0

// White balance, percent per channel, so white matches other bulbs.
// WS2812 green and blue run strong; these are the usual starting values
// for 5050 strips. Tune by eye.
#define LED_WHITE_R      100
#define LED_WHITE_G      69
#define LED_WHITE_B      94

// Dimming curve x10: 10 = straight through (50% looks nearly full),
// 22 = close to how most bulbs dim.
#define LED_GAMMA        22

// Fade when switching on or off, in tenths of a second. 0 = no fade.
#define LED_FADE_TIME    5

// After power comes back: 0x00 off, 0x01 on, 0x02 opposite of before,
// 0xFF same as before.
#define LED_STARTUP      0xFF

// -------------------------------------------------------
// POWER ESTIMATE - used by the current limit and the power sensor
// -------------------------------------------------------

// mA drawn by one colour channel of one LED at full brightness, and the
// strip voltage. 20 mA and 5 V are the datasheet numbers for WS2812;
// measure your strip at full white and adjust if the estimate is off.
#define LED_MA_PER_CHANNEL  20
#define LED_VOLTS           5

// -------------------------------------------------------
// DEVICE IDENTITY
// -------------------------------------------------------

// Device identity with character hex length prefix (see below for details)
#define MANUFACTURER_NAME  "\x09" "NiftyFish"

// Power source: 0x01 = mains, 0x03 = battery
#define POWER_SOURCE  0x01



// -------------------------------------------------------
// MANUFACTURER_NAME and MODEL_IDENTIFIER must be prefixed
// with a hex byte representing the string length.
//
// Count your characters and find the matching prefix:
//
//  Length | Prefix | Example
//  -------|--------|-------------------------
//    1    | \x01   |
//    2    | \x02   |
//    3    | \x03   |
//    4    | \x04   |
//    5    | \x05   |
//    6    | \x06   |
//    7    | \x07   |
//    8    | \x08   |
//    9    | \x09   | 
//   10    | \x0A   |
//   11    | \x0B   |
//   12    | \x0C   |
//   13    | \x0D   |
//   14    | \x0E   |
//   15    | \x0F   |
//   16    | \x10   |
//   17    | \x11   |
//   18    | \x12   |
//   19    | \x13   |
//   20    | \x14   |
//
// Example:
//   "Niftyfi.sh"    = 10 chars = \x0A
//   "DIY-Button-v1" = 13 chars = \x0D
//
// #define MANUFACTURER_NAME  "\x0A" "Niftyfi.sh"
// #define MODEL_IDENTIFIER   "\x0D" "DIY-Button-v1"
// -------------------------------------------------------