#include <string.h>
#include "nvs_flash.h"
#include "persistent_store.h"

static nvs_handle_t store;

void persistent_store_open(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    ESP_ERROR_CHECK(nvs_open("greenlight", NVS_READWRITE, &store));
}

uint8_t persistent_store_get_u8(const char *key)
{
    uint8_t value = 0;
    nvs_get_u8(store, key, &value);
    return value;
}

void persistent_store_set_u8(const char *key, uint8_t value)
{
    ESP_ERROR_CHECK(nvs_set_u8(store, key, value));
    ESP_ERROR_CHECK(nvs_commit(store));
}

bool persistent_store_get_u16(const char *key, uint16_t *value)
{
    return nvs_get_u16(store, key, value) == ESP_OK;
}

void persistent_store_set_u16(const char *key, uint16_t value)
{
    nvs_set_u16(store, key, value);
    nvs_commit(store);
}

void persistent_store_load_blob(const char *key, void *data, size_t size)
{
    size_t stored_size = size;
    if (nvs_get_blob(store, key, data, &stored_size) != ESP_OK || stored_size != size) {
        memset(data, 0, size);
    }
}

void persistent_store_save_blob(const char *key, const void *data, size_t size)
{
    nvs_set_blob(store, key, data, size);
    nvs_commit(store);
}

void persistent_store_erase(const char *key)
{
    if (nvs_erase_key(store, key) == ESP_OK) {  /* a missing key needs no commit */
        nvs_commit(store);
    }
}

/* Every boot is a wake (a deep-sleep wake is a reset), so count boots. 24 bits, like the ids and hashes
 * (docs/zigbee.md#state-device-to-ha); one u32 write per wake is negligible flash wear for NVS. */
uint32_t persistent_store_next_wake_count(void)
{
    uint32_t count = 0;
    nvs_get_u32(store, STORE_KEY_WAKE_COUNT, &count);
    count = (count + 1) & 0xFFFFFF;
    nvs_set_u32(store, STORE_KEY_WAKE_COUNT, count);
    nvs_commit(store);
    return count;
}
