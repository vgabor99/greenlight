/* Zigbee side of the device. See DESIGN.md section 6 for the data model this implements (rev 7): a single
 * endpoint with Basic, Power Configuration, Occupancy Sensing, the OTA Upgrade client (declared only) and the
 * custom cluster 0xFC8A. Its writable attributes carry the view and the dataset; its reported attributes are
 * the device state (wake_count, the HA push trigger; shown_view_id; dataset_hash; awake). Z2M needs the
 * external converter in z2m/greenlight.mjs to publish them. */

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

EventGroupHandle_t s_events;
zb_callbacks_t     s_cb;
uint32_t           s_shown_view_id;
uint32_t           s_ds_hash;
uint32_t           s_wake_count;
uint16_t           s_battery_mv;
static esp_timer_handle_t s_retry_timer;
static volatile bool      s_pairing;
static uint8_t            s_retry_mode;

/* ---- commissioning ---- */

static void retry_timer_cb(void *arg)
{
    esp_zigbee_lock_acquire(portMAX_DELAY);
    (void)ezb_bdb_start_top_level_commissioning(s_retry_mode);
    esp_zigbee_lock_release();
}

/* Diagnostic: log the parent and the other neighbours, to see whether a rejoin after a restart picks a new parent. */
static void log_neighbors_nolock(const char *when)
{
    static const char *rel[] = {"PARENT", "child", "sibling", "other"};
    ezb_nwk_info_iterator_t it = EZB_NWK_INFO_ITERATOR_INIT;
    ezb_nwk_neighbor_info_t n;
    ESP_LOGI(TAG, "neighbours (%s): own 0x%04hx", when, ezb_nwk_get_short_address());
    while (ezb_nwk_get_next_neighbor(&it, &n) == EZB_ERR_NONE) {
        ESP_LOGI(TAG, "  %-7s 0x%04hx %02x%02x%02x%02x%02x%02x%02x%02x type %u lqi %u rssi %d", rel[n.relationship & 3],
                 n.short_addr, n.ieee_addr.u8[7], n.ieee_addr.u8[6], n.ieee_addr.u8[5], n.ieee_addr.u8[4], n.ieee_addr.u8[3],
                 n.ieee_addr.u8[2], n.ieee_addr.u8[1], n.ieee_addr.u8[0], n.device_type, n.lqi, n.rssi);
    }
}

static void schedule_commissioning(uint8_t mode, uint32_t delay_ms)
{
    s_retry_mode = mode;
    esp_timer_stop(s_retry_timer);
    esp_timer_start_once(s_retry_timer, (uint64_t)delay_ms * 1000);
}

static bool app_signal_handler(const ezb_app_signal_t *app_signal)
{
    ezb_app_signal_type_t type = ezb_app_signal_get_type(app_signal);

    switch (type) {
    case EZB_ZDO_SIGNAL_SKIP_STARTUP:
        ESP_LOGI(TAG, "stack started, initializing");
        ezb_bdb_start_top_level_commissioning(EZB_BDB_MODE_INITIALIZATION);
        break;
    case EZB_BDB_SIGNAL_DEVICE_FIRST_START:
    case EZB_BDB_SIGNAL_DEVICE_REBOOT: {
        ezb_bdb_comm_status_t status = *((ezb_bdb_comm_status_t *)ezb_app_signal_get_params(app_signal));
        if (status == EZB_BDB_STATUS_SUCCESS) {
            bool factory_new = ezb_bdb_is_factory_new();
            ESP_LOGI(TAG, "%s", factory_new ? "factory new: no network" : "network restored");
            if (!factory_new) {
                log_neighbors_nolock("restored");
            }
            xEventGroupSetBits(s_events, BIT_STARTED | (factory_new ? BIT_FACTORY_NEW : BIT_JOINED));
        } else {
            ESP_LOGW(TAG, "%s failed with status 0x%02x, retrying", ezb_app_signal_to_string(type), status);
            if (type == EZB_BDB_SIGNAL_DEVICE_REBOOT) {
                /* The stack is up but cannot restore the stored network (yet); let the app go on and show REJOIN. */
                xEventGroupSetBits(s_events, BIT_STARTED);
            }
            schedule_commissioning(EZB_BDB_MODE_INITIALIZATION, 250);
        }
    } break;
    case EZB_BDB_SIGNAL_STEERING: {
        ezb_bdb_comm_status_t status = *((ezb_bdb_comm_status_t *)ezb_app_signal_get_params(app_signal));
        if (status == EZB_BDB_STATUS_SUCCESS) {
            ezb_extpanid_t ext_pan;
            ezb_nwk_get_extended_panid(&ext_pan);
            ESP_LOGI(TAG, "joined: PAN 0x%04hx, channel %d, short address 0x%04hx", ezb_nwk_get_panid(),
                     ezb_nwk_get_current_channel(), ezb_nwk_get_short_address());
            s_pairing = false;
            log_neighbors_nolock("joined");
            xEventGroupSetBits(s_events, BIT_JOINED);
            xEventGroupClearBits(s_events, BIT_FACTORY_NEW);
        } else if (s_pairing) {
            ESP_LOGW(TAG, "steering failed with status 0x%02x, retrying", status);
            schedule_commissioning(EZB_BDB_MODE_NETWORK_STEERING, 1000);
        }
    } break;
    case EZB_ZDO_SIGNAL_LEAVE:
        ESP_LOGI(TAG, "left the network");
        xEventGroupSetBits(s_events, BIT_LEFT);
        break;
    default:
        ESP_LOGI(TAG, "signal %s (0x%02x)", ezb_app_signal_to_string(type), type);
        break;
    }
    return true;
}

