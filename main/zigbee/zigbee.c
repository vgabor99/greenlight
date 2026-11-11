/* Zigbee side of the device: the stack and the network. See docs/zigbee.md for the data model this implements
 * (rev 7): a single endpoint with Basic, Power Configuration, Occupancy Sensing, the OTA Upgrade client (declared
 * only) and the custom cluster 0xFC8A (zigbee_device.c). Its writable attributes carry the view and the dataset
 * (zigbee_display_input.c); its reported attributes are the device state (zigbee_reports.c). Z2M needs the
 * external converter in z2m/greenlight.mjs to publish them. */

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "esp_zigbee.h"
#include "zigbee/zigbee.h"
#include "zigbee/zigbee_internal.h"

static const char *TAG = "zb";

/* End device timeout requested from the parent. The longest value (16384 min) made the join fail with a TCLK
 * exchange error on Z2M, so use the value of the Espressif examples. A wake after a longer sleep rejoins fine
 * (docs/behaviour.md#deep-sleep). */
#define ALL_CHANNELS_MASK   0x07FFF800U  /* channels 11-26 */
#define END_DEVICE_TIMEOUT  EZB_NWK_ED_TIMEOUT_64MIN
#define PARENT_POLL_MS      50    /* how often the awake device polls its parent for queued messages (docs/zigbee.md#frame-size-and-polling) */

zigbee_context_t zigbee_context;

static esp_timer_handle_t commissioning_retry_timer;
static uint8_t            commissioning_retry_mode;
static volatile bool      pairing;

/* ---- Commissioning ---- */

static void retry_commissioning(void *arg)
{
    esp_zigbee_lock_acquire(portMAX_DELAY);
    (void)ezb_bdb_start_top_level_commissioning(commissioning_retry_mode);
    esp_zigbee_lock_release();
}

static void schedule_commissioning(uint8_t mode, uint32_t delay_ms)
{
    commissioning_retry_mode = mode;
    esp_timer_stop(commissioning_retry_timer);
    esp_timer_start_once(commissioning_retry_timer, (uint64_t)delay_ms * 1000);
}

/* Diagnostic: log the parent and the other neighbours, to see whether a rejoin after a restart picks a new parent.
 * Lock held. */
static void log_neighbors_lock_held(const char *when)
{
    static const char *relationship_names[] = {"PARENT", "child", "sibling", "other"};
    ezb_nwk_info_iterator_t iterator = EZB_NWK_INFO_ITERATOR_INIT;
    ezb_nwk_neighbor_info_t neighbor;
    ESP_LOGI(TAG, "neighbours (%s): own 0x%04hx", when, ezb_nwk_get_short_address());
    while (ezb_nwk_get_next_neighbor(&iterator, &neighbor) == EZB_ERR_NONE) {
        const uint8_t *ieee = neighbor.ieee_addr.u8;
        ESP_LOGI(TAG, "  %-7s 0x%04hx %02x%02x%02x%02x%02x%02x%02x%02x type %u lqi %u rssi %d",
                 relationship_names[neighbor.relationship & 3], neighbor.short_addr, ieee[7], ieee[6], ieee[5], ieee[4],
                 ieee[3], ieee[2], ieee[1], ieee[0], neighbor.device_type, neighbor.lqi, neighbor.rssi);
    }
}

/* The stack started (first start or reboot) and tried to restore a network. */
static void handle_device_start(ezb_app_signal_type_t type, ezb_bdb_comm_status_t status)
{
    if (status == EZB_BDB_STATUS_SUCCESS) {
        bool factory_new = ezb_bdb_is_factory_new();
        ESP_LOGI(TAG, "%s", factory_new ? "factory new: no network" : "network restored");
        if (!factory_new) {
            log_neighbors_lock_held("restored");
        }
        xEventGroupSetBits(zigbee_context.events, EVENT_STARTED | (factory_new ? EVENT_FACTORY_NEW : EVENT_JOINED));
        return;
    }
    ESP_LOGW(TAG, "%s failed with status 0x%02x, retrying", ezb_app_signal_to_string(type), status);
    if (type == EZB_BDB_SIGNAL_DEVICE_REBOOT) {
        /* The stack is up but cannot restore the stored network (yet); let the app go on and show REJOIN. */
        xEventGroupSetBits(zigbee_context.events, EVENT_STARTED);
    }
    schedule_commissioning(EZB_BDB_MODE_INITIALIZATION, 250);
}

