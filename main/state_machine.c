#include <stdbool.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "battery.h"
#include "display.h"
#include "display_model.h"
#include "epd.h"
#include "frame_dump.h"
#include "inputs.h"
#include "panel.h"
#include "persistent_store.h"
#include "sleep.h"
#include "state_machine.h"
#include "wake_reason.h"
#include "zb.h"

static const char *TAG = "greenlight";

/* ---- Timing (DESIGN.md 3.2) ---- */
#define RUN_MS        20000   /* activity timeout */
#define PAIR_MS       180000  /* pairing window */
#define INTERVIEW_MS  10000   /* TEMPORARY for development; design value 60000 */
#define RESET_HOLD_MS 3000    /* pair button hold that clears the network */
#define REJOIN_MS     15000   /* give up rejoining after this long */

#define STACK_START_MS 30000   /* BOOT waits this long for the Zigbee stack to restore the network */
#define HEARTBEAT_MS   30000   /* ACTIVE: repeat awake + shown_view_id this often */
#define REPORT_DELAY_MS 2000   /* after a rejoin: the reports that follow the wake counter (DESIGN.md 6.4) */
#define VIEW_WAIT_MS   45000   /* ACTIVE: stay awake this long for the wake's view, so HA's resend can land (3.2) */
#define GOODBYE_MS      5000   /* SHUTDOWN: wait this long for Z2M to confirm occupancy 0 and awake false */

const char *const state_names[] = {
    "BOOT", "UNPAIRED", "PAIRING", "INTERVIEW", "REJOIN", "ACTIVE", "ERASE_NETWORK", "SHUTDOWN", "SLEEP",
    "LOW_BATTERY",
};

static TickType_t boot_tick;
uint32_t   wake_n;  /* this wake's number */

/* Mid-wake battery check, at most every BAT_CHECK_MS: true when the wake must end in LOW_BATTERY (the caller
 * returns ST_SHUTDOWN, which draws the battery screen). */
static bool battery_check(void)
{
    static TickType_t last;
    if (ms_since(last) < BAT_CHECK_MS) {
        return false;
    }
    last = xTaskGetTickCount();
    if (!battery_low()) {
        return false;
    }
    ESP_LOGW(TAG, "battery low (%u mV): shutting down", battery_mv());
    failed = RENDER_BATTERY;
    return true;
}

/* The reports of a wake other than the wake counter (DESIGN.md 6.4). After a rejoin they follow the wake counter
 * by REPORT_DELAY_MS, so the push that the wake counter triggers is not held up by them. */
static void report_wake_rest(void)
{
    zb_report_dataset_hash();
    zb_report_shown_view_id();
    zb_report_awake(true);
    zb_report_battery(battery_mv(), battery_low());
    zb_report_occupancy(occupied());
}

static bool rest_pending;  /* ACTIVE: report_wake_rest() still to do */

static state_t st_boot(void)
{
    /* The battery first, before the radio, the panel and any NVS write (DESIGN.md 6.6). */
    if (bat_latched) {
        if (battery_low()) {
            ESP_LOGW(TAG, "boot: battery still low (%u mV)", battery_mv());
            return ST_LOW_BATTERY;  /* the panel still shows the battery screen */
        }
        ESP_LOGI(TAG, "boot: battery recovered (%u mV%s)", battery_mv(), usb_powered() ? ", USB" : "");
        bat_latch(false);
    } else if (battery_low()) {
        ESP_LOGW(TAG, "boot: battery low (%u mV)", battery_mv());
        if (epd_init() != ESP_OK) {
            ESP_LOGE(TAG, "e-paper init failed");
        }
        failed = RENDER_BATTERY;
        return ST_SHUTDOWN;  /* the stack is not started: no reports */
    }

    bool pair_now = nv_get("pair_now");
    state_load();
    wake_n = wake_count_next();
    ESP_LOGI(TAG, "boot: pair_now=%d dataset=%06lx view=%06lx wake=%lu", pair_now, (unsigned long)ds.hash,
             (unsigned long)shown.view.id, (unsigned long)wake_n);

#ifdef SCREEN_DUMP
    dump_samples();  /* before the stack starts, so its log does not cut into the dump */
#endif
    boot_tick = xTaskGetTickCount();
    zb_callbacks_t cbs = {.view = on_view, .dataset = on_dataset, .activity = touch_activity, .rx = on_rx};
    zb_start(&cbs, ds.hash, shown.view.id, wake_n, battery_mv());

    /* MVP: full clear on every wake (DESIGN.md 3.4), in parallel with the stack start-up. epd_init() also
     * releases the pin hold and recreates the SPI bus left by epd_poweroff(). */
    if (epd_init() != ESP_OK || panel_clear("boot") != ESP_OK) {
        ESP_LOGE(TAG, "e-paper init failed");
        vTaskDelay(portMAX_DELAY);
    }
    partials = 0;
    show_state(ST_BOOT);
    redraw();  /* the stored view, while the stack starts */

    bool factory_new = false;
    if (!zb_wait_started(STACK_START_MS, &factory_new)) {
        ESP_LOGE(TAG, "Zigbee stack did not start");
        failed = RENDER_ERROR;
        return ST_SHUTDOWN;
    }
    ESP_LOGI(TAG, "stack up after %lu ms: %s (wake: %s)", (unsigned long)ms_since(boot_tick),
             factory_new ? "no network" : "network restored", woke_by);
    touch_activity();
    if (!factory_new) {
        return ST_REJOIN;
    }
    if (pair_now) {
        nv_set("pair_now", 0);
        return ST_PAIRING;
    }
    return ST_UNPAIRED;
}

