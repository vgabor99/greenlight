#pragma once

#include <stdint.h>
#include "render.h"

/* The age as the debug screen shows it: "12s", "5m", "3h", "-" when unknown. */
#define RENDER_AGE_LEN 12
void render_age(char out[RENDER_AGE_LEN], int64_t age_ms);

void render_debug(const render_view_t *v);
