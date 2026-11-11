#include <stdbool.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "board/battery.h"
#include "board/inputs.h"
#include "board/sleep.h"
#include "board/wake_reason.h"
#include "display/display.h"
#include "display/display_model.h"
#include "persistent_store.h"
#include "screen/sample_screens.h"
#include "state_machine.h"
#include "util/elapsed_time.h"
#include "zigbee/zigbee.h"

static const char *TAG = "greenlight";

/* ---- Timing (docs/behaviour.md#timeouts) ---- */
#define IDLE_TIMEOUT_MS        20000   /* no activity for this long ends the wake */
#define PAIRING_WINDOW_MS      180000
#define INTERVIEW_MS           60000   /* time to stay awake after joining, for the Z2M interview */
#define PAIR_BUTTON_HOLD_MS    3000    /* pair button hold that starts pairing, or clears the network */
#define REJOIN_TIMEOUT_MS      15000   /* give up rejoining after this long */
#define STACK_START_TIMEOUT_MS 30000   /* BOOT waits this long for the Zigbee stack to restore the network */
#define HEARTBEAT_MS           30000   /* ACTIVE: repeat awake + shown_view_id this often */
#define WAKE_REPORTS_DELAY_MS  2000    /* after a rejoin: the reports that follow the wake counter (docs/zigbee.md#when-the-device-reports) */
#define VIEW_WAIT_MS           45000   /* ACTIVE: stay awake this long for the wake's view, so HA's resend can land (docs/behaviour.md#timeouts) */
#define GOODBYE_TIMEOUT_MS     5000    /* SHUTDOWN: wait this long for Z2M to confirm occupancy 0 and awake false */
#define BATTERY_CHECK_MS       1000    /* mid-wake battery check period */
#define BATTERY_REPORT_TIMEOUT_MS 10000 /* SHUTDOWN: resend the low-battery report this long until Z2M confirms it */

static const char *const state_names[] = {
    "BOOT", "UNPAIRED", "PAIRING", "INTERVIEW", "REJOIN", "ACTIVE", "ERASE_NETWORK", "SHUTDOWN", "SLEEP",
    "LOW_BATTERY",
};
_Static_assert(sizeof(state_names) / sizeof(state_names[0]) == STATE_LOW_BATTERY + 1, "a name for every state");

static TickType_t boot_tick;
static uint32_t   wake_number;
static bool       wake_reports_pending;  /* ACTIVE: report_wake_state() still to do */

const char *device_state_name(device_state_t state)
{
    return state_names[state];
}

/* ---- Checks shared by the states ---- */

/* Mid-wake battery check, at most every BATTERY_CHECK_MS: true when the wake must end in LOW_BATTERY (the caller
 * goes to SHUTDOWN, which draws the battery screen). */
static bool battery_ran_low(void)
{
    static TickType_t last_check;
    if (elapsed_ms(last_check) < BATTERY_CHECK_MS) {
        return false;
    }
    last_check = xTaskGetTickCount();
    battery_measure();
    if (!battery_is_low()) {
        return false;
    }
    ESP_LOGW(TAG, "battery low (%u mV): shutting down", battery_voltage_mv());
    display_set_failure(RENDER_BATTERY);
    return true;
}

/* The long press that starts pairing (UNPAIRED) or clears the network (REJOIN, ACTIVE). A button still held from
 * the wake-up counts from boot. */
static bool pair_button_held_long(void)
{
    if (inputs_pair_button_held_ms() < PAIR_BUTTON_HOLD_MS) {
        return false;
    }
    ESP_LOGI(TAG, "pair button held %d ms", PAIR_BUTTON_HOLD_MS);
    return true;
}

/* The reports of a wake other than the wake counter (docs/zigbee.md#when-the-device-reports). After a rejoin they follow the wake counter
 * by WAKE_REPORTS_DELAY_MS, so the push that the wake counter triggers is not held up by them. */
static void report_wake_state(void)
{
    zigbee_report_dataset_hash();
    zigbee_report_shown_view_id();
    zigbee_report_awake(true);
    zigbee_report_battery(battery_voltage_mv(), battery_is_low());
    zigbee_report_occupancy(inputs_occupied());
}

