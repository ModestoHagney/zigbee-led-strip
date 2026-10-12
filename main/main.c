#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_ota_ops.h"
#include "esp_zigbee_core.h"
#include "esp_zigbee_ota.h"
#include "zcl/esp_zigbee_zcl_command.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "led_strip.h"
#include "driver/temperature_sensor.h"
#include "config.h"
#include <math.h>
#include <string.h>

static const char *TAG = "ZB_LED";

// -------------------------------------------------------
// Settings - changed from Home Assistant, kept in NVS
// -------------------------------------------------------

typedef struct {
    uint16_t led_count;
    uint8_t  color_order;     // index into color_orders
    uint8_t  led_type;        // 0 = RGB, 1 = RGBW
    uint16_t max_current;     // mA, 0 = no limit
    uint8_t  white_r;         // white balance, percent
    uint8_t  white_g;
    uint8_t  white_b;
    uint8_t  gamma;           // dimming curve x10
    int8_t   tx_power;        // dBm
    uint8_t  startup_on_off;  // 0x00 off, 0x01 on, 0x02 toggle, 0xFF previous
    uint16_t fade_time;       // on/off fade in 1/10 s, 0 = none
} settings_t;

// tx_power until the user picks one: leave the radio at its own default
#define TX_POWER_UNSET  INT8_MAX

static settings_t cfg = {
    .led_count      = LED_COUNT,
    .color_order    = LED_COLOR_ORDER,
    .led_type       = LED_TYPE,
    .max_current    = LED_MAX_CURRENT,
    .white_r        = LED_WHITE_R,
    .white_g        = LED_WHITE_G,
    .white_b        = LED_WHITE_B,
    .gamma          = LED_GAMMA,
    .tx_power       = TX_POWER_UNSET,
    .startup_on_off = LED_STARTUP,
    .fade_time      = LED_FADE_TIME,
};

// The settings Home Assistant sees, as attributes of one custom cluster.
// homeassistant/niftyfish_led.py has to list the same ids.
#define SETTINGS_CLUSTER  0xFC00
#define ATTR_TX_POWER     0x0008

static const struct {
    uint16_t id;
    uint8_t  type;
    void    *value;
} settings_attrs[] = {
    {0x0000, ESP_ZB_ZCL_ATTR_TYPE_U16,       &cfg.led_count},
    {0x0001, ESP_ZB_ZCL_ATTR_TYPE_8BIT_ENUM, &cfg.color_order},
    {0x0002, ESP_ZB_ZCL_ATTR_TYPE_8BIT_ENUM, &cfg.led_type},
    {0x0003, ESP_ZB_ZCL_ATTR_TYPE_U16,       &cfg.max_current},
    {0x0004, ESP_ZB_ZCL_ATTR_TYPE_U8,        &cfg.white_r},
    {0x0005, ESP_ZB_ZCL_ATTR_TYPE_U8,        &cfg.white_g},
    {0x0006, ESP_ZB_ZCL_ATTR_TYPE_U8,        &cfg.white_b},
    {0x0007, ESP_ZB_ZCL_ATTR_TYPE_U8,        &cfg.gamma},
    {ATTR_TX_POWER, ESP_ZB_ZCL_ATTR_TYPE_S8, &cfg.tx_power},
};
#define SETTINGS_ATTR_COUNT  (sizeof(settings_attrs) / sizeof(settings_attrs[0]))

// r, g, b position on the wire, in the order the colour order setting
// lists them: GRB, RGB, BRG, RBG, GBR, BGR
static const uint8_t color_orders[][3] = {
    {1, 0, 2}, {0, 1, 2}, {1, 2, 0}, {0, 2, 1}, {2, 0, 1}, {2, 1, 0},
};
#define COLOR_ORDER_COUNT  (sizeof(color_orders) / sizeof(color_orders[0]))

static void nvs_save(const char *key, const void *data, size_t len) {
    nvs_handle_t nvs;
    if (nvs_open("led", NVS_READWRITE, &nvs) != ESP_OK) return;
    nvs_set_blob(nvs, key, data, len);
    nvs_commit(nvs);
    nvs_close(nvs);
}

// false, with data untouched, if the key is missing or was saved by a
// build with a different layout
static bool nvs_load(const char *key, void *data, size_t len) {
    nvs_handle_t nvs;
    bool ok = false;
    if (nvs_open("led", NVS_READONLY, &nvs) == ESP_OK) {
        size_t stored = 0;
        ok = nvs_get_blob(nvs, key, NULL, &stored) == ESP_OK && stored == len
          && nvs_get_blob(nvs, key, data, &stored) == ESP_OK;
        nvs_close(nvs);
    }
    return ok;
}