/* Network steering (pairing) ended. */
static void handle_steering(ezb_bdb_comm_status_t status)
{
    if (status == EZB_BDB_STATUS_SUCCESS) {
        ezb_extpanid_t extended_pan_id;
        ezb_nwk_get_extended_panid(&extended_pan_id);
        ESP_LOGI(TAG, "joined: PAN 0x%04hx, channel %d, short address 0x%04hx", ezb_nwk_get_panid(),
                 ezb_nwk_get_current_channel(), ezb_nwk_get_short_address());
        pairing = false;
        log_neighbors_lock_held("joined");
        xEventGroupSetBits(zigbee_context.events, EVENT_JOINED);
        xEventGroupClearBits(zigbee_context.events, EVENT_FACTORY_NEW);
    } else if (pairing) {
        ESP_LOGW(TAG, "steering failed with status 0x%02x, retrying", status);
        schedule_commissioning(EZB_BDB_MODE_NETWORK_STEERING, 1000);
    }
}

static ezb_bdb_comm_status_t signal_status(const ezb_app_signal_t *signal)
{
    return *((ezb_bdb_comm_status_t *)ezb_app_signal_get_params(signal));
}

static bool handle_app_signal(const ezb_app_signal_t *signal)
{
    ezb_app_signal_type_t type = ezb_app_signal_get_type(signal);
    switch (type) {
    case EZB_ZDO_SIGNAL_SKIP_STARTUP:
        ESP_LOGI(TAG, "stack started, initializing");
        ezb_bdb_start_top_level_commissioning(EZB_BDB_MODE_INITIALIZATION);
        break;
    case EZB_BDB_SIGNAL_DEVICE_FIRST_START:
    case EZB_BDB_SIGNAL_DEVICE_REBOOT:
        handle_device_start(type, signal_status(signal));
        break;
    case EZB_BDB_SIGNAL_STEERING:
        handle_steering(signal_status(signal));
        break;
    case EZB_ZDO_SIGNAL_LEAVE:
        ESP_LOGI(TAG, "left the network");
        xEventGroupSetBits(zigbee_context.events, EVENT_LEFT);
        break;
    default:
        ESP_LOGI(TAG, "signal %s (0x%02x)", ezb_app_signal_to_string(type), type);
        break;
    }
    return true;
}

/* ---- ZCL core actions ---- */

/* Tell the stack the write is fine; zigbee_display_input.c checks the content. */
static void accept_attribute_write(ezb_zcl_set_attr_value_message_t *message)
{
    ESP_LOGI(TAG, "set attr: cluster 0x%04x attr 0x%04x type 0x%02x size %u (stack status 0x%02x)",
             message->info.cluster_id, message->in.attribute.id, message->in.attribute.data.type,
             message->in.attribute.data.size, message->info.status);
    message->out.result = EZB_ZCL_STATUS_SUCCESS;
}

void zigbee_handle_core_action(ezb_zcl_core_action_callback_id_t callback_id, void *message)
{
    switch (callback_id) {
    case EZB_ZCL_CORE_SET_ATTR_VALUE_CB_ID:
        accept_attribute_write(message);
        break;
    case EZB_ZCL_CORE_DEFAULT_RSP_CB_ID:
        zigbee_reports_on_default_response(message);
        break;
    default:
        ESP_LOGI(TAG, "ZCL core action 0x%04lx", (unsigned long)callback_id);
        break;
    }
}

/* ---- The stack ---- */

