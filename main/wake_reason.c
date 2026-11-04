#include <stdint.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "epd.h"
#include "wake_reason.h"

static const char *TAG = "greenlight";

char       woke_by[24];  /* what ended the sleep; BOOT itself only looks at the input levels */

/* The start of the log is lost after a deep-sleep wake on USB (the host re-enumerates the port, and on Windows no
 * data comes through at all, DESIGN.md 3.6), so the wake cause is logged again with the stack start-up. */
void wake_cause_init(void)
{
    uint32_t causes = esp_sleep_get_wakeup_causes();
    if (causes & BIT(ESP_SLEEP_WAKEUP_EXT1)) {
        uint64_t pins = esp_sleep_get_ext1_wakeup_status();
        snprintf(woke_by, sizeof woke_by, "%s%s", pins & (1ULL << PIR_PIN) ? "PIR " : "",
                 pins & (1ULL << PAIR_BUTTON_PIN) ? "pair button" : "");
    } else if (causes & BIT(ESP_SLEEP_WAKEUP_TIMER)) {
        snprintf(woke_by, sizeof woke_by, "timer");
    } else {
        snprintf(woke_by, sizeof woke_by, "reset %d", esp_reset_reason());
    }
    ESP_LOGI(TAG, "wake: %s", woke_by);
}
