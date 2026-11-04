#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_err.h"
#include "esp_zigbee.h"
#include "zigbee.h"

#define ZB_EP        1
#define COORD_EP     1
#define CUSTOM_CLUSTER_ID 0xFC8A

/* Custom cluster attribute ids (DESIGN.md 6.4, 6.5); 0x0000-0x0007 are retired, 0x0010-0x0014 reserved */
#define ATTR_VIEW_BITS     0x0020  /* .. ATTR_VIEW_ID: the four view_* attributes, in zb_view_t order */
#define ATTR_VIEW_ID       0x0023
#define ATTR_DS_ENTRY      0x0030
#define ATTR_DS_COMMIT     0x0031
#define ATTR_BATTERY_MV    0x0013  /* exact battery voltage in mV (DESIGN.md 6.6), reported, read-only */
#define ATTR_WAKE_COUNT    0x0040  /* reported state, read-only */
#define ATTR_SHOWN_VIEW_ID 0x0041
#define ATTR_DATASET_HASH  0x0042
#define ATTR_AWAKE         0x0043

#define VIEW_ALL  0x0F                         /* all four view_* attributes received */
#define ENTRY_MAX (6 + 1 + 2 + 1 + ZB_PART_CHARS)  /* "HHHHHH|II|text": 36 characters */

/* Battery (DESIGN.md 6.6): batteryAlarmState bit 0 (BatteryMinThreshold) = low, Z2M's battery_low */
#define BATTERY_ALARM_LOW 0x00000001u

/* End device timeout requested from the parent. The longest value (16384 min) made the join fail with a TCLK
 * exchange error on Z2M, so use the value of the Espressif examples; revisit when the device sleeps for hours. */
#define ZB_ALL_CHANNELS_MASK 0x07FFF800U  /* channels 11-26 */
#define ED_TIMEOUT EZB_NWK_ED_TIMEOUT_64MIN
#define POLL_MS    50    /* how often the awake device polls its parent for queued messages (ZIGBEE_PAYLOAD.md 6) */
#define GOODBYE_RETRY_MS 1000  /* zb_report_goodbye(): resend an unconfirmed report this often */

#define BIT_STARTED     BIT0
#define BIT_FACTORY_NEW BIT1
#define BIT_JOINED      BIT2
#define BIT_LEFT        BIT3
#define BIT_ACK_OCC     BIT4  /* Z2M confirmed the goodbye's occupancy report */
#define BIT_ACK_AWAKE   BIT5  /* ... and its awake report */

extern EventGroupHandle_t s_events;
extern zb_callbacks_t     s_cb;
extern uint32_t           s_shown_view_id;
extern uint32_t           s_ds_hash;
extern uint32_t           s_wake_count;
extern uint16_t           s_battery_mv;

/* zigbee_reports.c */
void    report_attr(uint16_t cluster, uint16_t attr, bool want_rsp);
void    set_value(uint16_t cluster, uint16_t attr, void *value);
void    set_and_report(uint16_t cluster, uint16_t attr, void *value);
uint8_t battery_half_percent(uint16_t mv);
void    report_shown_view_id_nolock(void);
void    report_dataset_hash_nolock(void);

/* zigbee_display_input.c */
void stage_clear(void);
void custom_cluster_init(uint8_t ep_id);
void custom_cluster_deinit(uint8_t ep_id);

/* zigbee.c */
void core_action_handler(ezb_zcl_core_action_callback_id_t callback_id, void *message);

/* zigbee_device.c */
esp_err_t create_device(void);
