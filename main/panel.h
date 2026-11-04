#pragma once

#include <stdint.h>
#include "esp_err.h"

extern int partials;

esp_err_t panel_clear(const char *why);
esp_err_t panel_refresh(const uint8_t *fb, const char *label);
