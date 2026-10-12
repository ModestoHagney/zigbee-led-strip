# NiftyFish firmware updates

Ready-built firmware for the NiftyFish Zigbee LED devices, for Home
Assistant (ZHA) to download and send to the devices over Zigbee.

| File | Device |
|------|--------|
| `niftyfish-c6-led-*.ota` | LED strip controller, ESP32-C6 (`C6-LED`) |
| `niftyfish-h2-led-*.ota` | LED strip controller, ESP32-H2 (`H2-LED`) |
| `niftyfish-4button-*.ota` | 4-button bedside light |

Add this to `configuration.yaml` and restart Home Assistant:

```yaml
zha:
  zigpy_config:
    ota:
      extra_providers:
        - type: z2m
          url: https://raw.githubusercontent.com/ModestoHagney/zigbee-led-strip/firmware/index.json
```

Home Assistant then checks `index.json` once a day and offers an update
on a device's Firmware entity when a newer version is listed.

This branch only holds built files. The source is in the other branches
of this repo and in zigbee-4button-light.
