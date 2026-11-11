#pragma once

/* Shared by the zigbee*.c files only: the data model's ids and the state the parts share. Functions marked
 * "lock held" must be called with the Zigbee lock held (esp_zigbee_lock_acquire()), as in the Zigbee task. */

#include <stdbool.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_err.h"
#include "esp_zigbee.h"
#include "zigbee/zigbee.h"

#define DEVICE_ENDPOINT      1
#define COORDINATOR_ENDPOINT 1
#define CUSTOM_CLUSTER_ID    0xFC8A

/* Custom cluster attribute ids (docs/zigbee.md#state-device-to-ha, #input-ha-to-device); 0x0000-0x0007 are retired, 0x0010-0x0012 and 0x0014 reserved */
#define ATTR_VIEW_BITS       0x0020  /* .. ATTR_VIEW_ID: the four view_* attributes, in view_t order */
#define ATTR_VIEW_ID         0x0023
#define ATTR_DATASET_ENTRY   0x0030
#define ATTR_DATASET_COMMIT  0x0031
#define ATTR_BATTERY_MV      0x0013  /* exact battery voltage in mV (docs/zigbee.md#battery), reported, read-only */
#define ATTR_WAKE_COUNT      0x0040  /* reported state, read-only */
#define ATTR_SHOWN_VIEW_ID   0x0041
#define ATTR_DATASET_HASH    0x0042
#define ATTR_AWAKE           0x0043
#define ATTR_OCCUPANCY       0x0000  /* Occupancy Sensing: occupancy */

/* "HHHHHH|II|text": the longest dataset entry, 36 characters */
#define ENTRY_MAX_CHARS (6 + 1 + 2 + 1 + ZIGBEE_PART_MAX_BYTES)

/* Event group bits */
#define EVENT_STARTED            BIT0
#define EVENT_FACTORY_NEW        BIT1
#define EVENT_JOINED             BIT2
#define EVENT_LEFT               BIT3
#define EVENT_GOODBYE_OCCUPANCY  BIT4  /* Z2M confirmed the goodbye's occupancy report */
#define EVENT_GOODBYE_AWAKE      BIT5  /* ... and its awake report */
#define EVENT_BATTERY_LOW        BIT6  /* Z2M confirmed the low-battery alarm report */

typedef struct {
    EventGroupHandle_t events;
    zigbee_callbacks_t callbacks;
    /* The values to report; battery_mv is also the stack's storage for its attribute (zigbee_device.c) */
    uint32_t           wake_count;
    uint32_t           shown_view_id;
    uint32_t           dataset_hash;
    uint16_t           battery_mv;
} zigbee_context_t;

extern zigbee_context_t zigbee_context;

/* zigbee_device.c: the endpoint and its clusters */
esp_err_t zigbee_device_register(void);

/* zigbee_display_input.c: the custom cluster's writable attributes */
void zigbee_display_input_init(void);
void zigbee_display_input_register_handlers(uint8_t endpoint);

/* zigbee_reports.c */
void    zigbee_set_attribute(uint16_t cluster, uint16_t attribute, void *value);  /* lock held */
void    zigbee_send_report(uint16_t cluster, uint16_t attribute, bool ask_for_response);  /* lock held */
void    zigbee_report_shown_view_id_lock_held(void);
void    zigbee_report_dataset_hash_lock_held(void);
uint8_t zigbee_battery_half_percent(uint16_t mv);  /* ZCL batteryPercentageRemaining */
void    zigbee_reports_on_default_response(const ezb_zcl_cmd_default_rsp_message_t *response);  /* a report confirmed */

/* zigbee.c: the stack's ZCL core actions (attribute writes; default responses go to zigbee_reports.c) */
void zigbee_handle_core_action(ezb_zcl_core_action_callback_id_t callback_id, void *message);