// Home Assistant limits what can be typed in, this is the backstop
static void settings_clamp(void) {
    if (cfg.led_count < 1) cfg.led_count = 1;
    if (cfg.led_count > LED_COUNT_MAX) cfg.led_count = LED_COUNT_MAX;
    if (cfg.color_order >= COLOR_ORDER_COUNT) cfg.color_order = 0;
    if (cfg.led_type > 1) cfg.led_type = 0;
    if (cfg.white_r > 100) cfg.white_r = 100;
    if (cfg.white_g > 100) cfg.white_g = 100;
    if (cfg.white_b > 100) cfg.white_b = 100;
    if (cfg.gamma < 10) cfg.gamma = 10;
    if (cfg.gamma > 30) cfg.gamma = 30;
    if (cfg.tx_power != TX_POWER_UNSET) {
        if (cfg.tx_power < -24) cfg.tx_power = -24;
        if (cfg.tx_power > 20) cfg.tx_power = 20;
    }
}

// -------------------------------------------------------
// Light state
// -------------------------------------------------------

static bool    light_on         = false;
static uint8_t light_level      = 254;
static uint8_t light_hue        = 0;
static uint8_t light_saturation = 0;
uint16_t light_x = ESP_ZB_ZCL_COLOR_CONTROL_CURRENT_X_DEF_VALUE;
uint16_t light_y = ESP_ZB_ZCL_COLOR_CONTROL_CURRENT_Y_DEF_VALUE;
uint8_t color_mode = 1; // 0 = HS, 1 = XY

// what gets kept across a power cut
typedef struct {
    bool     on;
    uint8_t  level, hue, saturation, color_mode;
    uint16_t x, y;
} light_state_t;

// When a light is switched off the stack drops its level to 0, and it
// hands that same 0 back on the next switch-on. Taking it as the
// brightness left the light on but dark. So a level of 0 is ignored and
// the last real brightness is kept.
static uint8_t keep_brightness(uint8_t current, uint8_t written) {
    return written ? written : current;
}

static esp_timer_handle_t save_timer;

static void state_save(void *arg) {
    light_state_t s = {
        .on = light_on, .level = light_level,
        .hue = light_hue, .saturation = light_saturation, .color_mode = color_mode,
        .x = light_x, .y = light_y,
    };
    nvs_save("state", &s, sizeof(s));
}

// Bring back the last colour and brightness, then decide on or off from
// the power-on setting.
static void state_load(void) {
    light_state_t s = {0};
    bool saved = nvs_load("state", &s, sizeof(s));
    if (saved) {
        light_level = s.level ? s.level : 254;  // 0: saved by a build that stored it
        light_hue = s.hue;
        light_saturation = s.saturation;
        color_mode = s.color_mode;
        light_x = s.x;
        light_y = s.y;
    }
    switch (cfg.startup_on_off) {
        case 0x00: light_on = false; break;
        case 0x01: light_on = true; break;
        case 0x02: light_on = !s.on; break;
        default:   light_on = s.on; break;
    }
}

// -------------------------------------------------------
// LED strip - only the LED task touches the strip
// -------------------------------------------------------

#define LED_FRAME_MS        20   // redraw interval while fading or blinking
#define LED_IDENTIFY_LEVEL  64   // blink brightness for Identify, 0-255

static led_strip_handle_t led_strip;
static TaskHandle_t led_task_handle;

static volatile bool strip_stale;      // count, order or type changed: rebuild the driver
static volatile bool identifying;      // Identify asked for: blink
static volatile uint32_t strip_ma;     // estimated current of what is on the strip now

