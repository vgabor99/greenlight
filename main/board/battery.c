#include <stdbool.h>
#include <stdint.h>
#include "esp_log.h"
#include "board/battery.h"
#include "board/battery_sense.h"
#include "persistent_store.h"

static const char *TAG = "greenlight";

/* The voltage is measured once at boot (at rest, before the radio and the panel start); the console can override
 * it (0 = measured). USB power is still simulated. Both console values are kept in NVS. The latch makes the exit
 * threshold apply (hysteresis). USB power overrides a low battery: the device then runs from USB. */
static uint16_t          measured_mv;
static volatile uint16_t simulated_mv;  /* 0: the measured voltage applies */
static volatile bool     simulated_usb_power;
static volatile bool     low_latched;
static volatile bool     simulation_changed;  /* the console changed the simulation: report it */

static void log_status(void)
{
    ESP_LOGI(TAG, "battery: %u mV (%s), usb %d (simulated), latched %d", battery_voltage_mv(),
             simulated_mv ? "simulated" : "measured", simulated_usb_power, low_latched);
}

void battery_init(void)
{
    measured_mv = battery_sense_read_mv();
    uint16_t mv;
    if (persistent_store_get_u16(STORE_KEY_SIMULATED_MV, &mv)) {
        simulated_mv = mv;
    }
    simulated_usb_power = persistent_store_get_u8(STORE_KEY_SIMULATED_USB);
    low_latched         = persistent_store_get_u8(STORE_KEY_LOW_BATTERY);
    log_status();
}

uint16_t battery_voltage_mv(void)
{
    return simulated_mv ? simulated_mv : measured_mv;
}

bool battery_on_usb_power(void)
{
    return simulated_usb_power;  /* later the VBUS sense pin */
}

bool battery_is_low(void)
{
    return !battery_on_usb_power() && battery_voltage_mv() < (low_latched ? BATTERY_LOW_EXIT_MV : BATTERY_LOW_ENTER_MV);
}

bool battery_low_latched(void)
{
    return low_latched;
}

void battery_set_low_latched(bool latched)
{
    low_latched = latched;
    persistent_store_set_u8(STORE_KEY_LOW_BATTERY, latched);
}

void battery_simulate_voltage(uint16_t mv)
{
    simulated_mv = mv;
    persistent_store_set_u16(STORE_KEY_SIMULATED_MV, mv);
}

void battery_simulate_usb_power(bool on)
{
    simulated_usb_power = on;
    persistent_store_set_u8(STORE_KEY_SIMULATED_USB, on);
}

void battery_announce_simulation(void)
{
    log_status();
    simulation_changed = true;
}

bool battery_take_simulation_change(void)
{
    if (!simulation_changed) {
        return false;
    }
    simulation_changed = false;
    return true;
}
