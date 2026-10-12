idf_component_register(
    SRCS  "main.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES nvs_flash ieee802154 led_strip esp_timer app_update esp_driver_tsens
)