// Work out the strip colour for the current light state.
// fade is 0 (off) to 1 (fully on). rgbw is 8 bits per channel.
static void light_rgbw(float fade, uint8_t rgbw[4]) {
    // colour at full brightness, in linear light
    float r = 0, g = 0, b = 0, w = 0;

    if (color_mode == 0x01) { // XY Mode
        float x = (float)light_x / 65535.0f;
        float y = (float)light_y / 65535.0f;
        float z = 1.0f - x - y;

        // xyY with Y = 1
        float X = (y > 0) ? x / y : 0;
        float Z = (y > 0) ? z / y : 0;

        r =  3.2406f * X - 1.5372f - 0.4986f * Z;
        g = -0.9689f * X + 1.8758f + 0.0415f * Z;
        b =  0.0557f * X - 0.2040f + 1.0570f * Z;
    } else { // HS Mode (0x00)
        float h = (float)light_hue / 254.0f;
        float s = (float)light_saturation / 254.0f;

        int i = (int)(h * 6);
        float f = h * 6 - i;
        float p = 1 - s;
        float q = 1 - f * s;
        float t = 1 - (1 - f) * s;

        switch (i % 6) {
            case 0: r = 1; g = t; b = p; break;
            case 1: r = q; g = 1; b = p; break;
            case 2: r = p; g = 1; b = t; break;
            case 3: r = p; g = q; b = 1; break;
            case 4: r = t; g = p; b = 1; break;
            case 5: r = 1; g = p; b = q; break;
        }

        // hue/saturation colours are sRGB encoded, the LEDs are linear
        r = powf(r, 2.2f);
        g = powf(g, 2.2f);
        b = powf(b, 2.2f);
    }

    // Negative means outside what the LEDs can show. Then scale all three
    // together so the brightest is 1: clipping one channel shifts the hue.
    r = fmaxf(r, 0);
    g = fmaxf(g, 0);
    b = fmaxf(b, 0);
    float max = fmaxf(r, fmaxf(g, b));
    if (max > 0) { r /= max; g /= max; b /= max; }

    // RGBW: the white LED shows the part all three channels share
    if (cfg.led_type == 1) {
        w = fminf(r, fminf(g, b));
        r -= w; g -= w; b -= w;
    }

    float v = powf((float)light_level / 254.0f * fade, cfg.gamma / 10.0f);
    float out[4] = {
        r * v * cfg.white_r / 100.0f,
        g * v * cfg.white_g / 100.0f,
        b * v * cfg.white_b / 100.0f,
        w * v,
    };

    int brightest = 0;
    for (int i = 0; i < 4; i++) {
        rgbw[i] = (uint8_t)(out[i] * 255 + 0.5f);
        if (out[i] > out[brightest]) brightest = i;
    }
    // fully on, but dimmer than one 8-bit step: stay lit instead of going black
    if (fade >= 1 && !(rgbw[0] | rgbw[1] | rgbw[2] | rgbw[3])) rgbw[brightest] = 1;
}

// Dim rgbw if the whole strip would draw more than the current limit,
// and note the estimate for the power sensor.
// ponytail: counts LED colour current only, not the ~1 mA each LED draws
// while dark. Add an idle term if the limit needs to be exact.
static void limit_current(uint8_t rgbw[4]) {
    uint32_t sum = rgbw[0] + rgbw[1] + rgbw[2] + rgbw[3];
    uint32_t ma = sum * LED_MA_PER_CHANNEL * cfg.led_count / 255;
    if (cfg.max_current && ma > cfg.max_current) {
        for (int i = 0; i < 4; i++) rgbw[i] = rgbw[i] * cfg.max_current / ma;
        ma = cfg.max_current;
    }
    strip_ma = ma;
}

static void led_init(void) {
    const uint8_t *pos = color_orders[cfg.color_order];
    led_strip_config_t strip_cfg = {
        .strip_gpio_num = LED_GPIO,
        .max_leds       = cfg.led_count,
        .led_model      = cfg.led_type ? LED_MODEL_SK6812 : LED_MODEL_WS2812,
        .color_component_format.format = {
            .r_pos = pos[0], .g_pos = pos[1], .b_pos = pos[2], .w_pos = 3,
            .bytes_per_color = 1,
            .num_components  = cfg.led_type ? 4 : 3,
        },
        .flags.invert_out = false,
    };
    led_strip_rmt_config_t rmt_cfg = {
        .clk_src        = RMT_CLK_SRC_DEFAULT,
        .resolution_hz  = 10 * 1000 * 1000,
        .flags.with_dma = false,
    };
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &led_strip));
    led_strip_clear(led_strip);
}