void core_action_handler(ezb_zcl_core_action_callback_id_t callback_id, void *message)
{
    switch (callback_id) {
    case EZB_ZCL_CORE_SET_ATTR_VALUE_CB_ID: {
        /* Tell the stack the write is fine; custom_write_attr() checks the content. */
        ezb_zcl_set_attr_value_message_t *m = message;
        ESP_LOGI(TAG, "set attr: cluster 0x%04x attr 0x%04x type 0x%02x size %u (stack status 0x%02x)", m->info.cluster_id,
                 m->in.attribute.id, m->in.attribute.data.type, m->in.attribute.data.size, m->info.status);
        m->out.result = EZB_ZCL_STATUS_SUCCESS;
    } break;
    case EZB_ZCL_CORE_DEFAULT_RSP_CB_ID: {
        ezb_zcl_cmd_default_rsp_message_t *rsp = message;
        ESP_LOGI(TAG, "default response: cluster 0x%04x cmd 0x%02x status 0x%02x", rsp->info.cluster_id,
                 rsp->in.rsp_to_cmd, rsp->in.status_code);
        if (rsp->in.rsp_to_cmd == EZB_ZCL_CMD_REPORT_ATTRIBUTES) {  /* only the goodbye reports ask for one */
            if (rsp->info.cluster_id == EZB_ZCL_CLUSTER_ID_OCCUPANCY_SENSING) {
                xEventGroupSetBits(s_events, BIT_ACK_OCC);
            } else if (rsp->info.cluster_id == CUSTOM_CLUSTER_ID) {
                xEventGroupSetBits(s_events, BIT_ACK_AWAKE);
            }
        }
    } break;
    default:
        ESP_LOGI(TAG, "ZCL core action 0x%04lx", (unsigned long)callback_id);
        break;
    }
}

static void stack_task(void *arg)
{
    esp_zigbee_config_t config = {
        .device_config = {
            .device_type         = EZB_NWK_DEVICE_TYPE_END_DEVICE,
            .install_code_policy = false,
            .zed_config          = {.ed_timeout = ED_TIMEOUT, .keep_alive = POLL_MS},
        },
        .platform_config = {
            .storage_partition_name = "nvs",
            .radio_config           = {.radio_mode = ESP_ZIGBEE_RADIO_MODE_NATIVE},
        },
    };
    ESP_ERROR_CHECK(esp_zigbee_init(&config));

    ezb_aps_secur_enable_distributed_security(false);
    ESP_ERROR_CHECK(ezb_bdb_set_primary_channel_set(ZB_ALL_CHANNELS_MASK));
    ESP_ERROR_CHECK(ezb_app_signal_add_handler(app_signal_handler));
    /* Sleepy end device: the parent queues messages for us and we poll it every POLL_MS (the first attempt with
     * rx-on-when-idle lost about half of Z2M's writes). Must be set before joining; changing it needs a re-pair. */
    ezb_nwk_set_rx_on_when_idle(false);

    ESP_ERROR_CHECK(create_device());
    ESP_ERROR_CHECK(esp_zigbee_start(false));
    esp_zigbee_launch_mainloop();
    esp_zigbee_deinit();
    vTaskDelete(NULL);
}

/* ---- public API ---- */

void zb_start(const zb_callbacks_t *cb, uint32_t ds_hash, uint32_t shown_view_id, uint32_t wake_count,
              uint16_t battery_mv)
{
    s_cb            = *cb;
    s_battery_mv    = battery_mv;
    s_ds_hash       = ds_hash;
    s_shown_view_id = shown_view_id;
    s_wake_count    = wake_count;
    stage_clear();
    s_events     = xEventGroupCreate();
    const esp_timer_create_args_t targs = {.callback = retry_timer_cb, .name = "zb_retry"};
    ESP_ERROR_CHECK(esp_timer_create(&targs, &s_retry_timer));
    xTaskCreate(stack_task, "zigbee", 6144, NULL, 5, NULL);
}

bool zb_wait_started(uint32_t timeout_ms, bool *factory_new)
{
    EventBits_t bits = xEventGroupWaitBits(s_events, BIT_STARTED, pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout_ms));
    if (!(bits & BIT_STARTED)) {
        return false;
    }
    *factory_new = (xEventGroupGetBits(s_events) & BIT_FACTORY_NEW) != 0;
    return true;
}

bool zb_joined(void)
{
    return s_events && (xEventGroupGetBits(s_events) & BIT_JOINED) != 0;  /* false before zb_start() */
}

void zb_pairing_start(void)
{
    s_pairing = true;
    esp_zigbee_lock_acquire(portMAX_DELAY);
    ezb_err_t err = ezb_bdb_start_top_level_commissioning(EZB_BDB_MODE_NETWORK_STEERING);
    esp_zigbee_lock_release();
    ESP_LOGI(TAG, "network steering started (0x%04x)", err);
}

void zb_pairing_stop(void)
{
    s_pairing = false;
    esp_timer_stop(s_retry_timer);
}

void zb_erase_network(void)
{
    if (zb_joined()) {
        xEventGroupClearBits(s_events, BIT_LEFT);
        esp_zigbee_lock_acquire(portMAX_DELAY);
        ezb_bdb_reset_via_local_action();  /* leave the network, clear the Zigbee data */
        esp_zigbee_lock_release();
        xEventGroupWaitBits(s_events, BIT_LEFT, pdFALSE, pdFALSE, pdMS_TO_TICKS(3000));
    }
    esp_zigbee_factory_reset();  /* clears all datasets and restarts the chip */
}

void zb_log_neighbors(void)
{
    esp_zigbee_lock_acquire(portMAX_DELAY);
    log_neighbors_nolock("heartbeat");
    esp_zigbee_lock_release();
}
