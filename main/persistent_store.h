#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* State that survives the deep sleep (a wake is a reset): NVS, namespace "greenlight". The Zigbee stack keeps the
 * network in the same partition. The key strings address state stored on devices: never change them. */
#define STORE_KEY_PAIR_ON_BOOT      "pair_now"  /* u8: start pairing right after the next boot (ERASE_NETWORK) */
#define STORE_KEY_WAKE_COUNT        "wake_n"    /* u32: the wake counter (docs/zigbee.md#state-device-to-ha) */
#define STORE_KEY_DATASET           "ds"        /* blob: the dataset (docs/data-model.md#dataset) */
#define STORE_KEY_SHOWN_VIEW        "view"      /* blob: the view on the panel with its rows; its id is shown_view_id */
#define STORE_KEY_SIMULATED_MV      "sim_mv"    /* u16: the battery voltage override, 0 = measured (docs/firmware.md#battery-and-usb-power) */
#define STORE_KEY_SIMULATED_USB     "sim_usb"   /* u8: the USB power override: 0 sensed, 1 USB, 2 battery */
#define STORE_KEY_LOW_BATTERY       "bat_low"   /* u8: the LOW_BATTERY latch */
#define STORE_KEY_RETIRED_SNAPSHOT  "snap"      /* rev 5 snapshot, no longer used: erased at boot */

/* Open the namespace; an NVS partition that is full or from a newer format is erased first. */
void persistent_store_open(void);

uint8_t persistent_store_get_u8(const char *key);  /* 0 when missing */
void    persistent_store_set_u8(const char *key, uint8_t value);
bool    persistent_store_get_u16(const char *key, uint16_t *value);  /* false when missing */
void    persistent_store_set_u16(const char *key, uint16_t value);

/* Fills data with zeros when the blob is missing or has another size. */
void persistent_store_load_blob(const char *key, void *data, size_t size);
void persistent_store_save_blob(const char *key, const void *data, size_t size);
void persistent_store_erase(const char *key);

/* Count this wake and return its number. */
uint32_t persistent_store_next_wake_count(void);
