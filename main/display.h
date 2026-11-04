#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "render.h"
#include "state_machine.h"

extern volatile bool   view_dirty;
extern render_screen_t failed;

void show_state(state_t st);
void redraw(void);
void wait_ms(uint32_t ms);