static void led_task(void *arg) {
    float fade = 0;  // 0 = off, 1 = on, moves toward light_on over the fade time
    int64_t last = esp_timer_get_time();
    bool busy = true;
    led_init();

    for (;;) {
        int64_t now = esp_timer_get_time();
        // after sleeping there is no previous frame to measure from
        float dt = busy ? (now - last) / 1e6f : 0;
        last = now;

        if (strip_stale) {
            strip_stale = false;
            // the driver always sends max_leds pixels, so rebuild it at the new
            // length. Clear first so LEDs past a shorter count don't stay lit.
            led_strip_clear(led_strip);
            led_strip_del(led_strip);
            led_init();
        }

        float target = light_on ? 1 : 0;
        float step = cfg.fade_time ? dt / (cfg.fade_time / 10.0f) : 1;
        fade = (target > fade) ? fminf(fade + step, target) : fmaxf(fade - step, target);

        uint8_t rgbw[4] = {0};
        if (identifying) {
            // blink white once a second
            if ((now / 500000) % 2 == 0) rgbw[0] = rgbw[1] = rgbw[2] = LED_IDENTIFY_LEVEL;
        } else if (fade > 0 && light_level > 0) {
            light_rgbw(fade, rgbw);
        }
        limit_current(rgbw);

        for (int j = 0; j < cfg.led_count; j++) {
            if (cfg.led_type == 1) {
                led_strip_set_pixel_rgbw(led_strip, j, rgbw[0], rgbw[1], rgbw[2], rgbw[3]);
            } else {
                led_strip_set_pixel(led_strip, j, rgbw[0], rgbw[1], rgbw[2]);
            }
        }
        led_strip_refresh(led_strip);

        // keep drawing while something is moving, otherwise sleep until told
        busy = identifying || fade != target;
        ulTaskNotifyTake(pdTRUE, busy ? pdMS_TO_TICKS(LED_FRAME_MS) : portMAX_DELAY);
    }
}

// The light changed: redraw, and save the state once it has settled so a
// fade or colour sweep doesn't write flash on every step.
static void led_apply(void) {
    xTaskNotifyGive(led_task_handle);
    esp_timer_stop(save_timer);
    esp_timer_start_once(save_timer, 5 * 1000 * 1000);
}

static void identify_handler(uint8_t identify_on) {
    identifying = identify_on;
    xTaskNotifyGive(led_task_handle);
}

// A setting was written from Home Assistant
static void settings_write(uint16_t attr_id, const void *value) {
    settings_t old = cfg;
    for (int i = 0; i < SETTINGS_ATTR_COUNT; i++) {
        if (settings_attrs[i].id != attr_id) continue;
        memcpy(settings_attrs[i].value, value,
               settings_attrs[i].type == ESP_ZB_ZCL_ATTR_TYPE_U16 ? 2 : 1);
    }
    settings_clamp();
    nvs_save("cfg", &cfg, sizeof(cfg));
    ESP_LOGI(TAG, "setting 0x%04x written", attr_id);

    if (cfg.tx_power != old.tx_power && cfg.tx_power != TX_POWER_UNSET) {
        esp_zb_set_tx_power(cfg.tx_power);
    }
    if (cfg.led_count != old.led_count || cfg.color_order != old.color_order
            || cfg.led_type != old.led_type) {
        strip_stale = true;
    }
    xTaskNotifyGive(led_task_handle);
}

// -------------------------------------------------------
// Sensors - chip temperature and estimated strip power
// -------------------------------------------------------

#define SENSOR_INTERVAL_MS  10000

static temperature_sensor_handle_t temp_sensor;

// runs in the Zigbee task, so it can set attributes without the lock
static void sensors_update(uint8_t param) {
    float celsius;
    if (temperature_sensor_get_celsius(temp_sensor, &celsius) == ESP_OK) {
        int16_t temp = (int16_t)celsius;
        esp_zb_zcl_set_attribute_val(LED_ENDPOINT, ESP_ZB_ZCL_CLUSTER_ID_DEVICE_TEMP_CONFIG,
            ESP_ZB_ZCL_CLUSTER_SERVER_ROLE, ESP_ZB_ZCL_ATTR_DEVICE_TEMP_CONFIG_CURRENT_TEMP_ID,
            &temp, false);
    }

    // mA x V = mW, reported in 0.1 W
    int16_t power = strip_ma * LED_VOLTS / 100;
    esp_zb_zcl_set_attribute_val(LED_ENDPOINT, ESP_ZB_ZCL_CLUSTER_ID_ELECTRICAL_MEASUREMENT,
        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE, ESP_ZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_ACTIVE_POWER_ID,
        &power, false);

    esp_zb_scheduler_alarm(sensors_update, 0, SENSOR_INTERVAL_MS);
}

