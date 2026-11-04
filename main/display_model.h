#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "render.h"
#include "zigbee.h"

/* What the panel is about: the stored dataset, the view on the panel and the writes HA sent in this wake. The
 * Zigbee task changes it through the callbacks below, the state machine draws a snapshot of it; a mutex guards it.
 * The dataset and the shown view are kept in NVS through the sleep. */

void display_model_init(void);  /* before any other call */
void display_model_load(void);  /* the stored dataset and view */
void display_model_erase_stored(void);

uint32_t display_model_dataset_hash(void);    /* 0 = no dataset */
uint32_t display_model_shown_view_id(void);   /* 0 = nothing shown */
bool     display_model_view_received(void);  /* a view was applied in this wake */

/* Something on the screen may have changed: the next display_wait_ms() redraws. */
void display_model_mark_changed(void);
bool display_model_has_changed(void);

/* Fill the model's part of the render content (dataset, shown view, staleness, received writes) and clear the
 * changed flag. *syncing: dataset entries arrived and their commit has not. */
void display_model_snapshot(render_view_t *content, bool *syncing);

/* Zigbee callbacks (zb_callbacks_t), called in the Zigbee task */
void display_model_on_view(const zb_view_t *view, zb_view_result_t result);
void display_model_on_dataset(const zb_dataset_t *dataset);
void display_model_on_received_write(const zb_rx_t *write);
