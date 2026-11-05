/* The data model (docs/zigbee.md#endpoint-and-clusters): one endpoint with Basic, Power Configuration, Occupancy Sensing, the custom
 * cluster 0xFC8A and the OTA Upgrade client. */

#include <string.h>
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_zigbee.h"
#include "ezbee/zha.h"
#include "zigbee/zigbee.h"
#include "zigbee/zigbee_internal.h"

static const char *TAG = "zb";

/* Attribute storage of the custom cluster (the stack keeps the attribute values here) */
static uint32_t view_attributes[4];   /* view_bits, view_ds, view_time, view_id */
static uint32_t commit_attribute;
static uint8_t  entry_attribute[ENTRY_MAX_CHARS + 1];
static uint32_t state_attributes[3];  /* wake_count, shown_view_id, dataset_hash */
static bool     awake_attribute;

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
    size_t length = strlen(text);
    if (length > size - 1) {
        length = size - 1;
    }
    buf[0] = (char)length;
    memcpy(buf + 1, text, length);
}

static ezb_zcl_cluster_desc_t create_basic_cluster(void)
{
    ezb_zcl_basic_cluster_server_config_t config = {
        .zcl_version  = EZB_ZCL_BASIC_ZCL_VERSION_DEFAULT_VALUE,
        .power_source = EZB_ZCL_BASIC_POWER_SOURCE_BATTERY,
    };
    ezb_zcl_cluster_desc_t basic = ezb_zcl_basic_create_cluster_desc(&config, EZB_ZCL_CLUSTER_SERVER);
    static char manufacturer[16], model[24], build_id[20], date_code[20];
    zcl_string(manufacturer, sizeof(manufacturer), "Greenlight");
    zcl_string(model, sizeof(model), "greenlight-eink42");
    zcl_string(build_id, sizeof(build_id), esp_app_get_description()->version);
    zcl_string(date_code, sizeof(date_code), esp_app_get_description()->date);
    ezb_zcl_basic_cluster_desc_add_attr(basic, EZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID, manufacturer);
    ezb_zcl_basic_cluster_desc_add_attr(basic, EZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID, model);
    ezb_zcl_basic_cluster_desc_add_attr(basic, EZB_ZCL_ATTR_BASIC_SW_BUILD_ID_ID, build_id);
    ezb_zcl_basic_cluster_desc_add_attr(basic, EZB_ZCL_ATTR_BASIC_DATE_CODE_ID, date_code);
    return basic;
}

/* The battery as measured at boot (simulated for now, docs/zigbee.md#battery) */
static ezb_zcl_cluster_desc_t create_power_config_cluster(void)
{
    ezb_zcl_power_config_cluster_server_config_t config = {
        .mains_voltage               = EZB_ZCL_POWER_CONFIG_MAINS_VOLTAGE_DEFAULT_VALUE,
        .mains_voltage_min_threshold = EZB_ZCL_POWER_CONFIG_MAINS_VOLTAGE_MIN_THRESHOLD_DEFAULT_VALUE,
        .mains_voltage_max_threshold = EZB_ZCL_POWER_CONFIG_MAINS_VOLTAGE_MAX_THRESHOLD_DEFAULT_VALUE,
    };
    ezb_zcl_cluster_desc_t power = ezb_zcl_power_config_create_cluster_desc(&config, EZB_ZCL_CLUSTER_SERVER);
    uint16_t mv = zigbee_context.battery_mv;
    uint8_t  voltage_100mv = mv / 100, half_percent = zigbee_battery_half_percent(mv);
    uint8_t  size = EZB_ZCL_POWER_CONFIG_BATTERY_SIZE_OTHER, quantity = 1, rated_voltage_100mv = 37;
    uint16_t capacity_10mah = 100;  /* 1000 mAh */
    uint32_t alarm_state = 0;
    ADD_ATTR(ezb_zcl_power_config_cluster_desc_add_attr, power, EZB_ZCL_ATTR_POWER_CONFIG_BATTERY_VOLTAGE_ID, &voltage_100mv);
    ADD_ATTR(ezb_zcl_power_config_cluster_desc_add_attr, power, EZB_ZCL_ATTR_POWER_CONFIG_BATTERY_PERCENTAGE_REMAINING_ID, &half_percent);
    ADD_ATTR(ezb_zcl_power_config_cluster_desc_add_attr, power, EZB_ZCL_ATTR_POWER_CONFIG_BATTERY_SIZE_ID, &size);
    ADD_ATTR(ezb_zcl_power_config_cluster_desc_add_attr, power, EZB_ZCL_ATTR_POWER_CONFIG_BATTERY_QUANTITY_ID, &quantity);
    ADD_ATTR(ezb_zcl_power_config_cluster_desc_add_attr, power, EZB_ZCL_ATTR_POWER_CONFIG_BATTERY_RATED_VOLTAGE_ID, &rated_voltage_100mv);
    ADD_ATTR(ezb_zcl_power_config_cluster_desc_add_attr, power, EZB_ZCL_ATTR_POWER_CONFIG_BATTERY_A_HR_RATING_ID, &capacity_10mah);
    ADD_ATTR(ezb_zcl_power_config_cluster_desc_add_attr, power, EZB_ZCL_ATTR_POWER_CONFIG_BATTERY_ALARM_STATE_ID, &alarm_state);
    return power;
}

/* The PIR */
static ezb_zcl_cluster_desc_t create_occupancy_cluster(void)
{
    ezb_zcl_occupancy_sensing_cluster_server_config_t config = {
        .occupancy                    = 0,
        .occupancy_sensor_type        = EZB_ZCL_OCCUPANCY_SENSING_OCCUPANCY_SENSOR_TYPE_PIR,
        .occupancy_sensor_type_bitmap = EZB_ZCL_OCCUPANCY_SENSING_OCCUPANCY_SENSOR_TYPE_BITMAP_PIR,
    };
    return ezb_zcl_occupancy_sensing_create_cluster_desc(&config, EZB_ZCL_CLUSTER_SERVER);
}