/* ---- BOOT ---- */

/* The battery first, before the radio, the panel and any NVS write (docs/behaviour.md#low-battery). STATE_BOOT to go on booting,
 * else the state to go to. */
static device_state_t check_battery_at_boot(void)
{
    if (battery_low_latched()) {
        if (battery_is_low()) {
            ESP_LOGW(TAG, "boot: battery still low (%u mV)", battery_voltage_mv());
            return STATE_LOW_BATTERY;  /* the panel still shows the battery screen */
        }
        ESP_LOGI(TAG, "boot: battery recovered (%u mV%s)", battery_voltage_mv(), battery_on_usb_power() ? ", USB" : "");
        battery_set_low_latched(false);
    } else if (battery_is_low()) {
        /* Not latched yet: boot on, so the first mid-wake check ends the wake with the battery_low report */
        ESP_LOGW(TAG, "boot: battery low (%u mV): reporting it, then shutting down", battery_voltage_mv());
    }
    return STATE_BOOT;
}

/* Start the Zigbee stack, and the panel with the stored view while the stack starts up. */
static void start_stack_and_panel(void)
{
    boot_tick = xTaskGetTickCount();
    zigbee_callbacks_t callbacks = {
        .on_view           = display_model_on_view,
        .on_dataset        = display_model_on_dataset,
        .on_activity       = inputs_note_activity,
        .on_received_write = display_model_on_received_write,
    };
    zigbee_start(&callbacks, display_model_dataset_hash(), display_model_shown_view_id(), wake_number,
             battery_voltage_mv());

    /* MVP: full clear on every wake (docs/behaviour.md#panel-on-each-wake), in parallel with the stack start-up. A
     * failed panel stays visible: no recovery (docs/behaviour.md#failure-modes). */
    if (!display_power_on()) {
        ESP_LOGE(TAG, "e-paper init failed");
        vTaskDelay(portMAX_DELAY);
    }
    display_show_state(STATE_BOOT);
    display_redraw();
}

static device_state_t run_boot(void)
{
    device_state_t battery_verdict = check_battery_at_boot();
    if (battery_verdict != STATE_BOOT) {
        return battery_verdict;
    }

    bool pair_on_boot = persistent_store_get_u8(STORE_KEY_PAIR_ON_BOOT);
    display_model_load();
    wake_number = persistent_store_next_wake_count();
    display_set_wake_number(wake_number);
    ESP_LOGI(TAG, "boot: pair_now=%d dataset=%06lx view=%06lx wake=%lu", pair_on_boot,
             (unsigned long)display_model_dataset_hash(), (unsigned long)display_model_shown_view_id(),
             (unsigned long)wake_number);

#ifdef SCREEN_DUMP
    sample_screens_dump();  /* before the stack starts, so its log does not cut into the dump */
#endif
    start_stack_and_panel();

    bool factory_new = false;
    if (!zigbee_wait_started(STACK_START_TIMEOUT_MS, &factory_new)) {
        ESP_LOGE(TAG, "Zigbee stack did not start");
        display_set_failure(RENDER_ERROR);
        return STATE_SHUTDOWN;
    }
    ESP_LOGI(TAG, "stack up after %lu ms: %s (wake: %s)", (unsigned long)elapsed_ms(boot_tick),
             factory_new ? "no network" : "network restored", wake_reason_text());
    inputs_note_activity();
    if (!factory_new) {
        return STATE_REJOIN;
    }
    if (pair_on_boot) {
        persistent_store_set_u8(STORE_KEY_PAIR_ON_BOOT, 0);
        return STATE_PAIRING;
    }
    return STATE_UNPAIRED;
}

/* ---- Joining a network ---- */

