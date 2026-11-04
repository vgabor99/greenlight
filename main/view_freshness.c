#include <stdbool.h>
#include <stdint.h>
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_rtc_time.h"
#include "esp_system.h"
#include "view_freshness.h"

static const char *TAG = "greenlight";

/* Staleness (DESIGN.md 5.3): every received view is valid for SCREEN_VALID_MS. The deadline is in RTC time
 * (esp_rtc_get_time_us(): monotonic, keeps counting through deep sleep and a software restart, 0 again after a
 * power-on) and lives in RTC memory, which survives the same, so a wake sees how old the stored view is. */
#define VALID_MAGIC 0x5CEE0A11u

static RTC_NOINIT_ATTR uint32_t valid_magic;
RTC_NOINIT_ATTR uint64_t screen_valid_until;  /* RTC time in us; 0 = never received */

void staleness_init(void)
{
    esp_reset_reason_t why = esp_reset_reason();
    if (valid_magic != VALID_MAGIC || why == ESP_RST_POWERON || why == ESP_RST_BROWNOUT || why == ESP_RST_UNKNOWN ||
        screen_valid_until > esp_rtc_get_time_us() + SCREEN_VALID_MS * 1000ULL) {  /* the RTC counter restarted */
        valid_magic = VALID_MAGIC;
        screen_valid_until = 0;
    }
    ESP_LOGI(TAG, "rtc time %llu ms, view valid until %llu ms", esp_rtc_get_time_us() / 1000,
             screen_valid_until / 1000);
}

bool screen_stale(void)
{
    return esp_rtc_get_time_us() > screen_valid_until;
}

/* Age of the last received view in ms, -1 if none is known. */
int64_t screen_age_ms(void)
{
    if (!screen_valid_until) {
        return -1;
    }
    return ((int64_t)esp_rtc_get_time_us() - (int64_t)screen_valid_until) / 1000 + SCREEN_VALID_MS;
}
