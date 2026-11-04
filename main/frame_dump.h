#pragma once

#include <stdint.h>

#ifdef SCREEN_DUMP
void dump_frame(const uint8_t *fb, const char *label);
void dump_samples(void);
#endif