static state_t st_unpaired(void)
{
    show_state(ST_UNPAIRED);
    touch_activity();
    for (;;) {
        /* Long press, like the network reset in ACTIVE. A button still held from the wake-up counts from boot. */
        if (pair_held_ms() >= RESET_HOLD_MS) {
            ESP_LOGI(TAG, "pair button held %d ms", RESET_HOLD_MS);
            return ST_PAIRING;
        }
        if (idle_ms() >= RUN_MS) {
            ESP_LOGI(TAG, "no activity for %d ms", RUN_MS);
            failed = RENDER_UNPAIRED;  /* still not paired: sleep with how to pair */
            return ST_SHUTDOWN;
        }
        if (battery_check()) {
            return ST_SHUTDOWN;
        }
        wait_ms(20);
    }
}

static state_t st_pairing(void)
{
    show_state(ST_PAIRING);
    TickType_t start = xTaskGetTickCount();
    zb_pairing_start();
    while (ms_since(start) < PAIR_MS) {
        if (zb_joined()) {
            zb_pairing_stop();
            ESP_LOGI(TAG, "joined after %lu ms", (unsigned long)ms_since(start));
            return ST_INTERVIEW;
        }
        if (battery_check()) {
            zb_pairing_stop();
            return ST_SHUTDOWN;
        }
        wait_ms(100);
    }
    zb_pairing_stop();
    ESP_LOGW(TAG, "pairing window over");
    return ST_UNPAIRED;
}

static state_t st_interview(void)
{
    show_state(ST_INTERVIEW);
    report_wake_rest();  /* Z2M has no cached state yet: everything, the wake counter last */
    zb_report_wake();
    ESP_LOGI(TAG, "staying awake for %d ms", INTERVIEW_MS);
    TickType_t start = xTaskGetTickCount();
    while (ms_since(start) < INTERVIEW_MS) {  /* the PIR is ignored here */
        wait_ms(20);
    }
    return ST_ACTIVE;
}

static state_t st_rejoin(void)
{
    show_state(ST_REJOIN);
    TickType_t start = xTaskGetTickCount();
    touch_activity();
    while (ms_since(start) < REJOIN_MS) {
        if (zb_joined()) {
            ESP_LOGI(TAG, "rejoined, %lu ms after boot", (unsigned long)ms_since(boot_tick));
            zb_report_wake();  /* the only report before the push (DESIGN.md 6.4) */
            rest_pending = true;
            return ST_ACTIVE;
        }
        if (pair_held_ms() >= RESET_HOLD_MS) {  /* network lost: allow re-pairing */
            ESP_LOGI(TAG, "pair button held %d ms", RESET_HOLD_MS);
            return ST_ERASE_NETWORK;
        }
        wait_ms(20);
    }
    ESP_LOGW(TAG, "rejoin failed");
    failed = RENDER_JOIN_FAILED;
    return ST_SHUTDOWN;
}

