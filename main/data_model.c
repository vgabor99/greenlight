#include <string.h>
#include "data_model.h"
#include "util/hex.h"

view_result_t data_model_check_view(const view_t *view, uint32_t dataset_hash, uint32_t shown_view_id)
{
    if (view->id == 0) {
        return VIEW_INCOMPLETE;
    }
    if (view->dataset_hash == 0 || view->dataset_hash != dataset_hash) {
        return VIEW_OUT_OF_SYNC;
    }
    return view->id == shown_view_id ? VIEW_TIME_ONLY : VIEW_NEW;
}

bool data_model_line_active(const view_t *view, int line)
{
    return view->bits & (1UL << line);
}

bool data_model_parse_line(const char *text, line_t *line)
{
    int         high = hex_digit_value(text[0]), low = high < 0 ? -1 : hex_digit_value(text[1]);
    const char *name_end = low < 0 || text[2] != '|' ? NULL : strchr(text + 3, '|');
    if (!name_end || name_end == text + 3) {
        return false;
    }
    line->icon_id     = high * 16 + low;
    line->name        = text + 3;
    line->name_length = name_end - line->name;
    line->state       = name_end + 1;
    return true;
}
