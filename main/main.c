#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_zigbee_core.h"
#include "zcl/esp_zigbee_zcl_command.h"
#include "nvs_flash.h"
#include "led_strip.h"
#include "config.h"
#include <math.h>

static const char *TAG = "ZB_LED";

// -------------------------------------------------------
// LED strip
// -------------------------------------------------------

static led_strip_handle_t led_strip;

// current light state
static bool    light_on         = false;
static uint8_t light_level      = 254;
static uint8_t light_hue        = 0;
static uint8_t light_saturation = 0;
uint16_t light_x = 0;
uint16_t light_y = 0;
uint8_t color_mode = 0; // 0 = HS, 1 = XY

static void led_apply(void) {
    if (!light_on || light_level == 0) {
        led_strip_clear(led_strip);
        led_strip_refresh(led_strip);
        return;
    }

    // Initialize to 0 to stop the "uninitialized" errors
    float r = 0, g = 0, b = 0;
    float v = (float)light_level / 254.0f;

    if (color_mode == 0x01) { // XY Mode
        float x = (float)light_x / 65535.0f;
        float y = (float)light_y / 65535.0f;
        float z = 1.0f - x - y;

        float Y = v; 
        float X = (y > 0) ? (x * Y) / y : 0;
        float Z = (y > 0) ? (z * Y) / y : 0;

        r =  3.2406f * X - 1.5372f * Y - 0.4986f * Z;
        g = -0.9689f * X + 1.8758f * Y + 0.0415f * Z;
        b =  0.0557f * X - 0.2040f * Y + 1.0570f * Z;
    } else { // HS Mode (0x00)
        float h = (float)light_hue / 254.0f;
        float s = (float)light_saturation / 254.0f;

        int i = (int)(h * 6);
        float f = h * 6 - i;
        float p = v * (1 - s);
        float q = v * (1 - f * s);
        float t = v * (1 - (1 - f) * s);

        switch (i % 6) {
            case 0: r = v; g = t; b = p; break;
            case 1: r = q; g = v; b = p; break;
            case 2: r = p; g = v; b = t; break;
            case 3: r = p; g = q; b = v; break;
            case 4: r = t; g = p; b = v; break;
            case 5: r = v; g = p; b = q; break;
        }
    }

    // Clamp values 0.0 - 1.0
    r = (r < 0) ? 0 : ((r > 1) ? 1 : r);
    g = (g < 0) ? 0 : ((g > 1) ? 1 : g);
    b = (b < 0) ? 0 : ((b > 1) ? 1 : b);

    for (int j = 0; j < LED_COUNT; j++) {
        led_strip_set_pixel(led_strip, j, (uint8_t)(r * 255), (uint8_t)(g * 255), (uint8_t)(b * 255));
    }
    led_strip_refresh(led_strip);
}



static void led_init(void) {
    led_strip_config_t strip_cfg = {
    .strip_gpio_num         = LED_GPIO,
    .max_leds               = LED_COUNT,
    .color_component_format = LED_COLOR_FORMAT,   // <- from config.h
    .led_model              = LED_MODEL_WS2812,
    .flags.invert_out       = false,
    };
    led_strip_rmt_config_t rmt_cfg = {
        .clk_src        = RMT_CLK_SRC_DEFAULT,
        .resolution_hz  = 10 * 1000 * 1000,
        .flags.with_dma = false,
    };
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &led_strip));
    led_strip_clear(led_strip);
    led_strip_refresh(led_strip);
}

// -------------------------------------------------------
// Zigbee callback - handle light commands from HA
// -------------------------------------------------------

