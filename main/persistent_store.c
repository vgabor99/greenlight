#include <stdint.h>
#include <string.h>
#include "nvs_flash.h"
#include "persistent_store.h"

/* ---- State that survives the (simulated) deep sleep: NVS ---- */
nvs_handle_t nvs;

uint8_t nv_get(const char *key)
{
    uint8_t v = 0;
    nvs_get_u8(nvs, key, &v);
    return v;
}

void nv_set(const char *key, uint8_t v)
{
    ESP_ERROR_CHECK(nvs_set_u8(nvs, key, v));
    ESP_ERROR_CHECK(nvs_commit(nvs));
}

void nvs_open_state(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    ESP_ERROR_CHECK(nvs_open("greenlight", NVS_READWRITE, &nvs));
}

/* NVS (namespace "greenlight"): "pair_now" (start pairing right after the next boot, set by ERASE_NETWORK),
 * "ds" (blob: the dataset, DESIGN.md 5.1), "view" (blob: the view on the panel with its rows; its id is the
 * shown_view_id) and "wake_n" (the wake counter, DESIGN.md 6.4). The Zigbee stack keeps the network in the same
 * partition. */

/* Every boot is a wake (a deep-sleep wake is a reset), so count boots. 24 bits, like the ids and hashes
 * (DESIGN.md 6.4); one u32 write per wake is negligible flash wear for NVS. */
uint32_t wake_count_next(void)
{
    uint32_t n = 0;
    nvs_get_u32(nvs, "wake_n", &n);
    n = (n + 1) & 0xFFFFFF;
    nvs_set_u32(nvs, "wake_n", n);
    nvs_commit(nvs);
    return n;
}

void blob_load(const char *key, void *p, size_t size)
{
    size_t n = size;
    if (nvs_get_blob(nvs, key, p, &n) != ESP_OK || n != size) {
        memset(p, 0, size);
    }
}

void blob_save(const char *key, const void *p, size_t size)
{
    nvs_set_blob(nvs, key, p, size);
    nvs_commit(nvs);
}
