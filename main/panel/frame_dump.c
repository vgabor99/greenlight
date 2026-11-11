#ifdef SCREEN_DUMP

#include <stdbool.h>
#include <stdio.h>
#include "panel/epaper.h"
#include "panel/frame_dump.h"

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

#endif
