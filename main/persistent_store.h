#pragma once

#include <stddef.h>
#include <stdint.h>
#include "nvs_flash.h"

extern nvs_handle_t nvs;

uint8_t  nv_get(const char *key);
void     nv_set(const char *key, uint8_t v);
void     nvs_open_state(void);
uint32_t wake_count_next(void);
void     blob_load(const char *key, void *p, size_t size);
void     blob_save(const char *key, const void *p, size_t size);
