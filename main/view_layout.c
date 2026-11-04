#include <stdio.h>
#include <string.h>
#include "view_layout.h"

void render_build_rows(render_shown_t *out, const zigbee_dataset_t *ds, const zigbee_view_t *view)
{
    memset(out, 0, sizeof(*out));
    out->view = *view;
    for (int i = 0; i < ds->count; i++) {
        if (view->bits & (1UL << i)) {
            out->active++;
        }
    }
    int shown = out->active > RENDER_ROWS ? RENDER_ROWS - 1 : out->active;
    for (int i = 0; i < ds->count && out->nrows < shown; i++) {
        if (view->bits & (1UL << i)) {
            memcpy(out->row[out->nrows++], ds->text[i], sizeof(out->row[0]));
        }
    }
    if (out->active > shown) {
        snprintf(out->row[out->nrows++], sizeof(out->row[0]), "01|további %u|", out->active - shown);
    }
}