/* Declared so the cluster list stays final (docs/zigbee.md#endpoint-and-clusters), not implemented yet */
static ezb_zcl_cluster_desc_t create_ota_cluster(void)
{
    ezb_zcl_ota_upgrade_cluster_client_config_t config = {
        .upgrade_server_id    = EZB_ZCL_OTA_UPGRADE_UPGRADE_SERVER_ID_DEFAULT_VALUE,
        .file_offset          = EZB_ZCL_OTA_UPGRADE_FILE_OFFSET_DEFAULT_VALUE,
        .image_upgrade_status = EZB_ZCL_OTA_UPGRADE_IMAGE_UPGRADE_STATUS_DEFAULT_VALUE,
    };
    return ezb_zcl_ota_upgrade_create_cluster_desc(&config, EZB_ZCL_CLUSTER_CLIENT);
}

static void remove_custom_cluster(uint8_t endpoint)
{
}

/* Add an attribute to the custom cluster and say so if the stack refuses it. */
static void add_custom_attribute(ezb_zcl_cluster_desc_t cluster, uint16_t attribute, uint8_t type, uint8_t access,
                                 void *storage)
{
    ezb_err_t err = ezb_zcl_custom_cluster_desc_add_attr(cluster, attribute, type, access, storage);
    if (err != EZB_ERR_NONE) {
        ESP_LOGW(TAG, "adding attribute 0x%04x to custom failed: 0x%04x", attribute, err);
    }
}

/* The view and the dataset input, and the reported state. */
static ezb_zcl_cluster_desc_t create_custom_cluster(void)
{
    const uint8_t writable = EZB_ZCL_ATTR_ACCESS_READ | EZB_ZCL_ATTR_ACCESS_WRITE;
    const uint8_t reported = EZB_ZCL_ATTR_ACCESS_READ | EZB_ZCL_ATTR_ACCESS_REPORTING;
    ezb_zcl_custom_cluster_config_t config = {
        .cluster_id  = CUSTOM_CLUSTER_ID,
        .init_func   = zigbee_display_input_register_handlers,
        .deinit_func = remove_custom_cluster,
    };
    ezb_zcl_cluster_desc_t custom = ezb_zcl_custom_create_cluster_desc(&config, EZB_ZCL_CLUSTER_SERVER);
    for (int i = 0; i < 4; i++) {  /* view_bits, view_ds, view_time, view_id */
        add_custom_attribute(custom, ATTR_VIEW_BITS + i, EZB_ZCL_ATTR_TYPE_UINT32, writable, &view_attributes[i]);
    }
    /* The stack takes the storage size of a string attribute from the initial value, so register a full-length
     * placeholder (not a valid entry). */
    entry_attribute[0] = ENTRY_MAX_CHARS;
    memset(&entry_attribute[1], ' ', ENTRY_MAX_CHARS);
    add_custom_attribute(custom, ATTR_DATASET_ENTRY, EZB_ZCL_ATTR_TYPE_STRING, writable, entry_attribute);
    add_custom_attribute(custom, ATTR_DATASET_COMMIT, EZB_ZCL_ATTR_TYPE_UINT32, writable, &commit_attribute);
    state_attributes[0] = zigbee_context.wake_count;
    state_attributes[1] = zigbee_context.shown_view_id;
    state_attributes[2] = zigbee_context.dataset_hash;
    for (int i = 0; i < 3; i++) {  /* wake_count, shown_view_id, dataset_hash */
        add_custom_attribute(custom, ATTR_WAKE_COUNT + i, EZB_ZCL_ATTR_TYPE_UINT32, reported, &state_attributes[i]);
    }
    add_custom_attribute(custom, ATTR_AWAKE, EZB_ZCL_ATTR_TYPE_BOOL, reported, &awake_attribute);
    add_custom_attribute(custom, ATTR_BATTERY_MV, EZB_ZCL_ATTR_TYPE_UINT16, reported, &zigbee_context.battery_mv);
    return custom;
}

esp_err_t zigbee_device_register(void)
{
    ezb_af_device_desc_t device = ezb_af_create_device_desc();
    ezb_af_ep_config_t   endpoint_config = {
        .ep_id              = DEVICE_ENDPOINT,
        .app_profile_id     = EZB_AF_HA_PROFILE_ID,
        .app_device_id      = EZB_ZHA_SIMPLE_SENSOR_DEVICE_ID,
        .app_device_version = 0,
    };
    ezb_af_ep_desc_t endpoint = ezb_af_create_endpoint_desc(&endpoint_config);
    ESP_ERROR_CHECK(ezb_af_endpoint_add_cluster_desc(endpoint, create_basic_cluster()));
    ESP_ERROR_CHECK(ezb_af_endpoint_add_cluster_desc(endpoint, create_power_config_cluster()));
    ESP_ERROR_CHECK(ezb_af_endpoint_add_cluster_desc(endpoint, create_occupancy_cluster()));
    ESP_ERROR_CHECK(ezb_af_endpoint_add_cluster_desc(endpoint, create_custom_cluster()));
    ESP_ERROR_CHECK(ezb_af_endpoint_add_cluster_desc(endpoint, create_ota_cluster()));
    ESP_ERROR_CHECK(ezb_af_device_add_endpoint_desc(device, endpoint));
    ESP_ERROR_CHECK(ezb_af_device_desc_register(device));

    ezb_zcl_core_action_handler_register(zigbee_handle_core_action);
    return ESP_OK;
}
