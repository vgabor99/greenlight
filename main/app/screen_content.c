#include <string.h>
#include "app/screen_content.h"

static screen_kind_t failure_screen(wake_failure_t failure)
{
    switch (failure) {
    case WAKE_FAILURE_UNPAIRED:    return SCREEN_UNPAIRED;
    case WAKE_FAILURE_JOIN_FAILED: return SCREEN_JOIN_FAILED;
    case WAKE_FAILURE_LOW_BATTERY: return SCREEN_BATTERY_DEAD;
    case WAKE_FAILURE_ERROR:
    default:                       return SCREEN_ERROR;
    }
}

/* The house status, unless the device is pairing, joining or syncing, or this wake failed. */
static screen_kind_t choose_kind(const app_state_t *state)
{
    if (state->failure != WAKE_FAILURE_NONE) {
        return failure_screen(state->failure);
    }
    bool view_shown = state->shown.view.id != 0;
    switch (state->state) {
    case STATE_UNPAIRED:      return SCREEN_UNPAIRED;
    case STATE_PAIRING:       return SCREEN_SEARCHING;
    case STATE_ERASE_NETWORK: return SCREEN_SEARCHING;  /* pairing follows the restart */
    case STATE_INTERVIEW:     return SCREEN_JOINING;
    case STATE_BOOT:          return view_shown ? SCREEN_HOUSE : SCREEN_BLANK;
    case STATE_SLEEP:         return view_shown ? SCREEN_HOUSE : SCREEN_SYNCING;  /* a stale view beats none */
    default:                  return view_shown && !state->syncing && !state->out_of_sync ? SCREEN_HOUSE : SCREEN_SYNCING;
    }
}

void screen_content_of(const app_state_t *state, screen_content_t *content)
{
    content->kind  = choose_kind(state);
    content->stale = state->stale;
    content->shown = content->kind == SCREEN_HOUSE ? &state->shown : NULL;
}

bool screen_content_equal(const screen_content_t *a, const screen_content_t *b)
{
    if (a->kind != b->kind || a->stale != b->stale) {
        return false;
    }
    if (a->kind != SCREEN_HOUSE) {
        return true;
    }
    if (a->shown->line_count != b->shown->line_count) {
        return false;
    }
    for (int i = 0; i < a->shown->line_count; i++) {
        if (strcmp(a->shown->lines[i], b->shown->lines[i]) != 0) {
            return false;
        }
    }
    return true;
}
