#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Staleness (docs/data-model.md#staleness-flag): every received view is fresh for VIEW_VALID_MS. HA pushes the view every 5 s while the
 * device is awake, + 3 s latency. */
#define VIEW_VALID_MS 8000

/* Forget the deadline after a power-on (RTC memory and the RTC clock start over). Call once at boot. */
void view_freshness_init(void);

/* A view was received now. */
void view_freshness_renew(void);

bool view_freshness_is_stale(void);

/* Age of the last received view in ms, -1 if none is known. */
int64_t view_freshness_age_ms(void);
