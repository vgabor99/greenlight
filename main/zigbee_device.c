#include <stdio.h>
#include <string.h>
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "esp_zigbee.h"
#include "ezbee/zha.h"
#include "hex.h"
#include "utf8.h"
#include "zigbee.h"
#include "zigbee_internal.h"

static const char *TAG = "zb";

/* Attribute storage of the custom cluster (the stack keeps the written values here). */
static uint32_t s_view_attr[4];
static uint32_t s_commit_attr;
static uint8_t  s_entry_attr[ENTRY_MAX + 1];
static uint32_t s_state_attr[3];  /* wake_count, shown_view_id, dataset_hash */
static bool     s_awake_attr;
static char     s_build_id[20];
static char    s_date_code[20];

/* ---- helpers ---- */

/* Add an attribute to a cluster descriptor and say so if the stack refuses it. */
#define ADD_ATTR(fn, desc, id, value)                                                       \
    do {                                                                                    \
        ezb_err_t e_ = fn(desc, id, value);                                                 \
        if (e_ != EZB_ERR_NONE) {                                                           \
            ESP_LOGW(TAG, "adding attribute 0x%04x to " #desc " failed: 0x%04x", (id), e_); \
        }                                                                                   \
    } while (0)

/* Build a ZCL character string (length byte + text) in buf. */
static void zcl_string(char *buf, size_t size, const char *text)
{
    size_t n = strlen(text);
    if (n > size - 1) {
        n = size - 1;
    }
    buf[0] = (char)n;
    memcpy(buf + 1, text, n);
}

/* ---- data model ---- */

esp_err_t create_device(void)
{
    ezb_af_device_desc_t dev = ezb_af_create_device_desc();

    /* Basic */
    ezb_zcl_basic_cluster_server_config_t basic_cfg = {
        .zcl_version  = EZB_ZCL_BASIC_ZCL_VERSION_DEFAULT_VALUE,
        .power_source = EZB_ZCL_BASIC_POWER_SOURCE_BATTERY,
    };
    ezb_zcl_cluster_desc_t basic = ezb_zcl_basic_create_cluster_desc(&basic_cfg, EZB_ZCL_CLUSTER_SERVER);
    static char manufacturer[16], model[24];
    zcl_string(manufacturer, sizeof(manufacturer), "Greenlight");
    zcl_string(model, sizeof(model), "greenlight-eink42");
    zcl_string(s_build_id, sizeof(s_build_id), esp_app_get_description()->version);
    zcl_string(s_date_code, sizeof(s_date_code), esp_app_get_description()->date);
    ezb_zcl_basic_cluster_desc_add_attr(basic, EZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID, manufacturer);
    ezb_zcl_basic_cluster_desc_add_attr(basic, EZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID, model);
    ezb_zcl_basic_cluster_desc_add_attr(basic, EZB_ZCL_ATTR_BASIC_SW_BUILD_ID_ID, s_build_id);
    ezb_zcl_basic_cluster_desc_add_attr(basic, EZB_ZCL_ATTR_BASIC_DATE_CODE_ID, s_date_code);

    /* Power Configuration: the battery as measured at boot (simulated for now, DESIGN.md 6.6) */
    ezb_zcl_power_config_cluster_server_config_t power_cfg = {
        .mains_voltage               = EZB_ZCL_POWER_CONFIG_MAINS_VOLTAGE_DEFAULT_VALUE,
        .mains_voltage_min_threshold = EZB_ZCL_POWER_CONFIG_MAINS_VOLTAGE_MIN_THRESHOLD_DEFAULT_VALUE,
        .mains_voltage_max_threshold = EZB_ZCL_POWER_CONFIG_MAINS_VOLTAGE_MAX_THRESHOLD_DEFAULT_VALUE,
    };
    ezb_zcl_cluster_desc_t power = ezb_zcl_power_config_create_cluster_desc(&power_cfg, EZB_ZCL_CLUSTER_SERVER);
    uint8_t  voltage = s_battery_mv / 100, percent = battery_half_percent(s_battery_mv);
    uint8_t  size = EZB_ZCL_POWER_CONFIG_BATTERY_SIZE_OTHER, quantity = 1, rated = 37;
    uint16_t ahr = 100;  /* 1000 mAh in units of 10 mAh */
    uint32_t alarm = 0;
    ADD_ATTR(ezb_zcl_power_config_cluster_desc_add_attr, power, EZB_ZCL_ATTR_POWER_CONFIG_BATTERY_VOLTAGE_ID, &voltage);
    ADD_ATTR(ezb_zcl_power_config_cluster_desc_add_attr, power, EZB_ZCL_ATTR_POWER_CONFIG_BATTERY_PERCENTAGE_REMAINING_ID, &percent);
    ADD_ATTR(ezb_zcl_power_config_cluster_desc_add_attr, power, EZB_ZCL_ATTR_POWER_CONFIG_BATTERY_SIZE_ID, &size);
    ADD_ATTR(ezb_zcl_power_config_cluster_desc_add_attr, power, EZB_ZCL_ATTR_POWER_CONFIG_BATTERY_QUANTITY_ID, &quantity);
    ADD_ATTR(ezb_zcl_power_config_cluster_desc_add_attr, power, EZB_ZCL_ATTR_POWER_CONFIG_BATTERY_RATED_VOLTAGE_ID, &rated);
    ADD_ATTR(ezb_zcl_power_config_cluster_desc_add_attr, power, EZB_ZCL_ATTR_POWER_CONFIG_BATTERY_A_HR_RATING_ID, &ahr);
    ADD_ATTR(ezb_zcl_power_config_cluster_desc_add_attr, power, EZB_ZCL_ATTR_POWER_CONFIG_BATTERY_ALARM_STATE_ID, &alarm);

    /* Occupancy Sensing: the PIR */
    ezb_zcl_occupancy_sensing_cluster_server_config_t occ_cfg = {
        .occupancy                    = 0,
        .occupancy_sensor_type        = EZB_ZCL_OCCUPANCY_SENSING_OCCUPANCY_SENSOR_TYPE_PIR,
        .occupancy_sensor_type_bitmap = EZB_ZCL_OCCUPANCY_SENSING_OCCUPANCY_SENSOR_TYPE_BITMAP_PIR,
    };
    ezb_zcl_cluster_desc_t occupancy = ezb_zcl_occupancy_sensing_create_cluster_desc(&occ_cfg, EZB_ZCL_CLUSTER_SERVER);

    /* OTA Upgrade client: declared so the cluster list stays final (DESIGN.md 6.3), not implemented yet */
    ezb_zcl_ota_upgrade_cluster_client_config_t ota_cfg = {
        .upgrade_server_id    = EZB_ZCL_OTA_UPGRADE_UPGRADE_SERVER_ID_DEFAULT_VALUE,
        .file_offset          = EZB_ZCL_OTA_UPGRADE_FILE_OFFSET_DEFAULT_VALUE,
        .image_upgrade_status = EZB_ZCL_OTA_UPGRADE_IMAGE_UPGRADE_STATUS_DEFAULT_VALUE,
    };
    ezb_zcl_cluster_desc_t ota = ezb_zcl_ota_upgrade_create_cluster_desc(&ota_cfg, EZB_ZCL_CLUSTER_CLIENT);

    /* Custom cluster: the view and the dataset input, and the reported state. The stack takes the storage size of
     * a string attribute from the initial value, so register a full-length placeholder (not a valid entry). */
    s_entry_attr[0] = ENTRY_MAX;
    memset(&s_entry_attr[1], ' ', ENTRY_MAX);
    ezb_zcl_custom_cluster_config_t custom_cfg = {
        .cluster_id  = CUSTOM_CLUSTER_ID,
        .init_func   = custom_cluster_init,
        .deinit_func = custom_cluster_deinit,
    };
    ezb_zcl_cluster_desc_t custom = ezb_zcl_custom_create_cluster_desc(&custom_cfg, EZB_ZCL_CLUSTER_SERVER);
    for (int i = 0; i < 4; i++) {  /* view_bits, view_ds, view_time, view_id */
        ezb_zcl_custom_cluster_desc_add_attr(custom, ATTR_VIEW_BITS + i, EZB_ZCL_ATTR_TYPE_UINT32,
                                             EZB_ZCL_ATTR_ACCESS_READ | EZB_ZCL_ATTR_ACCESS_WRITE, &s_view_attr[i]);
    }
    ezb_zcl_custom_cluster_desc_add_attr(custom, ATTR_DS_ENTRY, EZB_ZCL_ATTR_TYPE_STRING,
                                         EZB_ZCL_ATTR_ACCESS_READ | EZB_ZCL_ATTR_ACCESS_WRITE, s_entry_attr);
    ezb_zcl_custom_cluster_desc_add_attr(custom, ATTR_DS_COMMIT, EZB_ZCL_ATTR_TYPE_UINT32,
                                         EZB_ZCL_ATTR_ACCESS_READ | EZB_ZCL_ATTR_ACCESS_WRITE, &s_commit_attr);
    s_state_attr[0] = s_wake_count;
    s_state_attr[1] = s_shown_view_id;
    s_state_attr[2] = s_ds_hash;
    for (int i = 0; i < 3; i++) {  /* wake_count, shown_view_id, dataset_hash */
        ezb_zcl_custom_cluster_desc_add_attr(custom, ATTR_WAKE_COUNT + i, EZB_ZCL_ATTR_TYPE_UINT32,
                                             EZB_ZCL_ATTR_ACCESS_READ | EZB_ZCL_ATTR_ACCESS_REPORTING, &s_state_attr[i]);
    }
    ezb_zcl_custom_cluster_desc_add_attr(custom, ATTR_AWAKE, EZB_ZCL_ATTR_TYPE_BOOL,
                                         EZB_ZCL_ATTR_ACCESS_READ | EZB_ZCL_ATTR_ACCESS_REPORTING, &s_awake_attr);
    ezb_zcl_custom_cluster_desc_add_attr(custom, ATTR_BATTERY_MV, EZB_ZCL_ATTR_TYPE_UINT16,
                                         EZB_ZCL_ATTR_ACCESS_READ | EZB_ZCL_ATTR_ACCESS_REPORTING, &s_battery_mv);

    ezb_af_ep_config_t ep_cfg = {
        .ep_id              = ZB_EP,
        .app_profile_id     = EZB_AF_HA_PROFILE_ID,
        .app_device_id      = EZB_ZHA_SIMPLE_SENSOR_DEVICE_ID,
        .app_device_version = 0,
    };
    ezb_af_ep_desc_t ep = ezb_af_create_endpoint_desc(&ep_cfg);
    ESP_ERROR_CHECK(ezb_af_endpoint_add_cluster_desc(ep, basic));
    ESP_ERROR_CHECK(ezb_af_endpoint_add_cluster_desc(ep, power));
    ESP_ERROR_CHECK(ezb_af_endpoint_add_cluster_desc(ep, occupancy));
    ESP_ERROR_CHECK(ezb_af_endpoint_add_cluster_desc(ep, custom));
    ESP_ERROR_CHECK(ezb_af_endpoint_add_cluster_desc(ep, ota));
    ESP_ERROR_CHECK(ezb_af_device_add_endpoint_desc(dev, ep));
    ESP_ERROR_CHECK(ezb_af_device_desc_register(dev));

    ezb_zcl_core_action_handler_register(core_action_handler);
    return ESP_OK;
}
