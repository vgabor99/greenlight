#include <stdbool.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_rtc_time.h"
#include "esp_system.h"
#include "view_freshness.h"

static const char *TAG = "greenlight";

/* The deadline is in RTC time (esp_rtc_get_time_us(): monotonic, keeps counting through deep sleep and a software
 * restart, 0 again after a power-on) and lives in RTC memory, which survives the same, so a wake sees how old the
 * stored view is. */
#define VALID_MAGIC 0x5CEE0A11u

static RTC_NOINIT_ATTR uint32_t valid_magic;
static RTC_NOINIT_ATTR uint64_t valid_until_us;  /* RTC time; 0 = never received */

/* The Zigbee task renews the deadline while the state machine reads it: a 64-bit value needs a lock here. */
static portMUX_TYPE valid_until_lock = portMUX_INITIALIZER_UNLOCKED;

static uint64_t read_valid_until_us(void)
{
    taskENTER_CRITICAL(&valid_until_lock);
    uint64_t valid_until = valid_until_us;
    taskEXIT_CRITICAL(&valid_until_lock);
    return valid_until;
}

void view_freshness_init(void)
{
    esp_reset_reason_t reason = esp_reset_reason();
    bool rtc_counter_restarted = valid_until_us > esp_rtc_get_time_us() + VIEW_VALID_MS * 1000ULL;
    if (valid_magic != VALID_MAGIC || reason == ESP_RST_POWERON || reason == ESP_RST_BROWNOUT ||
        reason == ESP_RST_UNKNOWN || rtc_counter_restarted) {
        valid_magic    = VALID_MAGIC;
        valid_until_us = 0;
    }
    ESP_LOGI(TAG, "rtc time %llu ms, view valid until %llu ms", esp_rtc_get_time_us() / 1000, valid_until_us / 1000);
}

void view_freshness_renew(void)
{
    uint64_t valid_until = esp_rtc_get_time_us() + VIEW_VALID_MS * 1000ULL;
    taskENTER_CRITICAL(&valid_until_lock);
    valid_until_us = valid_until;
    taskEXIT_CRITICAL(&valid_until_lock);
}

bool view_freshness_is_stale(void)
{
    return esp_rtc_get_time_us() > read_valid_until_us();
}

int64_t view_freshness_age_ms(void)
{
    uint64_t valid_until = read_valid_until_us();
    if (!valid_until) {
        return -1;
    }
    return ((int64_t)esp_rtc_get_time_us() - (int64_t)valid_until) / 1000 + VIEW_VALID_MS;
}