static state_t st_active(void)
{
    show_state(ST_ACTIVE);
    touch_activity();
    bool       last_occ = occupied();
    TickType_t last_beat = xTaskGetTickCount();
    TickType_t start = last_beat;
    bool       waiting = false;
    for (;;) {
        if (rest_pending && ms_since(start) >= REPORT_DELAY_MS) {
            rest_pending = false;
            report_wake_rest();
            last_occ = occupied();
        }
        if (!rest_pending && occupied() != last_occ) {
            last_occ = !last_occ;
            zb_report_occupancy(last_occ);
        }
        if (battery_check()) {
            return ST_SHUTDOWN;
        }
        if (bat_changed && !rest_pending) {  /* the console changed the simulation */
            bat_changed = false;
            zb_report_battery(battery_mv(), battery_low());
        }
        if (ms_since(last_beat) >= HEARTBEAT_MS) {  /* tell HA we are still awake */
            last_beat = xTaskGetTickCount();
            zb_report_shown_view_id();
            zb_report_awake(true);
            zb_log_neighbors();
        }
        if (pair_held_ms() >= RESET_HOLD_MS) {
            ESP_LOGI(TAG, "pair button held %d ms", RESET_HOLD_MS);
            return ST_ERASE_NETWORK;
        }
        if (idle_ms() >= RUN_MS) {
            if (!view_seen && ms_since(start) < VIEW_WAIT_MS) {
                if (!waiting) {
                    waiting = true;
                    ESP_LOGI(TAG, "no view yet: staying awake up to %d ms", VIEW_WAIT_MS);
                }
            } else {
                ESP_LOGI(TAG, "no activity for %d ms%s", RUN_MS, view_seen ? "" : ", and no view");
                return ST_SHUTDOWN;
            }
        }
        wait_ms(20);
    }
}

static state_t st_erase_network(void)
{
    show_state(ST_ERASE_NETWORK);
    redraw();
    nv_set("pair_now", 1);
    nvs_erase_key(nvs, "ds");    /* nothing is known about the panel any more */
    nvs_erase_key(nvs, "view");
    nvs_commit(nvs);
    ESP_LOGI(TAG, "erasing the network, restarting");
    zb_erase_network();  /* does not return */
    return ST_BOOT;
}

static state_t st_shutdown(void)
{
    bool low_battery = failed == RENDER_BATTERY;
    if (low_battery) {
        bat_latch(true);
    }
    if (zb_joined()) {
        if (low_battery) {
            zb_report_battery(battery_mv(), true);  /* HA learns why the device went quiet */
        }
        zb_report_goodbye(GOODBYE_MS);  /* not joined (failed rejoin, unpaired, stack down): nothing to send */
    }
    /* Cleanest state for power-off: full white reset, then the screen to sleep with (SLEEP on the debug screen). */
    if (panel_clear("shutdown") != ESP_OK) {
        ESP_LOGE(TAG, "e-paper final clear failed");
    }
    partials = 0;
    show_state(low_battery ? ST_LOW_BATTERY : ST_SLEEP);
    redraw();
    ESP_LOGI(TAG, "final refresh done");
    TickType_t t0 = xTaskGetTickCount();
    epd_poweroff();
    ESP_LOGI(TAG, "panel: poweroff %lu ms", (unsigned long)ms_since(t0));
    ESP_LOGI(TAG, "panel asleep, pins low: safe to remove power now");
    return low_battery ? ST_LOW_BATTERY : ST_SLEEP;
}


void state_machine_run(void)
{
    state_t st = ST_BOOT;
    for (;;) {
        state_t next;
        switch (st) {
        case ST_BOOT:          next = st_boot(); break;
        case ST_UNPAIRED:      next = st_unpaired(); break;
        case ST_PAIRING:       next = st_pairing(); break;
        case ST_INTERVIEW:     next = st_interview(); break;
        case ST_REJOIN:        next = st_rejoin(); break;
        case ST_ACTIVE:        next = st_active(); break;
        case ST_ERASE_NETWORK: next = st_erase_network(); break;
        case ST_SHUTDOWN:      next = st_shutdown(); break;
        case ST_LOW_BATTERY:   next = st_low_battery(); break;
        default:               next = st_sleep(); break;
        }
        ESP_LOGI(TAG, "%s -> %s", state_names[st], state_names[next]);
        st = next;
    }
}
