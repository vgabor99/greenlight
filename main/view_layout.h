#pragma once

#include <stdint.h>
#include "zigbee.h"

#define RENDER_ROWS 6  /* line rows on the screen (DESIGN.md 5.2) */

/* The view on the panel, with its rows already laid out, so a new dataset never changes it (DESIGN.md 6.5). */
typedef struct {
    zigbee_view_t view;    /* view.id 0 = nothing shown */
    uint8_t   active;  /* active dataset lines (may be more than the rows) */
    uint8_t   nrows;
    char      row[RENDER_ROWS][ZIGBEE_LINE_MAX_BYTES + 1];
} render_shown_t;

/* Lay out a view: the active lines in dataset order; with more than RENDER_ROWS of them, the first
 * RENDER_ROWS - 1 and "további N" with the dots icon (DESIGN.md 5.2). */
void render_build_rows(render_shown_t *out, const zigbee_dataset_t *ds, const zigbee_view_t *view);
