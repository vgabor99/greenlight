#pragma once

#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "render.h"
#include "zb.h"

extern SemaphoreHandle_t view_lock;
extern zb_dataset_t      ds;
extern render_shown_t    shown;
extern bool              oos;
extern volatile bool     view_seen;
extern bool              syncing;
extern render_rx_t       rx_log[RENDER_RX_N];
extern int               rx_head, rx_count;

void state_load(void);
void on_view(const zb_view_t *v, zb_view_result_t result);
void on_dataset(const zb_dataset_t *d);
void on_rx(const zb_rx_t *ev);
