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

/* want_rsp asks the coordinator for a default response: only the goodbye reports do, so nothing else adds a downlink. */
void report_attr(uint16_t cluster, uint16_t attr, bool want_rsp)
{
    ezb_zcl_report_attr_cmd_t cmd = {
        .cmd_ctrl = {
            .dst_addr   = EZB_ADDRESS_SHORT(0x0000),
            .dst_ep     = COORD_EP,
            .src_ep     = ZB_EP,
            .cluster_id = cluster,
            .manuf_code = EZB_ZCL_STD_MANUF_CODE,
            .fc = {.manuf_specific = 0, .direction = EZB_ZCL_CMD_DIRECTION_TO_CLI, .dis_default_rsp = !want_rsp},
        },
        .payload = {.attr_id = attr},
    };
    ezb_err_t err = ezb_zcl_report_attr_cmd_req(&cmd);
    if (err != EZB_ERR_NONE) {
        ESP_LOGW(TAG, "report cluster 0x%04x attr 0x%04x failed: 0x%04x", cluster, attr, err);
    }
}

void set_value(uint16_t cluster, uint16_t attr, void *value)
{
    ezb_zcl_status_t st = ezb_zcl_set_attr_value(ZB_EP, cluster, EZB_ZCL_CLUSTER_SERVER, attr, EZB_ZCL_STD_MANUF_CODE,
                                                 value, false);
    if (st != EZB_ZCL_STATUS_SUCCESS) {
        ESP_LOGW(TAG, "set cluster 0x%04x attr 0x%04x failed: 0x%02x", cluster, attr, st);
    }
}

void set_and_report(uint16_t cluster, uint16_t attr, void *value)
{
    set_value(cluster, attr, value);
    report_attr(cluster, attr, false);
}

/* LiPo charge in half percents (ZCL batteryPercentageRemaining, 200 = 100 %), interpolated in a rough open-circuit
 * voltage curve. */
uint8_t battery_half_percent(uint16_t mv)
{
    static const struct { uint16_t mv; uint8_t pct; } curve[] = {
        {3300, 0}, {3500, 5}, {3600, 12}, {3700, 30}, {3800, 50}, {4000, 80}, {4200, 100},
    };
    const int n = sizeof(curve) / sizeof(curve[0]);
    if (mv <= curve[0].mv) {
        return 0;
    }
    for (int i = 1; i < n; i++) {
        if (mv < curve[i].mv) {
            uint32_t pct = curve[i - 1].pct * 2 + (uint32_t)(curve[i].pct - curve[i - 1].pct) * 2 *
                                                      (mv - curve[i - 1].mv) / (curve[i].mv - curve[i - 1].mv);
            return (uint8_t)pct;
        }
    }
    return 200;
}

void report_shown_view_id_nolock(void)
{
    set_and_report(CUSTOM_CLUSTER_ID, ATTR_SHOWN_VIEW_ID, &s_shown_view_id);
}

void report_dataset_hash_nolock(void)
{
    set_and_report(CUSTOM_CLUSTER_ID, ATTR_DATASET_HASH, &s_ds_hash);
}

void zb_report_battery(uint16_t mv, bool low)
{
    uint8_t  voltage = mv / 100, percent = battery_half_percent(mv);
    uint32_t alarm   = low ? BATTERY_ALARM_LOW : 0;
    esp_zigbee_lock_acquire(portMAX_DELAY);
    /* BatteryVoltage (0x0020) is set but not reported: the stack answers "not found" for it. A read gets it. */
    ezb_zcl_set_attr_value(ZB_EP, EZB_ZCL_CLUSTER_ID_POWER_CONFIG, EZB_ZCL_CLUSTER_SERVER,
                           EZB_ZCL_ATTR_POWER_CONFIG_BATTERY_VOLTAGE_ID, EZB_ZCL_STD_MANUF_CODE, &voltage, false);
    set_and_report(EZB_ZCL_CLUSTER_ID_POWER_CONFIG, EZB_ZCL_ATTR_POWER_CONFIG_BATTERY_PERCENTAGE_REMAINING_ID, &percent);
    set_and_report(EZB_ZCL_CLUSTER_ID_POWER_CONFIG, EZB_ZCL_ATTR_POWER_CONFIG_BATTERY_ALARM_STATE_ID, &alarm);
    set_and_report(CUSTOM_CLUSTER_ID, ATTR_BATTERY_MV, &mv);  /* the exact voltage: 0x0020 has 100 mV steps */
    esp_zigbee_lock_release();
}

