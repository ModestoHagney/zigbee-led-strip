#pragma once

// -------------------------------------------------------
// LED STRIP CONFIG
// -------------------------------------------------------

#define LED_GPIO         GPIO_NUM_22
#define LED_COUNT        64
#define LED_ENDPOINT     1

// Strip color order, handled by the driver itself.
// Options: _RGB _RBG _GRB _GBR _BRG _BGR
#define LED_COLOR_FORMAT  LED_STRIP_COLOR_COMPONENT_FMT_GRB

// Device identity with character hex length prefix (see below for details)
#define MANUFACTURER_NAME  "\x09" "NiftyFish"
#define MODEL_IDENTIFIER   "\x06" "H2-LED"

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