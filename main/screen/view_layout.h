#pragma once

#include <stdint.h>
#include "zigbee/zigbee.h"

#define VIEW_LAYOUT_ROWS 6  /* line rows on the screen (DESIGN.md 5.2) */

/* The view on the panel, with its rows already laid out, so a new dataset never changes it (DESIGN.md 6.5).
 * Stored in NVS as is: keep the layout. */
typedef struct {
    zigbee_view_t view;          /* view.id 0 = nothing shown */
    uint8_t       active_lines;  /* active dataset lines (may be more than the rows) */
    uint8_t       row_count;
    char          rows[VIEW_LAYOUT_ROWS][ZIGBEE_LINE_MAX_BYTES + 1];  /* dataset lines, "II|name|state" */
} shown_view_t;

/* Lay out a view: the active lines in dataset order; with more than VIEW_LAYOUT_ROWS of them, the first
 * VIEW_LAYOUT_ROWS - 1 and "további N" with the dots icon (DESIGN.md 5.2). */
void view_layout_build(shown_view_t *out, const zigbee_dataset_t *dataset, const zigbee_view_t *view);