static esp_err_t zb_action_handler(esp_zb_core_action_callback_id_t callback_id, const void *message) {
    switch (callback_id) {
        case ESP_ZB_CORE_SET_ATTR_VALUE_CB_ID: {
            esp_zb_zcl_set_attr_value_message_t *msg =
                (esp_zb_zcl_set_attr_value_message_t *)message;

            ESP_LOGI(TAG, "GOT ATTR: ep=%d cluster=0x%04x", 
                     msg->info.dst_endpoint, msg->info.cluster);  // <-- ADD THIS!

            if (msg->info.dst_endpoint != LED_ENDPOINT) break;

            ESP_LOGI(TAG, "attr: ep=%d cluster=0x%04x attr=0x%04x", 
                    msg->info.dst_endpoint, msg->info.cluster, msg->attribute.id);

            if (msg->info.cluster == ESP_ZB_ZCL_CLUSTER_ID_ON_OFF) {
                if (msg->attribute.id == ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID) {
                    light_on = msg->attribute.data.value
                        ? *(bool *)msg->attribute.data.value
                        : false;
                    ESP_LOGI(TAG, "light on/off: %d", light_on);
                    led_apply();
                }
            } else if (msg->info.cluster == ESP_ZB_ZCL_CLUSTER_ID_LEVEL_CONTROL) {
                if (msg->attribute.id == ESP_ZB_ZCL_ATTR_LEVEL_CONTROL_CURRENT_LEVEL_ID) {
                    light_level = msg->attribute.data.value
                        ? *(uint8_t *)msg->attribute.data.value
                        : 254;
                    ESP_LOGI(TAG, "light level: %d", light_level);
                    led_apply();
                }
            } else if (msg->info.cluster == ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL) {
                uint16_t attr_id = msg->attribute.id;
                void* val_ptr = msg->attribute.data.value;

                if (attr_id == ESP_ZB_ZCL_ATTR_COLOR_CONTROL_CURRENT_HUE_ID) {
                    light_hue = *(uint8_t *)val_ptr;
                } 
                else if (attr_id == ESP_ZB_ZCL_ATTR_COLOR_CONTROL_CURRENT_SATURATION_ID) {
                    light_saturation = *(uint8_t *)val_ptr;
                } 
                else if (attr_id == ESP_ZB_ZCL_ATTR_COLOR_CONTROL_CURRENT_X_ID) {
                    light_x = *(uint16_t *)val_ptr;
                } 
                else if (attr_id == ESP_ZB_ZCL_ATTR_COLOR_CONTROL_CURRENT_Y_ID) {
                    light_y = *(uint16_t *)val_ptr;
                } 
                else if (attr_id == ESP_ZB_ZCL_ATTR_COLOR_CONTROL_COLOR_MODE_ID) {
                    color_mode = *(uint8_t *)val_ptr;
                }

                ESP_LOGI(TAG, "Color update: Mode=%d, H=%d, S=%d, X=%d, Y=%d", 
                        color_mode, light_hue, light_saturation, light_x, light_y);
                
                led_apply();
            }
            break;
        }
        default:
            break;
    }
    return ESP_OK;
}

// -------------------------------------------------------
// Zigbee endpoints
// -------------------------------------------------------