static void stack_task(void *arg)
{
    esp_zigbee_config_t config = {
        .device_config = {
            .device_type         = EZB_NWK_DEVICE_TYPE_END_DEVICE,
            .install_code_policy = false,
            .zed_config          = {.ed_timeout = END_DEVICE_TIMEOUT, .keep_alive = PARENT_POLL_MS},
        },
        .platform_config = {
            .storage_partition_name = "nvs",
            .radio_config           = {.radio_mode = ESP_ZIGBEE_RADIO_MODE_NATIVE},
        },
    };
    ESP_ERROR_CHECK(esp_zigbee_init(&config));

    ezb_aps_secur_enable_distributed_security(false);
    ESP_ERROR_CHECK(ezb_bdb_set_primary_channel_set(ALL_CHANNELS_MASK));
    ESP_ERROR_CHECK(ezb_app_signal_add_handler(handle_app_signal));
    /* Sleepy end device: the parent queues messages for us and we poll it every PARENT_POLL_MS (the first attempt
     * with rx-on-when-idle lost about half of Z2M's writes). Must be set before joining; changing it needs a
     * re-pair. */
    ezb_nwk_set_rx_on_when_idle(false);

    ESP_ERROR_CHECK(zigbee_device_register());
    ESP_ERROR_CHECK(esp_zigbee_start(false));
    esp_zigbee_launch_mainloop();
    esp_zigbee_deinit();
    vTaskDelete(NULL);
}

/* ---- Public API ---- */

void zigbee_start(const zigbee_callbacks_t *callbacks, uint32_t dataset_hash, uint32_t shown_view_id,
                  uint32_t wake_count, uint16_t battery_mv)
{
    zigbee_context.callbacks     = *callbacks;
    zigbee_context.battery_mv    = battery_mv;
    zigbee_context.dataset_hash  = dataset_hash;
    zigbee_context.shown_view_id = shown_view_id;
    zigbee_context.wake_count    = wake_count;
    zigbee_display_input_init();
    zigbee_context.events = xEventGroupCreate();
    const esp_timer_create_args_t timer_args = {.callback = retry_commissioning, .name = "zb_retry"};
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &commissioning_retry_timer));
    xTaskCreate(stack_task, "zigbee", 6144, NULL, 5, NULL);
}

bool zigbee_wait_started(uint32_t timeout_ms, bool *factory_new)
{
    EventBits_t bits = xEventGroupWaitBits(zigbee_context.events, EVENT_STARTED, pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(timeout_ms));
    if (!(bits & EVENT_STARTED)) {
        return false;
    }
    *factory_new = (xEventGroupGetBits(zigbee_context.events) & EVENT_FACTORY_NEW) != 0;
    return true;
}

bool zigbee_is_joined(void)
{
    /* false before zigbee_start() */
    return zigbee_context.events && (xEventGroupGetBits(zigbee_context.events) & EVENT_JOINED) != 0;
}

void zigbee_start_pairing(void)
{
    pairing = true;
    esp_zigbee_lock_acquire(portMAX_DELAY);
    ezb_err_t err = ezb_bdb_start_top_level_commissioning(EZB_BDB_MODE_NETWORK_STEERING);
    esp_zigbee_lock_release();
    ESP_LOGI(TAG, "network steering started (0x%04x)", err);
}

void zigbee_stop_pairing(void)
{
    pairing = false;
    esp_timer_stop(commissioning_retry_timer);
}

void zigbee_erase_network(void)
{
    if (zigbee_is_joined()) {
        xEventGroupClearBits(zigbee_context.events, EVENT_LEFT);
        esp_zigbee_lock_acquire(portMAX_DELAY);
        ezb_bdb_reset_via_local_action();  /* leave the network, clear the Zigbee data */
        esp_zigbee_lock_release();
        xEventGroupWaitBits(zigbee_context.events, EVENT_LEFT, pdFALSE, pdFALSE, pdMS_TO_TICKS(3000));
    }
    esp_zigbee_factory_reset();  /* clears all datasets and restarts the chip */
}

void zigbee_log_neighbors(void)
{
    esp_zigbee_lock_acquire(portMAX_DELAY);
    log_neighbors_lock_held("heartbeat");
    esp_zigbee_lock_release();
}
