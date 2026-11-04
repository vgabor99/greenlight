#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "epd.h"
#include "frame_dump.h"
#include "render.h"

#ifdef SCREEN_DUMP
/* The frame on the console for tools/fb_dump.py, which makes PNGs of it: one line per pixel row that is not all
 * white, "FB <row> <byte>:<hex> ...", each item a run of bytes (1 = white) that are not 0xFF. Compact, because
 * the console is slow and the Zigbee task logs through it too: a full hex dump took ~3 s and stalled the sync. */
void dump_frame(const uint8_t *fb, const char *label)
{
    printf("FB begin %s\n", label);
    for (int y = 0; y < EPD_HEIGHT; y++) {
        const uint8_t *row = fb + y * EPD_STRIDE;
        bool           any = false;
        for (int i = 0; i < EPD_STRIDE; i++) {
            if (row[i] == 0xFF) {
                continue;
            }
            if (!any) {
                printf("FB %d", y);
                any = true;
            }
            printf(" %d:", i);
            for (; i < EPD_STRIDE && row[i] != 0xFF; i++) {
                printf("%02X", row[i]);
            }
        }
        if (any) {
            printf("\n");
        }
    }
    printf("FB end\n");
}

/* Every screen once, with the content of the UI spec's mockups and a few edge cases, dumped but not drawn:
 * to compare with the mockups without having to provoke each state. */
void dump_samples(void)
{
    static const char *const rows[][RENDER_ROWS] = {
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
    static const char *const names[] = {"problems", "overflow", "two", "edge", "ok", "problems_stale", "ok_stale"};
    static render_view_t v;
    vTaskDelay(pdMS_TO_TICKS(2000));  /* the USB console drops output until the host has reopened the port */
    for (int s = 0; s < sizeof(rows) / sizeof(rows[0]); s++) {
        memset(&v, 0, sizeof(v));
        v.screen = RENDER_HOUSE;
        v.shown.view = (zb_view_t){.id = 1, .time = 1793472360};  /* Sat 2026-10-31 18:46 */
        for (int i = 0; i < RENDER_ROWS && rows[s][i]; i++) {
            strlcpy(v.shown.row[v.shown.nrows++], rows[s][i], sizeof(v.shown.row[0]));
        }
        v.shown.active = s == 1 ? 9 : v.shown.nrows;
        v.stale = s >= 5;  /* the sync badge */
        dump_frame(render_status(&v), names[s]);
    }
    static const struct { render_screen_t screen; const char *name; } dev[] = {
        {RENDER_UNPAIRED, "unpaired"}, {RENDER_SEARCHING, "searching"}, {RENDER_JOINING, "joining"},
        {RENDER_SYNCING, "syncing"}, {RENDER_JOIN_FAILED, "join_failed"}, {RENDER_BATTERY, "battery"}, {RENDER_ERROR, "error"},
    };
    for (int s = 0; s < sizeof(dev) / sizeof(dev[0]); s++) {
        memset(&v, 0, sizeof(v));
        v.screen = dev[s].screen;
        dump_frame(render_status(&v), dev[s].name);
    }
}
#endif
