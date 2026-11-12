#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Holds that keep the wake going besides the inputs (docs/behaviour.md#timeouts): each one names how long it needs
 * from now; the wake may end once the latest of them has passed. Not kept through sleep: the deadline starts out
 * expired at every boot. */

/* Keep the device awake at least `ms` from now (a later deadline already set stays). */
void keep_awake_for(uint32_t ms);

/* No hold is left: the deadline has passed. */
bool keep_awake_expired(void);