// -------------------------------------------------------
// Wireless firmware update (Zigbee OTA client)
// -------------------------------------------------------

// An OTA file is a Zigbee header (the stack reads that), then one
// element: a 2-byte tag, a 4-byte length, and the firmware itself.
#define OTA_ELEMENT_HEADER_LEN  6
#define OTA_TAG_UPGRADE_IMAGE   0

static esp_err_t ota_handler(const esp_zb_zcl_ota_upgrade_value_message_t *msg) {
    static const esp_partition_t *partition;
    static esp_ota_handle_t handle;
    static bool writing;
    static uint32_t received;  // bytes of the file so far, element header included

    if (msg->info.status != ESP_ZB_ZCL_STATUS_SUCCESS) return ESP_OK;

    switch (msg->upgrade_status) {
        case ESP_ZB_ZCL_OTA_UPGRADE_STATUS_START:
            ESP_LOGI(TAG, "update: start");
            if (writing) esp_ota_abort(handle);  // an earlier download never finished
            writing = false;
            received = 0;
            partition = esp_ota_get_next_update_partition(NULL);
            if (!partition) return ESP_FAIL;
            ESP_RETURN_ON_ERROR(esp_ota_begin(partition, OTA_WITH_SEQUENTIAL_WRITES, &handle),
                                TAG, "update: can't open the spare partition");
            writing = true;
            break;

        case ESP_ZB_ZCL_OTA_UPGRADE_STATUS_RECEIVE: {
            const uint8_t *data = msg->payload;
            uint16_t len = msg->payload_size;
            if (!data || !len) break;

            if (received == 0) {
                uint16_t tag;
                uint32_t length;
                if (len <= OTA_ELEMENT_HEADER_LEN) return ESP_ERR_INVALID_ARG;
                memcpy(&tag, data, sizeof(tag));
                memcpy(&length, data + sizeof(tag), sizeof(length));
                if (tag != OTA_TAG_UPGRADE_IMAGE
                        || length + OTA_ELEMENT_HEADER_LEN != msg->ota_header.image_size) {
                    ESP_LOGE(TAG, "update: not a firmware file made by tools/make_ota.py");
                    return ESP_ERR_INVALID_ARG;
                }
                data += OTA_ELEMENT_HEADER_LEN;
                len  -= OTA_ELEMENT_HEADER_LEN;
            }
            received += msg->payload_size;
            ESP_RETURN_ON_ERROR(esp_ota_write(handle, data, len), TAG, "update: flash write failed");
            break;
        }

        case ESP_ZB_ZCL_OTA_UPGRADE_STATUS_CHECK:
            ESP_LOGI(TAG, "update: received %lu of %lu bytes", received, msg->ota_header.image_size);
            return received == msg->ota_header.image_size ? ESP_OK : ESP_FAIL;

        case ESP_ZB_ZCL_OTA_UPGRADE_STATUS_FINISH:
            writing = false;
            // esp_ota_end checks the image before it is allowed to boot
            ESP_RETURN_ON_ERROR(esp_ota_end(handle), TAG, "update: image check failed");
            ESP_RETURN_ON_ERROR(esp_ota_set_boot_partition(partition), TAG, "update: can't switch partition");
            ESP_LOGW(TAG, "update: done, restarting into version 0x%lx", msg->ota_header.file_version);
            esp_restart();
            break;

        case ESP_ZB_ZCL_OTA_UPGRADE_STATUS_ABORT:
            if (writing) esp_ota_abort(handle);
            writing = false;
            break;

        default:
            break;
    }
    return ESP_OK;
}

// How often the board asks Home Assistant whether there is an update.
// The first question, right after joining, is also what tells Home
// Assistant which version is running.
#define OTA_QUERY_MINUTES  60

static void ota_server_found(esp_zb_zdp_status_t status, uint16_t addr, uint8_t endpoint, void *ctx) {
    if (status != ESP_ZB_ZDP_STATUS_SUCCESS) {
        ESP_LOGW(TAG, "update: the coordinator has no update server");
        return;
    }
    esp_zb_ota_upgrade_client_query_interval_set(LED_ENDPOINT, OTA_QUERY_MINUTES);
    esp_zb_ota_upgrade_client_query_image_req(addr, endpoint);
}

