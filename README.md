| Supported Targets | ESP32-C6 | ESP32-H2 |
| ----------------- | -------- | -------- |

# Zigbee LED Strip

Firmware that turns an ESP32-H2 or ESP32-C6 and a WS2812 strip into a
Zigbee colour light. It joins the network as a router (it relays for
other Zigbee devices, like a mains-powered bulb) and shows up in Home
Assistant as one colour dimmable light (on/off, brightness, and colour
in either hue/saturation or XY).

The whole strip is one light: every pixel gets the same colour.

## Boards

One source builds for both chips. `main/config.h` picks the settings
from the chip target:

| Target   | LED data pin | LED count | Zigbee model |
|----------|--------------|-----------|--------------|
| ESP32-H2 | GPIO 22      | 64        | `H2-LED`     |
| ESP32-C6 | GPIO 19      | 144       | `C6-LED`     |

The LED count in the table is only the default, see Settings below.
Manufacturer name is `NiftyFish`. Power source and the Zigbee endpoint
are set in `main/config.h`.

## Settings in Home Assistant

These are changed on the device page in Home Assistant (ZHA) and saved
on the board. `main/config.h` holds the defaults a freshly flashed board
starts with.

| Setting | Range | Notes |
|---------|-------|-------|
| LED count | 1 to 1024 | |
| Colour order | GRB, RGB, BRG, RBG, GBR, BGR | for strips that show the wrong colours |
| LED type | RGB, RGBW | RGBW sends white to the strip's white LED |
| Max current | 0 to 65000 mA | dims the strip to stay under it, 0 = no limit |
| White balance red, green, blue | 0 to 100 % | so white matches other bulbs |
| Dimming curve | 10 to 30 | 10 = straight, 22 = like most bulbs |
| Zigbee transmit power | -24 to 20 dBm | |
| Start-up behaviour | off, on, toggle, previous | what the strip does when power returns |
| On/off transition time | tenths of a second | fade when switching, 0 = no fade |

The last two are standard Zigbee settings that ZHA shows by itself. The
rest need the quirk file: copy `homeassistant/niftyfish_led.py` to
`/config/zha_quirks/` in Home Assistant, add this to
`configuration.yaml` and restart:

```yaml
zha:
  custom_quirks_path: /config/zha_quirks/
```

The device also reports the chip temperature and an estimated strip
power. The estimate is worked out from colour, brightness and LED count
with `LED_MA_PER_CHANNEL` and `LED_VOLTS` from `main/config.h`; the
current limit uses the same numbers.

The Identify button blinks the strip white.

The last colour, brightness and on/off state are saved a few seconds
after they change and come back after a power cut.

## Hardware

`hardware/ZigbeeLEDController/` is the KiCad 9 project for the
controller board: an ESP32-C6-MINI-1 with a USB-C input, a CH221K USB
PD sink, an AP63203 3.3 V buck regulator and an LED strip output. The
folder also has the PCBWay fabrication zip (`.kicad_pcb.zip`: gerbers,
BOM and positions) and a STEP export of the board.

## Build and flash

Needs ESP-IDF 5.x (built with v5.5.2).

```
idf.py set-target esp32h2
idf.py -p PORT flash monitor
```

Use `esp32c6` for the C6 board. Switching target regenerates
`sdkconfig` and `dependencies.lock`; neither is committed.

Run `idf.py -p PORT erase-flash` first if the board has been paired
before and you want it to join as a new device. This is also needed
once for a board flashed before wireless updates were added, because
the partition layout changed; then remove it from ZHA and pair it again.

An existing `sdkconfig` keeps the old end-device setting. Delete it (or
run `idf.py set-target` again) so `sdkconfig.defaults` applies.

The partition table has two firmware slots of 896 KB each and fits 2 MB
flash.

## Wireless updates

After the first USB flash, new firmware can be sent over Zigbee from
Home Assistant. Home Assistant downloads it from the `firmware` branch
of this repo, so the repo has to be public.

One-time setup in Home Assistant: add this to `configuration.yaml` and
restart.

```yaml
zha:
  zigpy_config:
    ota:
      extra_providers:
        - type: z2m
          url: https://raw.githubusercontent.com/ModestoHagney/zigbee-led-strip/firmware/index.json
```

Home Assistant checks that index once a day (and when it restarts). A
board asks Home Assistant for updates when it joins and then every
hour; when a newer version is listed, its Firmware entity offers it.

To publish an update:

1. Raise `FW_VERSION` in `main/config.h`.
2. `idf.py build`, once per chip.
3. `python tools/make_ota.py` writes `build/niftyfish-<chip>-led-v<version>.ota`.
4. On the `firmware` branch, replace the old `.ota` file with the new
   one, then run
   `python tools/make_index.py . https://raw.githubusercontent.com/ModestoHagney/zigbee-led-strip/firmware`
   and commit and push.

The `firmware` branch also carries the 4-button light's update file, so
one line in Home Assistant covers every NiftyFish device.

An H2 file is never offered to a C6 board, or a strip file to the
4-button light: each has its own image type. If new firmware crashes
before it starts Zigbee, the board goes back to the previous one on the
next boot.

Without GitHub: put the `.ota` file in a folder on the Home Assistant
machine and list that folder as a `type: advanced` provider instead
(ZHA's documentation has the exact lines).

## Pairing

On first boot (or after an erase) the device starts network steering
on its own. Put the coordinator into permit-join and it should log
`joined network! ready.`
