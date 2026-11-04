#ifdef SCREEN_DUMP

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "display/epaper.h"
#include "display/frame_dump.h"
#include "screen/render.h"

/* One line per pixel row that is not all white, each item a run of bytes (1 = white) that are not 0xFF. Compact,
 * because the console is slow and the Zigbee task logs through it too: a full hex dump took ~3 s and stalled the
 * sync. */
void frame_dump_print(const uint8_t *frame, const char *label)
{
    printf("FB begin %s\n", label);
    for (int y = 0; y < EPAPER_HEIGHT; y++) {
        const uint8_t *row = frame + y * EPAPER_ROW_BYTES;
        bool           row_started = false;
        for (int i = 0; i < EPAPER_ROW_BYTES; i++) {
            if (row[i] == 0xFF) {
                continue;
            }
            if (!row_started) {
                printf("FB %d", y);
                row_started = true;
            }
            printf(" %d:", i);
            for (; i < EPAPER_ROW_BYTES && row[i] != 0xFF; i++) {
                printf("%02X", row[i]);
            }
        }
        if (row_started) {
            printf("\n");
        }
    }
    printf("FB end\n");
}

/* The content of the UI spec's mockups and a few edge cases: to compare with the mockups without having to provoke
 * each state. */
void frame_dump_samples(void)
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
    static render_content_t content;
    vTaskDelay(pdMS_TO_TICKS(2000));  /* the USB console drops output until the host has reopened the port */
    for (int sample = 0; sample < sizeof(sample_rows) / sizeof(sample_rows[0]); sample++) {
        memset(&content, 0, sizeof(content));
        content.screen = RENDER_HOUSE;
        content.shown.view = (zigbee_view_t){.id = 1, .time = 1793472360};  /* Sat 2026-10-31 18:46 */
        for (int i = 0; i < VIEW_LAYOUT_ROWS && sample_rows[sample][i]; i++) {
            strlcpy(content.shown.rows[content.shown.row_count++], sample_rows[sample][i],
                    sizeof(content.shown.rows[0]));
        }
        content.shown.active_lines = sample == 1 ? 9 : content.shown.row_count;
        content.stale = sample >= 5;  /* the sync badge */
        frame_dump_print(render_frame(&content), sample_names[sample]);
    }
    static const struct {
        render_screen_t screen;
        const char     *name;
    } device_screens[] = {
        {RENDER_UNPAIRED, "unpaired"},       {RENDER_SEARCHING, "searching"}, {RENDER_JOINING, "joining"},
        {RENDER_SYNCING, "syncing"},         {RENDER_JOIN_FAILED, "join_failed"},
        {RENDER_BATTERY, "battery"},         {RENDER_ERROR, "error"},
    };
    for (int i = 0; i < sizeof(device_screens) / sizeof(device_screens[0]); i++) {
        memset(&content, 0, sizeof(content));
        content.screen = device_screens[i].screen;
        frame_dump_print(render_frame(&content), device_screens[i].name);
    }
}

#endif