// Ask the coordinator which endpoint its update server is on
static void ota_find_server(void) {
    uint16_t clusters[] = {ESP_ZB_ZCL_CLUSTER_ID_OTA_UPGRADE};
    esp_zb_zdo_match_desc_req_param_t req = {
        .dst_nwk_addr     = 0x0000,
        .addr_of_interest = 0x0000,
        .profile_id       = ESP_ZB_AF_HA_PROFILE_ID,
        .num_in_clusters  = 1,
        .num_out_clusters = 0,
        .cluster_list     = clusters,
    };
    esp_zb_zdo_match_cluster(&req, ota_server_found, NULL);
}

// -------------------------------------------------------
// Zigbee callback - handle light commands from HA
// -------------------------------------------------------

static esp_err_t zb_action_handler(esp_zb_core_action_callback_id_t callback_id, const void *message) {
    switch (callback_id) {
        case ESP_ZB_CORE_SET_ATTR_VALUE_CB_ID: {
            esp_zb_zcl_set_attr_value_message_t *msg =
                (esp_zb_zcl_set_attr_value_message_t *)message;

            if (msg->info.dst_endpoint != LED_ENDPOINT) break;

            ESP_LOGI(TAG, "attr: ep=%d cluster=0x%04x attr=0x%04x",
                    msg->info.dst_endpoint, msg->info.cluster, msg->attribute.id);

            uint16_t attr_id = msg->attribute.id;
            void* val_ptr = msg->attribute.data.value;
            if (!val_ptr) break;

            if (msg->info.cluster == ESP_ZB_ZCL_CLUSTER_ID_ON_OFF) {
                if (attr_id == ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID) {
                    light_on = *(bool *)val_ptr;
                    ESP_LOGI(TAG, "light on/off: %d", light_on);
                    led_apply();
                } else if (attr_id == ESP_ZB_ZCL_ATTR_ON_OFF_START_UP_ON_OFF) {
                    cfg.startup_on_off = *(uint8_t *)val_ptr;
                    nvs_save("cfg", &cfg, sizeof(cfg));
                }
            } else if (msg->info.cluster == ESP_ZB_ZCL_CLUSTER_ID_LEVEL_CONTROL) {
                if (attr_id == ESP_ZB_ZCL_ATTR_LEVEL_CONTROL_CURRENT_LEVEL_ID) {
                    // see keep_brightness() for why 0 is skipped
                    light_level = keep_brightness(light_level, *(uint8_t *)val_ptr);
                    ESP_LOGI(TAG, "light level: %d", light_level);
                    led_apply();
                } else if (attr_id == ESP_ZB_ZCL_ATTR_LEVEL_CONTROL_ON_OFF_TRANSITION_TIME_ID) {
                    cfg.fade_time = *(uint16_t *)val_ptr;
                    nvs_save("cfg", &cfg, sizeof(cfg));
                }
            } else if (msg->info.cluster == ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL) {
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
            } else if (msg->info.cluster == SETTINGS_CLUSTER) {
                settings_write(attr_id, val_ptr);
            }
            break;
        }
        case ESP_ZB_CORE_OTA_UPGRADE_VALUE_CB_ID:
            return ota_handler((const esp_zb_zcl_ota_upgrade_value_message_t *)message);
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

    // The on/off, level and colour clusters start from the restored state
    // so Home Assistant reads what the strip is actually showing.
    // The two extra attributes are standard ones that ZHA shows as
    // settings by itself: power-on behaviour and on/off fade time.
    esp_zb_on_off_cluster_cfg_t on_off_cfg = {.on_off = light_on};
    esp_zb_attribute_list_t *on_off_cluster = esp_zb_on_off_cluster_create(&on_off_cfg);
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_zb_on_off_cluster_add_attr(on_off_cluster,
        ESP_ZB_ZCL_ATTR_ON_OFF_START_UP_ON_OFF, &cfg.startup_on_off));
    esp_zb_cluster_list_add_on_off_cluster(list, on_off_cluster,
        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    esp_zb_level_cluster_cfg_t level_cfg = {.current_level = light_level};
    esp_zb_attribute_list_t *level_cluster = esp_zb_level_cluster_create(&level_cfg);
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_zb_level_cluster_add_attr(level_cluster,
        ESP_ZB_ZCL_ATTR_LEVEL_CONTROL_ON_OFF_TRANSITION_TIME_ID, &cfg.fade_time));
    esp_zb_cluster_list_add_level_cluster(list, level_cluster,
        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    esp_zb_color_cluster_cfg_t color_cfg = {
        .current_x = light_x,
        .current_y = light_y,
        .color_mode = color_mode,
        .options = ESP_ZB_ZCL_COLOR_CONTROL_OPTIONS_DEFAULT_VALUE,
        .enhanced_color_mode = color_mode,
        .color_capabilities = 0x0009, // XY and HS
    };

    esp_zb_attribute_list_t *color_cluster = esp_zb_color_control_cluster_create(&color_cfg);

    // ADD THESE MANUALLY - The 'create' function often misses them!
    esp_zb_color_control_cluster_add_attr(color_cluster, ESP_ZB_ZCL_ATTR_COLOR_CONTROL_COLOR_MODE_ID, &color_mode);
    esp_zb_color_control_cluster_add_attr(color_cluster, ESP_ZB_ZCL_ATTR_COLOR_CONTROL_ENHANCED_COLOR_MODE_ID, &color_mode);
    esp_zb_color_control_cluster_add_attr(color_cluster, ESP_ZB_ZCL_ATTR_COLOR_CONTROL_CURRENT_HUE_ID, &light_hue);
    esp_zb_color_control_cluster_add_attr(color_cluster, ESP_ZB_ZCL_ATTR_COLOR_CONTROL_CURRENT_SATURATION_ID, &light_saturation);

    esp_zb_cluster_list_add_color_control_cluster(list, color_cluster, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    // Settings: one custom cluster, turned into entities in Home
    // Assistant by homeassistant/niftyfish_led.py
    esp_zb_attribute_list_t *settings_cluster = esp_zb_zcl_attr_list_create(SETTINGS_CLUSTER);
    for (int i = 0; i < SETTINGS_ATTR_COUNT; i++) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_zb_custom_cluster_add_custom_attr(settings_cluster,
            settings_attrs[i].id, settings_attrs[i].type,
            ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE, settings_attrs[i].value));
    }
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_zb_cluster_list_add_custom_cluster(list, settings_cluster,
        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE));

    // Chip temperature
    esp_zb_device_temp_config_cluster_cfg_t temp_cfg = {.current_temperature = 0};
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_zb_cluster_list_add_device_temp_config_cluster(list,
        esp_zb_device_temp_config_cluster_create(&temp_cfg),
        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE));

    // Estimated strip power, in 0.1 W
    esp_zb_electrical_meas_cluster_cfg_t power_cfg = {
        .measured_type = ESP_ZB_ZCL_ELECTRICAL_MEASUREMENT_ACTIVE_MEASUREMENT,
    };
    esp_zb_attribute_list_t *power_cluster = esp_zb_electrical_meas_cluster_create(&power_cfg);
    int16_t power = 0;
    uint16_t power_multiplier = 1, power_divisor = 10;
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_zb_electrical_meas_cluster_add_attr(power_cluster,
        ESP_ZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_ACTIVE_POWER_ID, &power));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_zb_electrical_meas_cluster_add_attr(power_cluster,
        ESP_ZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_ACPOWER_MULTIPLIER_ID, &power_multiplier));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_zb_electrical_meas_cluster_add_attr(power_cluster,
        ESP_ZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_ACPOWER_DIVISOR_ID, &power_divisor));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_zb_cluster_list_add_electrical_meas_cluster(list, power_cluster,
        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE));

    // Wireless firmware update. The values have to match the ones the
    // update file was made with (tools/make_ota.py).
    esp_zb_ota_cluster_cfg_t ota_cfg = {
        .ota_upgrade_file_version        = FW_VERSION,
        .ota_upgrade_downloaded_file_ver = ESP_ZB_ZCL_OTA_UPGRADE_DOWNLOADED_FILE_VERSION_DEF_VALUE,
        .ota_upgrade_manufacturer        = OTA_MANUFACTURER,
        .ota_upgrade_image_type          = OTA_IMAGE_TYPE,
    };
    esp_zb_attribute_list_t *ota_cluster = esp_zb_ota_cluster_create(&ota_cfg);
    esp_zb_zcl_ota_upgrade_client_variable_t ota_client = {
        .timer_query   = ESP_ZB_ZCL_OTA_UPGRADE_QUERY_TIMER_COUNT_DEF,
        .hw_version    = 1,
        .max_data_size = 223,  // block size the SDK recommends
    };
    uint16_t ota_server_addr = 0xffff;
    uint8_t  ota_server_ep   = 0xff;
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_zb_ota_cluster_add_attr(ota_cluster,
        ESP_ZB_ZCL_ATTR_OTA_UPGRADE_CLIENT_DATA_ID, &ota_client));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_zb_ota_cluster_add_attr(ota_cluster,
        ESP_ZB_ZCL_ATTR_OTA_UPGRADE_SERVER_ADDR_ID, &ota_server_addr));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_zb_ota_cluster_add_attr(ota_cluster,
        ESP_ZB_ZCL_ATTR_OTA_UPGRADE_SERVER_ENDPOINT_ID, &ota_server_ep));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_zb_cluster_list_add_ota_cluster(list, ota_cluster,
        ESP_ZB_ZCL_CLUSTER_CLIENT_ROLE));

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
    esp_zb_identify_notify_handler_register(LED_ENDPOINT, identify_handler);
}

