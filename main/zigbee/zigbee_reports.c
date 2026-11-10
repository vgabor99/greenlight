/* Reports to the coordinator (docs/zigbee.md#state-device-to-ha): the device state, the occupancy and the battery. */

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "esp_zigbee.h"
#include "util/elapsed_time.h"
#include "zigbee/zigbee.h"
#include "zigbee/zigbee_internal.h"

static const char *TAG = "zb";

/* Battery (docs/zigbee.md#battery): batteryAlarmState bit 0 (BatteryMinThreshold) = low, Z2M's battery_low */
#define BATTERY_ALARM_LOW 0x00000001u
#define GOODBYE_RETRY_MS  1000  /* zigbee_report_goodbye(), zigbee_report_battery_low(): resend this often */

/* ---- Helpers (lock held) ---- */

/* ask_for_response asks the coordinator for a default response: only the goodbye reports do, so nothing else adds
 * a downlink. */
void zigbee_send_report(uint16_t cluster, uint16_t attribute, bool ask_for_response)
{
    ezb_zcl_report_attr_cmd_t command = {
        .cmd_ctrl = {
            .dst_addr   = EZB_ADDRESS_SHORT(0x0000),
            .dst_ep     = COORDINATOR_ENDPOINT,
            .src_ep     = DEVICE_ENDPOINT,
            .cluster_id = cluster,
            .manuf_code = EZB_ZCL_STD_MANUF_CODE,
            .fc = {.manuf_specific = 0, .direction = EZB_ZCL_CMD_DIRECTION_TO_CLI, .dis_default_rsp = !ask_for_response},
        },
        .payload = {.attr_id = attribute},
    };
    ezb_err_t err = ezb_zcl_report_attr_cmd_req(&command);
    if (err != EZB_ERR_NONE) {
        ESP_LOGW(TAG, "report cluster 0x%04x attr 0x%04x failed: 0x%04x", cluster, attribute, err);
    }
}

void zigbee_set_attribute(uint16_t cluster, uint16_t attribute, void *value)
{
    ezb_zcl_status_t status = ezb_zcl_set_attr_value(DEVICE_ENDPOINT, cluster, EZB_ZCL_CLUSTER_SERVER, attribute,
                                                     EZB_ZCL_STD_MANUF_CODE, value, false);
    if (status != EZB_ZCL_STATUS_SUCCESS) {
        ESP_LOGW(TAG, "set cluster 0x%04x attr 0x%04x failed: 0x%02x", cluster, attribute, status);
    }
}

static void set_and_report(uint16_t cluster, uint16_t attribute, void *value)
{
    zigbee_set_attribute(cluster, attribute, value);
    zigbee_send_report(cluster, attribute, false);
}

void zigbee_report_shown_view_id_lock_held(void)
{
    set_and_report(CUSTOM_CLUSTER_ID, ATTR_SHOWN_VIEW_ID, &zigbee_context.shown_view_id);
}

void zigbee_report_dataset_hash_lock_held(void)
{
    set_and_report(CUSTOM_CLUSTER_ID, ATTR_DATASET_HASH, &zigbee_context.dataset_hash);
}

/* LiPo charge in half percents (ZCL batteryPercentageRemaining, 200 = 100 %), interpolated in a rough open-circuit
 * voltage curve. */
uint8_t zigbee_battery_half_percent(uint16_t mv)
{
    static const struct {
        uint16_t mv;
        uint8_t  percent;
    } curve[] = {
        {3300, 0}, {3500, 5}, {3600, 12}, {3700, 30}, {3800, 50}, {4000, 80}, {4200, 100},
    };
    const int points = sizeof(curve) / sizeof(curve[0]);
    if (mv <= curve[0].mv) {
        return 0;
    }
    for (int i = 1; i < points; i++) {
        if (mv < curve[i].mv) {
            uint32_t half_percent = curve[i - 1].percent * 2 + (uint32_t)(curve[i].percent - curve[i - 1].percent) * 2 *
                                                               (mv - curve[i - 1].mv) / (curve[i].mv - curve[i - 1].mv);
            return (uint8_t)half_percent;
        }
    }
    return 200;
}

/* ---- Public API ---- */

void zigbee_report_wake_count(void)
{
    esp_zigbee_lock_acquire(portMAX_DELAY);
    set_and_report(CUSTOM_CLUSTER_ID, ATTR_WAKE_COUNT, &zigbee_context.wake_count);
    esp_zigbee_lock_release();
}

void zigbee_report_shown_view_id(void)
{
    esp_zigbee_lock_acquire(portMAX_DELAY);
    zigbee_report_shown_view_id_lock_held();
    esp_zigbee_lock_release();
}

void zigbee_report_dataset_hash(void)
{
    esp_zigbee_lock_acquire(portMAX_DELAY);
    zigbee_report_dataset_hash_lock_held();
    esp_zigbee_lock_release();
}

void zigbee_report_awake(bool awake)
{
    esp_zigbee_lock_acquire(portMAX_DELAY);
    set_and_report(CUSTOM_CLUSTER_ID, ATTR_AWAKE, &awake);
    esp_zigbee_lock_release();
}

