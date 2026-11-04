#pragma once

#include <stdbool.h>
#include <stdint.h>

#define SCREEN_VALID_MS 8000   /* a received view is fresh this long: HA pushes every 5 s while awake, + 3 s latency (5.3) */

extern uint64_t screen_valid_until;

void    staleness_init(void);
bool    screen_stale(void);
int64_t screen_age_ms(void);