// -------------------------------------------------------
// Zigbee stack
// -------------------------------------------------------

static void zigbee_task(void *arg) {
    // Router: always listening and relays for other devices, like a
    // mains-powered bulb. Needs CONFIG_ZB_ZCZR in sdkconfig.
    esp_zb_cfg_t zb_cfg = {
        .esp_zb_role         = ESP_ZB_DEVICE_TYPE_ROUTER,
        .install_code_policy = false,
        .nwk_cfg.zczr_cfg = {
            .max_children = 10,
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

// Called once the radio is up. Use the saved transmit power, or if none
// was ever set, show Home Assistant what the radio is using.
static void tx_power_init(void) {
    if (cfg.tx_power != TX_POWER_UNSET) {
        esp_zb_set_tx_power(cfg.tx_power);
        return;
    }
    esp_zb_get_tx_power(&cfg.tx_power);
    esp_zb_zcl_set_attribute_val(LED_ENDPOINT, SETTINGS_CLUSTER,
        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE, ATTR_TX_POWER, &cfg.tx_power, false);
}

// The stack starts these two standard settings at its own values, so
// Home Assistant showed power-on behaviour as Off while the board was
// using "previous". Put the values the firmware really uses back in.
static void standard_settings_sync(void) {
    esp_zb_zcl_set_attribute_val(LED_ENDPOINT, ESP_ZB_ZCL_CLUSTER_ID_ON_OFF,
        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE, ESP_ZB_ZCL_ATTR_ON_OFF_START_UP_ON_OFF,
        &cfg.startup_on_off, false);
    esp_zb_zcl_set_attribute_val(LED_ENDPOINT, ESP_ZB_ZCL_CLUSTER_ID_LEVEL_CONTROL,
        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE, ESP_ZB_ZCL_ATTR_LEVEL_CONTROL_ON_OFF_TRANSITION_TIME_ID,
        &cfg.fade_time, false);
}

void esp_zb_app_signal_handler(esp_zb_app_signal_t *signal_struct) {
    esp_err_t                err = signal_struct->esp_err_status;
    esp_zb_app_signal_type_t sig = *signal_struct->p_app_signal;

    switch (sig) {
        case ESP_ZB_ZDO_SIGNAL_SKIP_STARTUP:
            tx_power_init();
            sensors_update(0);
            esp_zb_bdb_start_top_level_commissioning(
                ESP_ZB_BDB_MODE_INITIALIZATION);
            break;

        case ESP_ZB_BDB_SIGNAL_DEVICE_FIRST_START:
        case ESP_ZB_BDB_SIGNAL_DEVICE_REBOOT:
            if (err == ESP_OK) {
                // The stack came up: a wirelessly updated build is good to
                // keep. One that crashes before this is rolled back.
                esp_ota_mark_app_valid_cancel_rollback();
                standard_settings_sync();
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
                ota_find_server();
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
    nvs_load("cfg", &cfg, sizeof(cfg));
    settings_clamp();
    state_load();
    ESP_LOGI(TAG, "firmware version %d, %d LEDs", FW_VERSION, cfg.led_count);

    temperature_sensor_config_t temp_cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
    ESP_ERROR_CHECK(temperature_sensor_install(&temp_cfg, &temp_sensor));
    ESP_ERROR_CHECK(temperature_sensor_enable(temp_sensor));

    esp_timer_create_args_t save_args = {.callback = state_save, .name = "state_save"};
    ESP_ERROR_CHECK(esp_timer_create(&save_args, &save_timer));

    xTaskCreate(led_task, "led_task", 4096, NULL, 5, &led_task_handle);
    xTaskCreate(zigbee_task, "zigbee_task", 8192, NULL, 5, NULL);
}
