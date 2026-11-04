#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "zigbee.h"

/* The panel image. Two renderers, chosen at build time:
 * - the proper screen (screen.c, DESIGN.md 8): the house status (problem lines or "Minden OK") and the device
 *   screens (pairing, syncing, errors), in Inter with Material Design Icons;
 * - the debug screen for RnD (DESIGN.md 8.1): the device state, the dataset, the view on the panel and the last
 *   writes received from HA, as UTF-8 text in u8g2's Terminus Bold 16 (49 columns, Hungarian accents included).
 * RENDER_DEBUG_SCREEN 1 selects the debug screen: `idf.py -DDEBUG_SCREEN=1 build` (`-DDEBUG_SCREEN=0` switches
 * back; CMake remembers the last value). Both are always compiled. */
#ifndef RENDER_DEBUG_SCREEN
#define RENDER_DEBUG_SCREEN 0
#endif

#define RENDER_RX_N 8  /* received writes shown, newest first */
#define RENDER_ROWS 6  /* line rows on the screen (DESIGN.md 5.2) */

/* The view on the panel, with its rows already laid out, so a new dataset never changes it (DESIGN.md 6.5). */
typedef struct {
    zigbee_view_t view;    /* view.id 0 = nothing shown */
    uint8_t   active;  /* active dataset lines (may be more than the rows) */
    uint8_t   nrows;
    char      row[RENDER_ROWS][ZIGBEE_LINE_MAX_BYTES + 1];
} render_shown_t;

typedef struct {
    uint32_t ms;  /* uptime when it arrived */
    zigbee_received_write_t  ev;
} render_rx_t;

/* What the proper screen shows (chosen by the state machine; the debug screen ignores it). */
typedef enum {
    RENDER_BLANK,        /* white */
    RENDER_HOUSE,        /* the shown view: problem lines or "Minden OK" */
    RENDER_UNPAIRED,     /* "Párosítás" with how to pair: not paired */
    RENDER_SEARCHING,    /* "Hálózatkeresés": pairing */
    RENDER_JOINING,      /* "Csatlakozás": interview */
    RENDER_SYNCING,      /* "Szinkronizálás": no view yet, or a dataset sync under way */
    RENDER_JOIN_FAILED,  /* "Csatlakozási hiba": the rejoin failed */
    RENDER_BATTERY,      /* "Akkumulátor lemerült": to sleep with in LOW_BATTERY */
    RENDER_ERROR,        /* "Ismeretlen hiba" */
} render_screen_t;

typedef struct {
    render_screen_t screen;
    const char     *state;
    uint32_t        wake;       /* wake counter */
    uint32_t        uptime_ms;
    uint32_t        ds_hash;    /* stored dataset */
    uint8_t         ds_count;
    render_shown_t  shown;
    bool            oos;        /* the last view received was for another dataset */
    bool            stale;      /* the shown view is older than its validity (DESIGN.md 5.3) */
    int64_t         age_ms;     /* age of the last received view, -1 = unknown */
    uint32_t        valid_ms;   /* how long a received view stays fresh */
    uint16_t        battery_mv;
    bool            usb;        /* on USB power */
    bool            bat_low;    /* low battery (latched or below the threshold) */
    int             rx_count;
    render_rx_t     rx[RENDER_RX_N];  /* newest first */
} render_view_t;

/* Lay out a view: the active lines in dataset order; with more than RENDER_ROWS of them, the first
 * RENDER_ROWS - 1 and "további N" with the dots icon (DESIGN.md 5.2). */
void render_build_rows(render_shown_t *out, const zigbee_dataset_t *ds, const zigbee_view_t *view);

/* The age as the debug screen shows it: "12s", "5m", "3h", "-" when unknown. */
#define RENDER_AGE_LEN 12
void render_age(char out[RENDER_AGE_LEN], int64_t age_ms);

/* Draw the view into the framebuffer and return it (EPD_STRIDE * EPD_HEIGHT bytes, 1 = white). */
const uint8_t *render_status(const render_view_t *v);