void zb_report_occupancy(bool occupied)
{
    uint8_t v = occupied ? 1 : 0;
    ESP_LOGI(TAG, "report occupancy %u", v);
    esp_zigbee_lock_acquire(portMAX_DELAY);
    set_and_report(EZB_ZCL_CLUSTER_ID_OCCUPANCY_SENSING, 0x0000, &v);
    esp_zigbee_lock_release();
}

void zb_report_shown_view_id(void)
{
    esp_zigbee_lock_acquire(portMAX_DELAY);
    report_shown_view_id_nolock();
    esp_zigbee_lock_release();
}

void zb_report_dataset_hash(void)
{
    esp_zigbee_lock_acquire(portMAX_DELAY);
    report_dataset_hash_nolock();
    esp_zigbee_lock_release();
}

void zb_report_awake(bool awake)
{
    esp_zigbee_lock_acquire(portMAX_DELAY);
    set_and_report(CUSTOM_CLUSTER_ID, ATTR_AWAKE, &awake);
    esp_zigbee_lock_release();
}

bool zb_report_goodbye(uint32_t timeout_ms)
{
    uint8_t    occ = 0;
    bool       awake = false;
    TickType_t start = xTaskGetTickCount();
    xEventGroupClearBits(s_events, BIT_ACK_OCC | BIT_ACK_AWAKE);
    esp_zigbee_lock_acquire(portMAX_DELAY);
    set_value(EZB_ZCL_CLUSTER_ID_OCCUPANCY_SENSING, 0x0000, &occ);
    set_value(CUSTOM_CLUSTER_ID, ATTR_AWAKE, &awake);
    esp_zigbee_lock_release();
    const EventBits_t both = BIT_ACK_OCC | BIT_ACK_AWAKE;
    EventBits_t       got = 0;
    int               sends = 0;
    while ((got & both) != both && pdTICKS_TO_MS(xTaskGetTickCount() - start) < timeout_ms) {
        esp_zigbee_lock_acquire(portMAX_DELAY);  /* resend what is not confirmed yet */
        if (!(got & BIT_ACK_OCC)) {
            report_attr(EZB_ZCL_CLUSTER_ID_OCCUPANCY_SENSING, 0x0000, true);
        }
        if (!(got & BIT_ACK_AWAKE)) {
            report_attr(CUSTOM_CLUSTER_ID, ATTR_AWAKE, true);
        }
        esp_zigbee_lock_release();
        sends++;
        got = xEventGroupWaitBits(s_events, both, pdFALSE, pdTRUE, pdMS_TO_TICKS(GOODBYE_RETRY_MS));
    }
    bool ok = (got & both) == both;
    ESP_LOGI(TAG, "goodbye %s: %d send(s), %lu ms (occupancy %s, awake %s)", ok ? "confirmed" : "timed out", sends,
             (unsigned long)pdTICKS_TO_MS(xTaskGetTickCount() - start), got & BIT_ACK_OCC ? "ok" : "lost",
             got & BIT_ACK_AWAKE ? "ok" : "lost");
    return ok;
}

void zb_report_wake(void)
{
    esp_zigbee_lock_acquire(portMAX_DELAY);
    set_and_report(CUSTOM_CLUSTER_ID, ATTR_WAKE_COUNT, &s_wake_count);
    esp_zigbee_lock_release();
}