void zigbee_report_occupancy(bool occupied)
{
    uint8_t value = occupied ? 1 : 0;
    ESP_LOGI(TAG, "report occupancy %u", value);
    esp_zigbee_lock_acquire(portMAX_DELAY);
    set_and_report(EZB_ZCL_CLUSTER_ID_OCCUPANCY_SENSING, ATTR_OCCUPANCY, &value);
    esp_zigbee_lock_release();
}

/* confirm: batteryAlarmState asks for a default response (only the low-battery farewell does) */
static void send_battery_reports(uint16_t mv, bool low, bool confirm)
{
    uint8_t  voltage_100mv = mv / 100, half_percent = zigbee_battery_half_percent(mv);
    uint32_t alarm_state = low ? BATTERY_ALARM_LOW : 0;
    esp_zigbee_lock_acquire(portMAX_DELAY);
    /* BatteryVoltage (0x0020) is set but not reported: the stack answers "not found" for it. A read gets it. */
    zigbee_set_attribute(EZB_ZCL_CLUSTER_ID_POWER_CONFIG, EZB_ZCL_ATTR_POWER_CONFIG_BATTERY_VOLTAGE_ID, &voltage_100mv);
    set_and_report(EZB_ZCL_CLUSTER_ID_POWER_CONFIG, EZB_ZCL_ATTR_POWER_CONFIG_BATTERY_PERCENTAGE_REMAINING_ID,
                   &half_percent);
    zigbee_set_attribute(EZB_ZCL_CLUSTER_ID_POWER_CONFIG, EZB_ZCL_ATTR_POWER_CONFIG_BATTERY_ALARM_STATE_ID, &alarm_state);
    zigbee_send_report(EZB_ZCL_CLUSTER_ID_POWER_CONFIG, EZB_ZCL_ATTR_POWER_CONFIG_BATTERY_ALARM_STATE_ID, confirm);
    set_and_report(CUSTOM_CLUSTER_ID, ATTR_BATTERY_MV, &mv);  /* the exact voltage: 0x0020 has 100 mV steps */
    esp_zigbee_lock_release();
}

void zigbee_report_battery(uint16_t mv, bool low)
{
    send_battery_reports(mv, low, false);
}

bool zigbee_report_battery_low(uint16_t mv, uint32_t timeout_ms)
{
    TickType_t start = xTaskGetTickCount();
    bool       confirmed = false;
    int        sends = 0;
    xEventGroupClearBits(zigbee_context.events, EVENT_BATTERY_LOW);
    while (!confirmed && elapsed_ms(start) < timeout_ms) {
        send_battery_reports(mv, true, true);
        sends++;
        confirmed = xEventGroupWaitBits(zigbee_context.events, EVENT_BATTERY_LOW, pdFALSE, pdTRUE,
                                        pdMS_TO_TICKS(GOODBYE_RETRY_MS)) & EVENT_BATTERY_LOW;
    }
    ESP_LOGI(TAG, "battery low report %s: %d send(s), %lu ms", confirmed ? "confirmed" : "timed out", sends,
             (unsigned long)elapsed_ms(start));
    return confirmed;
}

/* Resend whichever of the two goodbye reports Z2M has not confirmed yet. */
static void send_unconfirmed_goodbye_reports(EventBits_t confirmed)
{
    esp_zigbee_lock_acquire(portMAX_DELAY);
    if (!(confirmed & EVENT_GOODBYE_OCCUPANCY)) {
        zigbee_send_report(EZB_ZCL_CLUSTER_ID_OCCUPANCY_SENSING, ATTR_OCCUPANCY, true);
    }
    if (!(confirmed & EVENT_GOODBYE_AWAKE)) {
        zigbee_send_report(CUSTOM_CLUSTER_ID, ATTR_AWAKE, true);
    }
    esp_zigbee_lock_release();
}

bool zigbee_report_goodbye(uint32_t timeout_ms)
{
    const EventBits_t both = EVENT_GOODBYE_OCCUPANCY | EVENT_GOODBYE_AWAKE;
    uint8_t           occupancy = 0;
    bool              awake = false;
    TickType_t        start = xTaskGetTickCount();
    xEventGroupClearBits(zigbee_context.events, both);
    esp_zigbee_lock_acquire(portMAX_DELAY);
    zigbee_set_attribute(EZB_ZCL_CLUSTER_ID_OCCUPANCY_SENSING, ATTR_OCCUPANCY, &occupancy);
    zigbee_set_attribute(CUSTOM_CLUSTER_ID, ATTR_AWAKE, &awake);
    esp_zigbee_lock_release();
    EventBits_t confirmed = 0;
    int         sends = 0;
    while ((confirmed & both) != both && elapsed_ms(start) < timeout_ms) {
        send_unconfirmed_goodbye_reports(confirmed);
        sends++;
        confirmed = xEventGroupWaitBits(zigbee_context.events, both, pdFALSE, pdTRUE, pdMS_TO_TICKS(GOODBYE_RETRY_MS));
    }
    bool all_confirmed = (confirmed & both) == both;
    ESP_LOGI(TAG, "goodbye %s: %d send(s), %lu ms (occupancy %s, awake %s)", all_confirmed ? "confirmed" : "timed out",
             sends, (unsigned long)elapsed_ms(start),
             confirmed & EVENT_GOODBYE_OCCUPANCY ? "ok" : "lost", confirmed & EVENT_GOODBYE_AWAKE ? "ok" : "lost");
    return all_confirmed;
}
