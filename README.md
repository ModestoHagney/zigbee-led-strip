| Supported Targets | ESP32-C6 | ESP32-H2 |
| ----------------- | -------- | -------- |

# Zigbee LED Strip

Firmware that turns an ESP32-H2 or ESP32-C6 and a WS2812 strip into a
Zigbee colour light. It joins the network as an end device and shows up
in Home Assistant as one colour dimmable light (on/off, brightness, and
colour in either hue/saturation or XY).

The whole strip is one light: every pixel gets the same colour.

## Boards

One source builds for both chips. `main/config.h` picks the settings
from the chip target:

| Target   | LED data pin | LED count | Zigbee model |
|----------|--------------|-----------|--------------|
| ESP32-H2 | GPIO 22      | 64        | `H2-LED`     |
| ESP32-C6 | GPIO 19      | 144       | `C6-LED`     |

Manufacturer name is `NiftyFish`. Colour order (GRB), power source and
the Zigbee endpoint are also set in `main/config.h`.

## Build and flash

Needs ESP-IDF 5.x (built with v5.5.2).

```
idf.py set-target esp32h2
idf.py -p PORT flash monitor
```

Use `esp32c6` for the C6 board. Switching target regenerates
`sdkconfig` and `dependencies.lock`; neither is committed.

Run `idf.py -p PORT erase-flash` first if the board has been paired
before and you want it to join as a new device.

## Pairing

On first boot (or after an erase) the device starts network steering
on its own. Put the coordinator into permit-join and it should log
`joined network! ready.`
