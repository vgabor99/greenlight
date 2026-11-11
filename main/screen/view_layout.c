#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "screen/view_layout.h"

void view_layout_build(shown_view_t *out, const dataset_t *dataset, const view_t *view)
{
    memset(out, 0, sizeof(*out));
    out->view = *view;
    for (int line = 0; line < dataset->count; line++) {
        if (data_model_line_active(view, line)) {
            out->active_lines++;
        }
    }
    int lines_shown = out->active_lines > VIEW_LAYOUT_ROWS ? VIEW_LAYOUT_ROWS - 1 : out->active_lines;
    for (int line = 0; line < dataset->count && out->row_count < lines_shown; line++) {
        if (data_model_line_active(view, line)) {
            memcpy(out->rows[out->row_count++], dataset->text[line], sizeof(out->rows[0]));
        }
    }
    if (out->active_lines > lines_shown) {
        snprintf(out->rows[out->row_count++], sizeof(out->rows[0]), "01|további %u|", out->active_lines - lines_shown);
    }
}