static device_state_t run_unpaired(void)
{
    display_show_state(STATE_UNPAIRED);
    inputs_note_activity();
    for (;;) {
        if (pair_button_held_long()) {
            return STATE_PAIRING;
        }
        if (inputs_idle_ms() >= IDLE_TIMEOUT_MS) {
            ESP_LOGI(TAG, "no activity for %d ms", IDLE_TIMEOUT_MS);
            display_set_failure(RENDER_UNPAIRED);  /* still not paired: sleep with how to pair */
            return STATE_SHUTDOWN;
        }
        if (battery_ran_low()) {
            return STATE_SHUTDOWN;
        }
        display_wait_ms(20);
    }
}

static device_state_t run_pairing(void)
{
    display_show_state(STATE_PAIRING);
    TickType_t start = xTaskGetTickCount();
    zigbee_start_pairing();
    while (elapsed_ms(start) < PAIRING_WINDOW_MS) {
        if (zigbee_is_joined()) {
            zigbee_stop_pairing();
            ESP_LOGI(TAG, "joined after %lu ms", (unsigned long)elapsed_ms(start));
            return STATE_INTERVIEW;
        }
        if (battery_ran_low()) {
            zigbee_stop_pairing();
            return STATE_SHUTDOWN;
        }
        display_wait_ms(100);
    }
    zigbee_stop_pairing();
    ESP_LOGW(TAG, "pairing window over");
    return STATE_UNPAIRED;
}

static device_state_t run_interview(void)
{
    display_show_state(STATE_INTERVIEW);
    report_wake_state();  /* Z2M has no cached state yet: everything, the wake counter last */
    zigbee_report_wake_count();
    ESP_LOGI(TAG, "staying awake for %d ms", INTERVIEW_MS);
    TickType_t start = xTaskGetTickCount();
    while (elapsed_ms(start) < INTERVIEW_MS) {  /* the PIR is ignored here */
        display_wait_ms(20);
    }
    return STATE_ACTIVE;
}

static device_state_t run_rejoin(void)
{
    display_show_state(STATE_REJOIN);
    TickType_t start = xTaskGetTickCount();
    inputs_note_activity();
    while (elapsed_ms(start) < REJOIN_TIMEOUT_MS) {
        if (zigbee_is_joined()) {
            ESP_LOGI(TAG, "rejoined, %lu ms after boot", (unsigned long)elapsed_ms(boot_tick));
            zigbee_report_wake_count();  /* the only report before the push (docs/zigbee.md#when-the-device-reports) */
            wake_reports_pending = true;
            return STATE_ACTIVE;
        }
        if (pair_button_held_long()) {  /* network lost: allow re-pairing */
            return STATE_ERASE_NETWORK;
        }
        display_wait_ms(20);
    }
    ESP_LOGW(TAG, "rejoin failed");
    battery_measure();
    display_set_failure(battery_is_low() ? RENDER_BATTERY : RENDER_JOIN_FAILED);  /* not latched: retried next wake */
    return STATE_SHUTDOWN;
}

/* ---- ACTIVE ---- */

/* The wake's other reports once WAKE_REPORTS_DELAY_MS is over, then the occupancy whenever it changes. */
static void report_pending_and_occupancy(TickType_t active_since, bool *reported_occupancy)
{
    if (wake_reports_pending && elapsed_ms(active_since) >= WAKE_REPORTS_DELAY_MS) {
        wake_reports_pending = false;
        report_wake_state();
        *reported_occupancy = inputs_occupied();
    }
    if (!wake_reports_pending && inputs_occupied() != *reported_occupancy) {
        *reported_occupancy = !*reported_occupancy;
        zigbee_report_occupancy(*reported_occupancy);
    }
}

/* Tell HA we are still awake. */
static void report_heartbeat(void)
{
    zigbee_report_shown_view_id();
    zigbee_report_awake(true);
    zigbee_log_neighbors();
}

/* No activity for IDLE_TIMEOUT_MS: true while the wake goes on anyway, because its view has not arrived yet (up to
 * VIEW_WAIT_MS after ACTIVE began). *announced: the wait was logged. */
static bool keep_waiting_for_view(TickType_t active_since, bool *announced)
{
    if (!display_model_view_received() && elapsed_ms(active_since) < VIEW_WAIT_MS) {
        if (!*announced) {
            *announced = true;
            ESP_LOGI(TAG, "no view yet: staying awake up to %d ms", VIEW_WAIT_MS);
        }
        return true;
    }
    ESP_LOGI(TAG, "no activity for %d ms%s", IDLE_TIMEOUT_MS, display_model_view_received() ? "" : ", and no view");
    return false;
}

