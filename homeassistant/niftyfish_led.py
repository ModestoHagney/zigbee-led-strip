"""ZHA quirk for the NiftyFish Zigbee LED devices.

Puts the firmware's settings on the device page in Home Assistant, under
Configuration. The attribute ids have to match settings_attrs in
main/main.c.

Covers the strip controllers (C6-LED, H2-LED) and the 4-button bedside
light. A device is only touched once its firmware has the settings
cluster, so one running older firmware carries on exactly as before.

Install: copy this file to /config/zha_quirks/ and add to configuration.yaml

    zha:
      custom_quirks_path: /config/zha_quirks/

then restart Home Assistant.
"""

from zigpy.quirks import CustomCluster
from zigpy.quirks.v2 import EntityType, QuirkBuilder
import zigpy.types as t
from zigpy.zcl.clusters.general import DeviceTemperature
from zigpy.zcl.foundation import BaseAttributeDefs, ZCLAttributeDef, ZCLHeader


class ColorOrder(t.enum8):
    """Order the strip expects the colours in."""

    GRB = 0
    RGB = 1
    BRG = 2
    RBG = 3
    GBR = 4
    BGR = 5


class LedType(t.enum8):
    """Kind of LED on the strip."""

    RGB = 0
    RGBW = 1


class LedAnimation(t.enum8):
    """What a button LED of the 4-button light does while it is on.

    The whole list is here from the start so that firmware can gain
    animations later without this file changing. One the firmware doesn't
    know yet shows as Steady.
    """

    Steady = 0
    Breathe = 1
    Blink = 2
    Heartbeat = 3
    Chase = 4
    Alternate = 5
    Rainbow = 6
    Candle = 7
    Countdown = 8
    Strobe = 9


class LedSettings(CustomCluster):
    """Settings cluster of the LED strip firmware."""

    cluster_id = 0xFC00
    name = "LED strip settings"
    ep_attribute = "led_strip_settings"
    # The firmware registers these as plain attributes, so they must be
    # read and written without a manufacturer code.
    manufacturer_id_override = ZCLHeader.NO_MANUFACTURER_ID

    class AttributeDefs(BaseAttributeDefs):
        led_count = ZCLAttributeDef(id=0x0000, type=t.uint16_t, access="rw")
        color_order = ZCLAttributeDef(id=0x0001, type=ColorOrder, access="rw")
        led_type = ZCLAttributeDef(id=0x0002, type=LedType, access="rw")
        max_current = ZCLAttributeDef(id=0x0003, type=t.uint16_t, access="rw")
        white_red = ZCLAttributeDef(id=0x0004, type=t.uint8_t, access="rw")
        white_green = ZCLAttributeDef(id=0x0005, type=t.uint8_t, access="rw")
        white_blue = ZCLAttributeDef(id=0x0006, type=t.uint8_t, access="rw")
        dimming_curve = ZCLAttributeDef(id=0x0007, type=t.uint8_t, access="rw")
        tx_power = ZCLAttributeDef(id=0x0008, type=t.int8s, access="rw")
        # 4-button light only
        button_1_led_animation = ZCLAttributeDef(id=0x0010, type=LedAnimation, access="rw")
        button_2_led_animation = ZCLAttributeDef(id=0x0011, type=LedAnimation, access="rw")
        button_3_led_animation = ZCLAttributeDef(id=0x0012, type=LedAnimation, access="rw")
        button_4_led_animation = ZCLAttributeDef(id=0x0013, type=LedAnimation, access="rw")


class ChipTemperature(CustomCluster, DeviceTemperature):
    """Chip temperature in the scale ZHA expects.

    The firmware sends whole degrees, as the Zigbee spec says. ZHA shows
    this cluster's value divided by 100, so 25 degrees came out as 0.25.
    """

    def _update_attribute(self, attrid, value):
        if attrid == DeviceTemperature.AttributeDefs.current_temperature.id and value is not None:
            value = value * 100
        super()._update_attribute(attrid, value)


def has_settings(device) -> bool:
    """True if the firmware on this device has the settings cluster."""
    return 1 in device.endpoints and LedSettings.cluster_id in device.endpoints[1].in_clusters


def led_device(model: str) -> QuirkBuilder:
    """The settings every NiftyFish LED device has."""
    return (
        QuirkBuilder("NiftyFish", model)
        .filter(has_settings)
        .replaces(LedSettings)
        .replaces(ChipTemperature)
        .number(
            "led_count",
            LedSettings.cluster_id,
            min_value=1,
            max_value=1024,
            step=1,
            translation_key="led_count",
            fallback_name="LED count",
        )
        .enum(
            "color_order",
            ColorOrder,
            LedSettings.cluster_id,
            translation_key="color_order",
            fallback_name="Colour order",
        )
        .enum(
            "led_type",
            LedType,
            LedSettings.cluster_id,
            translation_key="led_type",
            fallback_name="LED type",
        )
        .number(
            "max_current",
            LedSettings.cluster_id,
            min_value=0,
            max_value=65000,
            step=100,
            unit="mA",
            translation_key="max_current",
            fallback_name="Max current (0 = no limit)",
        )
        .number(
            "white_red",
            LedSettings.cluster_id,
            min_value=0,
            max_value=100,
            step=1,
            unit="%",
            translation_key="white_red",
            fallback_name="White balance red",
        )
        .number(
            "white_green",
            LedSettings.cluster_id,
            min_value=0,
            max_value=100,
            step=1,
            unit="%",
            translation_key="white_green",
            fallback_name="White balance green",
        )
        .number(
            "white_blue",
            LedSettings.cluster_id,
            min_value=0,
            max_value=100,
            step=1,
            unit="%",
            translation_key="white_blue",
            fallback_name="White balance blue",
        )
        # gamma x10. Not shown as 1.0-3.0: ZHA's multiplier rounds down on
        # the way back, so 2.3 would be written as 22.
        .number(
            "dimming_curve",
            LedSettings.cluster_id,
            min_value=10,
            max_value=30,
            step=1,
            translation_key="dimming_curve",
            fallback_name="Dimming curve (10 = straight, 22 = normal)",
        )
        .number(
            "tx_power",
            LedSettings.cluster_id,
            min_value=-24,
            max_value=20,
            step=1,
            unit="dBm",
            translation_key="tx_power",
            fallback_name="Zigbee transmit power",
        )
    )


# The LED strip controllers
for model in ("C6-LED", "H2-LED"):
    led_device(model).add_to_registry()

# The 4-button light, under both names it can report: its strip endpoint
# says 4-button-strip, its button endpoints say 4-button-led. It also gets
# an animation for the LED under each button.
for model in ("4-button-strip", "4-button-led"):
    quirk = led_device(model)
    for button in (1, 2, 3, 4):
        quirk = quirk.enum(
            f"button_{button}_led_animation",
            LedAnimation,
            LedSettings.cluster_id,
            entity_type=EntityType.STANDARD,
            translation_key=f"button_{button}_led_animation",
            fallback_name=f"Button {button} LED animation",
        )
    quirk.add_to_registry()
