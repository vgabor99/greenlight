#pragma once

#include <stdint.h>
#include "data_model.h"
#include "zigbee/zigbee.h"

/* HA's input (docs/zigbee.md#input-ha-to-device): the views and datasets the transport decodes, checked against the
 * application state by the data model's rules, applied to it and acknowledged. HA working with the device keeps it
 * awake. These are the device's zigbee_callbacks_t; they run in the Zigbee task. */

void ha_input_on_view(const view_t *view, uint8_t received_mask);
void ha_input_on_dataset_entry(const zigbee_dataset_entry_t *entry);
void ha_input_on_dataset_commit(const zigbee_dataset_commit_t *commit);