static device_state_t run_active(void)
{
    display_show_state(STATE_ACTIVE);
    inputs_note_activity();
    bool       reported_occupancy = inputs_occupied();
    TickType_t last_heartbeat = xTaskGetTickCount();
    TickType_t start = last_heartbeat;
    bool       view_wait_announced = false;
    for (;;) {
        report_pending_and_occupancy(start, &reported_occupancy);
        if (battery_ran_low()) {
            return STATE_SHUTDOWN;
        }
        if (!wake_reports_pending && battery_take_simulation_change()) {  /* the console changed the simulation */
            zigbee_report_battery(battery_voltage_mv(), battery_is_low());
        }
        if (elapsed_ms(last_heartbeat) >= HEARTBEAT_MS) {
            last_heartbeat = xTaskGetTickCount();
            report_heartbeat();
        }
        if (pair_button_held_long()) {
            return STATE_ERASE_NETWORK;
        }
        if (inputs_idle_ms() >= IDLE_TIMEOUT_MS && !keep_waiting_for_view(start, &view_wait_announced)) {
            return STATE_SHUTDOWN;
        }
        display_wait_ms(20);
    }
}

/* ---- Ending a wake ---- */

static device_state_t run_erase_network(void)
{
    display_show_state(STATE_ERASE_NETWORK);
    display_redraw();
    persistent_store_set_u8(STORE_KEY_PAIR_ON_BOOT, 1);
    display_model_erase_stored();
    ESP_LOGI(TAG, "erasing the network, restarting");
    zigbee_erase_network();  /* does not return */
    return STATE_BOOT;
}

static device_state_t run_shutdown(void)
{
    bool low_battery = display_failure() == RENDER_BATTERY;
    bool latch = false;
    if (zigbee_is_joined()) {
        /* HA learns why the device went quiet. Only a confirmed report latches; else the next wake tries again. */
        if (low_battery) {
            latch = zigbee_report_battery_low(battery_voltage_mv(), BATTERY_REPORT_TIMEOUT_MS);
        }
        zigbee_report_goodbye(GOODBYE_TIMEOUT_MS);  /* not joined (failed rejoin, unpaired, stack down): nothing to send */
    }
    if (latch) {
        battery_set_low_latched(true);
    } else if (low_battery) {
        ESP_LOGW(TAG, "battery low not reported: not latched, the next wake tries again");
    }
    device_state_t next = latch ? STATE_LOW_BATTERY : STATE_SLEEP;
    display_power_off(next);  /* the failure screen (Battery dead) is drawn either way */
    return next;
}

static device_state_t run_low_battery(void)
{
    sleep_until_usb_power(); /* This does not return: the device will sleep and reboot. */
    return STATE_BOOT; /* NOTREACHED */
}

static device_state_t run_sleep(void)
{
    sleep_until_input(); /* This does not return: the device will sleep and reboot. */
    return STATE_BOOT; /* NOTREACHED */
}

void state_machine_run(void)
{
    device_state_t state = STATE_BOOT;
    for (;;) {
        device_state_t next;
        switch (state) {
        case STATE_BOOT:          next = run_boot(); break;
        case STATE_UNPAIRED:      next = run_unpaired(); break;
        case STATE_PAIRING:       next = run_pairing(); break;
        case STATE_INTERVIEW:     next = run_interview(); break;
        case STATE_REJOIN:        next = run_rejoin(); break;
        case STATE_ACTIVE:        next = run_active(); break;
        case STATE_ERASE_NETWORK: next = run_erase_network(); break;
        case STATE_SHUTDOWN:      next = run_shutdown(); break;
        case STATE_LOW_BATTERY:   next = run_low_battery(); break;
        case STATE_SLEEP:
        default:                  next = run_sleep(); break;
        }
        ESP_LOGI(TAG, "%s -> %s", state_names[state], state_names[next]);
        state = next;
    }
}
