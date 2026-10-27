#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "zb.h"

/* Debug screen (MVP): the device state, the dataset, the view on the panel and the last writes received from
 * HA, as UTF-8 text in u8g2's Terminus Bold 16 font with Latin Extended-A (49 columns, Hungarian accents included). */

#define RENDER_RX_N 8  /* received writes shown, newest first */
#define RENDER_ROWS 6  /* line rows on the screen (DESIGN.md 5.2) */

/* The view on the panel, with its rows already laid out, so a new dataset never changes it (DESIGN.md 6.5). */
typedef struct {
    zb_view_t view;    /* view.id 0 = nothing shown */
    uint8_t   active;  /* active dataset lines (may be more than the rows) */
    uint8_t   nrows;
    char      row[RENDER_ROWS][ZB_DS_CHARS + 1];
} render_shown_t;

typedef struct {
    uint32_t ms;  /* uptime when it arrived */
    zb_rx_t  ev;
} render_rx_t;

typedef struct {
    const char    *state;
    uint32_t       wake;       /* wake counter */
    uint32_t       uptime_ms;
    uint32_t       ds_hash;    /* stored dataset */
    uint8_t        ds_count;
    render_shown_t shown;
    bool           oos;        /* the last view received was for another dataset */
    int            rx_count;
    render_rx_t    rx[RENDER_RX_N];  /* newest first */
} render_view_t;

/* Lay out a view: the active lines in dataset order; with more than RENDER_ROWS of them, the first
 * RENDER_ROWS - 1 and "+N more" (DESIGN.md 5.2). */
void render_build_rows(render_shown_t *out, const zb_dataset_t *ds, const zb_view_t *view);

/* Draw the view into the framebuffer and return it (EPD_STRIDE * EPD_HEIGHT bytes, 1 = white). */
const uint8_t *render_status(const render_view_t *v);
