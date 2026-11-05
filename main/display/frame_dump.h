#pragma once

#include <stdint.h>

/* SCREEN_DUMP builds (`idf.py -DSCREEN_DUMP=1 build`) print frames on the console for tools/fb_dump.py. */
#ifdef SCREEN_DUMP

/* The frame as "FB begin <label>", "FB <row> <byte>:<hex> ..." lines and "FB end" (docs/display.md#checking-against-the-mockups). */
void frame_dump_print(const uint8_t *frame, const char *label);

/* Every screen once with sample content, dumped but not drawn. */
void frame_dump_samples(void);

#endif
