#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "screen/view_layout.h"
#include "zigbee/zigbee.h"

/* The panel image. Two renderers, chosen at build time:
 * - the proper screen (status_screen.c, DESIGN.md 8): the house status (problem lines or "Minden OK") and the
 *   device screens (pairing, syncing, errors), in Inter with Material Design Icons;
 * - the debug screen for RnD (debug_screen.c, DESIGN.md 8.1): the device state, the dataset, the view on the panel
 *   and the last writes received from HA, as UTF-8 text in u8g2's Terminus Bold 16 (49 columns, Hungarian
 *   accents included).
 * RENDER_DEBUG_SCREEN 1 selects the debug screen: `idf.py -DDEBUG_SCREEN=1 build` (`-DDEBUG_SCREEN=0` switches
 * back; CMake remembers the last value). Both are always compiled. */
#ifndef RENDER_DEBUG_SCREEN
#define RENDER_DEBUG_SCREEN 0
#endif

#define RENDER_RECEIVED_WRITES 8  /* received writes shown, newest first */

typedef struct {
    uint32_t                uptime_ms;  /* when it arrived */
    zigbee_received_write_t write;
} render_received_write_t;

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

/* Everything either screen draws. */
typedef struct {
    render_screen_t         screen;
    const char             *state_name;
    uint32_t                wake_number;
    uint32_t                uptime_ms;
    uint32_t                dataset_hash;   /* stored dataset */
    uint8_t                 dataset_lines;
    shown_view_t            shown;
    bool                    out_of_sync;    /* the last view received was for another dataset */
    bool                    stale;          /* the shown view is older than its validity (DESIGN.md 5.3) */
    int64_t                 age_ms;         /* age of the last received view, -1 = unknown */
    uint32_t                valid_ms;       /* how long a received view stays fresh */
    uint16_t                battery_mv;
    bool                    on_usb_power;
    bool                    battery_low;    /* latched or below the threshold */
    int                     received_write_count;
    render_received_write_t received_writes[RENDER_RECEIVED_WRITES];  /* newest first */
} render_content_t;

/* Draw the content into the framebuffer and return it (FRAMEBUFFER_SIZE bytes, 1 = white). */
const uint8_t *render_frame(const render_content_t *content);
