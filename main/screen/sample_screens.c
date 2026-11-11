#ifdef SCREEN_DUMP

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "panel/frame_dump.h"
#include "screen/render.h"
#include "screen/sample_screens.h"

/* The content of the UI spec's mockups and a few edge cases: to compare with the mockups without having to provoke
 * each state. */
void sample_screens_dump(void)
{
    static const char *const sample_rows[][VIEW_LAYOUT_ROWS] = {
        {"02|Teraszajtó|nyitva", "04|Háló ablak|nyitva", "0D|Nappali lámpa|be", "2E|Hűtő|túl meleg",
         "2F|Fagyasztó|túl meleg", "27|Mosogató|nedves"},
        {"02|Teraszajtó|nyitva", "04|Háló ablak|nyitva", "0D|Nappali lámpa|be", "2E|Hűtő|túl meleg",
         "2F|Fagyasztó|túl meleg", "01|további 4|"},
        {"02|Teraszajtó|nyitva", "04|Háló ablak|nyitva"},
        {"Teraszajtó nyitva", "7F|Ismeretlen ikon|nyitva", "02|Nagyon hosszú nevű ajtó a pincelépcső alatt|nyitva",
         "0D|Lámpa|nagyon hosszú állapot szöveg, ami nem fér el", "13|Árvíztűrő tükörfúrógép|"},
        {0},  /* Minden OK */
        {"02|Teraszajtó|nyitva", "04|Háló ablak|nyitva", "0D|Nappali lámpa|be", "2E|Hűtő|túl meleg",
         "2F|Fagyasztó|túl meleg", "27|Mosogató|nedves"},
        {0},
    };
    static const char *const sample_names[] = {"problems", "overflow", "two", "edge", "ok", "problems_stale", "ok_stale"};
    static app_state_t state;
    vTaskDelay(pdMS_TO_TICKS(2000));  /* the USB console drops output until the host has reopened the port */
    for (int sample = 0; sample < sizeof(sample_rows) / sizeof(sample_rows[0]); sample++) {
        memset(&state, 0, sizeof(state));
        state.state      = STATE_ACTIVE;
        state.shown.view = (view_t){.id = 1, .time = 1793472360};  /* Sat 2026-10-31 18:46 */
        for (int i = 0; i < VIEW_LAYOUT_ROWS && sample_rows[sample][i]; i++) {
            strlcpy(state.shown.rows[state.shown.row_count++], sample_rows[sample][i], sizeof(state.shown.rows[0]));
        }
        state.shown.active_lines = sample == 1 ? 9 : state.shown.row_count;
        state.stale = sample >= 5;  /* the sync badge */
        frame_dump_print(render_frame(&state), sample_names[sample]);
    }
    /* The device screens, each from a state that leads to it (app/screen_content.c) */
    static const struct {
        device_state_t state;
        wake_failure_t failure;
        const char    *name;
    } device_screens[] = {
        {STATE_UNPAIRED, WAKE_FAILURE_NONE, "unpaired"},
        {STATE_PAIRING, WAKE_FAILURE_NONE, "searching"},
        {STATE_INTERVIEW, WAKE_FAILURE_NONE, "joining"},
        {STATE_ACTIVE, WAKE_FAILURE_NONE, "syncing"},
        {STATE_SLEEP, WAKE_FAILURE_JOIN_FAILED, "join_failed"},
        {STATE_LOW_BATTERY, WAKE_FAILURE_LOW_BATTERY, "battery"},
        {STATE_SLEEP, WAKE_FAILURE_ERROR, "error"},
    };
    for (int i = 0; i < sizeof(device_screens) / sizeof(device_screens[0]); i++) {
        memset(&state, 0, sizeof(state));
        state.state   = device_screens[i].state;
        state.failure = device_screens[i].failure;
        frame_dump_print(render_frame(&state), device_screens[i].name);
    }
}

#endif
