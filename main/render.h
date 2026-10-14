#pragma once

#include <stdint.h>
#include "zb.h"

/* Debug screen (MVP): the device state, the snapshot on the panel and the last writes received from HA,
 * as plain ASCII text in a 5x7 font at scale 2 (33 columns). */

#define RENDER_RX_N 8  /* received writes shown, newest first */

typedef struct {
    uint32_t ms;  /* uptime when it arrived */
    zb_rx_t  ev;
} render_rx_t;

typedef struct {
    const char   *state;
    uint32_t      wake;       /* wake counter */
    uint32_t      uptime_ms;
    zb_snapshot_t snap;       /* the snapshot shown (hash 0 = none) */
    int           rx_count;
    render_rx_t   rx[RENDER_RX_N];  /* newest first */
} render_view_t;

/* Draw the view into the framebuffer and return it (EPD_STRIDE * EPD_HEIGHT bytes, 1 = white). */
const uint8_t *render_status(const render_view_t *v);
