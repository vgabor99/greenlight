#pragma once

#include <stdbool.h>
#include "app/app_state.h"

/* What should be on the screen now (docs/display.md), decided from the application state alone. It says what to
 * show, not how: no sizes, rows, texts or icons, so any UI on any panel can draw it. */

typedef enum {
    SCREEN_BLANK,        /* nothing (white) */
    SCREEN_HOUSE,        /* the shown view: its lines, or All OK when it has none */
    SCREEN_UNPAIRED,     /* not paired: how to pair */
    SCREEN_SEARCHING,    /* pairing */
    SCREEN_JOINING,      /* joined, the Z2M interview */
    SCREEN_SYNCING,      /* no view yet, or a dataset sync under way */
    SCREEN_JOIN_FAILED,  /* the rejoin failed */
    SCREEN_BATTERY_DEAD, /* to sleep with in LOW_BATTERY */
    SCREEN_ERROR,        /* unknown error */
} screen_kind_t;

typedef struct {
    screen_kind_t       kind;
    bool                stale;  /* the shown view is out of date (docs/data-model.md#staleness-flag) */
    const shown_view_t *shown;  /* SCREEN_HOUSE: points into the state it was made from */
} screen_content_t;

/* The content for the state. */
void screen_content_of(const app_state_t *state, screen_content_t *content);

/* Two contents look the same on any screen. */
bool screen_content_equal(const screen_content_t *a, const screen_content_t *b);