static void add_light_endpoint(esp_zb_ep_list_t *ep_list) {
    esp_zb_cluster_list_t *list = esp_zb_zcl_cluster_list_create();

    esp_zb_basic_cluster_cfg_t basic_cfg = {
        .zcl_version  = ESP_ZB_ZCL_BASIC_ZCL_VERSION_DEFAULT_VALUE,
        .power_source = POWER_SOURCE,
    };
    esp_zb_attribute_list_t *basic_attrs = esp_zb_basic_cluster_create(&basic_cfg);
    esp_zb_basic_cluster_add_attr(basic_attrs,
        ESP_ZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID, MANUFACTURER_NAME);
    esp_zb_basic_cluster_add_attr(basic_attrs,
        ESP_ZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID, MODEL_IDENTIFIER);
    esp_zb_cluster_list_add_basic_cluster(list, basic_attrs,
        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    esp_zb_identify_cluster_cfg_t identify_cfg = {.identify_time = 0};
    esp_zb_cluster_list_add_identify_cluster(list,
        esp_zb_identify_cluster_create(&identify_cfg),
        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    esp_zb_on_off_cluster_cfg_t on_off_cfg = {.on_off = false};
    esp_zb_cluster_list_add_on_off_cluster(list,
        esp_zb_on_off_cluster_create(&on_off_cfg),
        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    esp_zb_level_cluster_cfg_t level_cfg = {.current_level = 254};
    esp_zb_cluster_list_add_level_cluster(list,
        esp_zb_level_cluster_create(&level_cfg),
        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    esp_zb_color_cluster_cfg_t color_cfg = {
        .current_x = ESP_ZB_ZCL_COLOR_CONTROL_CURRENT_X_DEF_VALUE,
        .current_y = ESP_ZB_ZCL_COLOR_CONTROL_CURRENT_Y_DEF_VALUE,
        .color_mode = 0x00,
        .options = ESP_ZB_ZCL_COLOR_CONTROL_OPTIONS_DEFAULT_VALUE,
        .enhanced_color_mode = 0x00,
        .color_capabilities = 0x0009, // XY and HS
    };

    esp_zb_attribute_list_t *color_cluster = esp_zb_color_control_cluster_create(&color_cfg);

    // ADD THESE MANUALLY - The 'create' function often misses them!
    uint8_t color_mode = 0x01; // Default to XY
    uint8_t enh_mode = 0x01;
    uint16_t hue = 0;
    uint8_t sat = 0;

    esp_zb_color_control_cluster_add_attr(color_cluster, ESP_ZB_ZCL_ATTR_COLOR_CONTROL_COLOR_MODE_ID, &color_mode);
    esp_zb_color_control_cluster_add_attr(color_cluster, ESP_ZB_ZCL_ATTR_COLOR_CONTROL_ENHANCED_COLOR_MODE_ID, &enh_mode);
    esp_zb_color_control_cluster_add_attr(color_cluster, ESP_ZB_ZCL_ATTR_COLOR_CONTROL_CURRENT_HUE_ID, &hue);
    esp_zb_color_control_cluster_add_attr(color_cluster, ESP_ZB_ZCL_ATTR_COLOR_CONTROL_CURRENT_SATURATION_ID, &sat);

    esp_zb_cluster_list_add_color_control_cluster(list, color_cluster, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    esp_zb_endpoint_config_t ep_cfg = {
        .endpoint           = LED_ENDPOINT,
        .app_profile_id     = ESP_ZB_AF_HA_PROFILE_ID,
        .app_device_id      = ESP_ZB_HA_COLOR_DIMMABLE_LIGHT_DEVICE_ID,
        .app_device_version = 0,
    };
    esp_zb_ep_list_add_ep(ep_list, list, ep_cfg);
}

static void zigbee_endpoint_create(void) {
    esp_zb_ep_list_t *ep_list = esp_zb_ep_list_create();
    add_light_endpoint(ep_list);
    esp_zb_device_register(ep_list);
    esp_zb_core_action_handler_register(zb_action_handler);
}

// -------------------------------------------------------
// Zigbee stack
// -------------------------------------------------------

static void zigbee_task(void *arg) {
    esp_zb_cfg_t zb_cfg = {
        .esp_zb_role         = ESP_ZB_DEVICE_TYPE_ED,
        .install_code_policy = false,
        .nwk_cfg.zed_cfg = {
            .ed_timeout = ESP_ZB_ED_AGING_TIMEOUT_64MIN,
            .keep_alive = 3000,
        },
    };
    esp_zb_init(&zb_cfg);
    zigbee_endpoint_create();
    esp_zb_set_primary_network_channel_set(ESP_ZB_TRANSCEIVER_ALL_CHANNELS_MASK);
    ESP_ERROR_CHECK(esp_zb_start(false));
    esp_zb_stack_main_loop();
}

// -------------------------------------------------------
// Zigbee signal handler
// -------------------------------------------------------

void esp_zb_app_signal_handler(esp_zb_app_signal_t *signal_struct) {
    esp_err_t                err = signal_struct->esp_err_status;
    esp_zb_app_signal_type_t sig = *signal_struct->p_app_signal;

    switch (sig) {
        case ESP_ZB_ZDO_SIGNAL_SKIP_STARTUP:
            esp_zb_bdb_start_top_level_commissioning(
                ESP_ZB_BDB_MODE_INITIALIZATION);
            break;

        case ESP_ZB_BDB_SIGNAL_DEVICE_FIRST_START:
        case ESP_ZB_BDB_SIGNAL_DEVICE_REBOOT:
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "starting network steering");
                esp_zb_bdb_start_top_level_commissioning(
                    ESP_ZB_BDB_MODE_NETWORK_STEERING);
            } else {
                ESP_LOGE(TAG, "init failed, retrying");
                esp_zb_bdb_start_top_level_commissioning(
                    ESP_ZB_BDB_MODE_INITIALIZATION);
            }
            break;

        case ESP_ZB_BDB_SIGNAL_STEERING:
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "joined network! ready.");
            } else {
                ESP_LOGI(TAG, "steering failed, retrying");
                esp_zb_bdb_start_top_level_commissioning(
                    ESP_ZB_BDB_MODE_NETWORK_STEERING);
            }
            break;

        default:
            ESP_LOGI(TAG, "signal %d err %d", sig, err);
            break;
    }
}

// -------------------------------------------------------
// app_main
// -------------------------------------------------------

void app_main(void) {
    ESP_ERROR_CHECK(nvs_flash_init());
    led_init();
    xTaskCreate(zigbee_task, "zigbee_task", 4096, NULL, 5, NULL);
}