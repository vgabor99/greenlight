#pragma once

#include <stdint.h>

/* ---- State machine (DESIGN.md 3.3) ---- */
typedef enum {
    ST_BOOT, ST_UNPAIRED, ST_PAIRING, ST_INTERVIEW, ST_REJOIN, ST_ACTIVE, ST_ERASE_NETWORK,
    ST_SHUTDOWN, ST_SLEEP, ST_LOW_BATTERY,
} state_t;

extern const char *const state_names[];
extern uint32_t          wake_n;

void state_machine_run(void);
