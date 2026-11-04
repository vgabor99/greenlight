#include <stdbool.h>
#include <stdint.h>
#include "esp_log.h"
#include "battery.h"
#include "persistent_store.h"

static const char *TAG = "greenlight";

#define SIMULATED_MV_DEFAULT 4100  /* simulated voltage until the console sets one */

/* The voltage and USB power are set over the console and kept in NVS. The latch makes the exit threshold apply
 * (hysteresis). USB power overrides a low battery: the device then runs from USB. */
static volatile uint16_t simulated_mv = SIMULATED_MV_DEFAULT;
static volatile bool     simulated_usb_power;
static volatile bool     low_latched;
static volatile bool     simulation_changed;  /* the console changed the simulation: report it */

static void log_status(void)
{
    ESP_LOGI(TAG, "battery: %u mV (simulated), usb %d, latched %d", simulated_mv, simulated_usb_power, low_latched);
}

void battery_init(void)
{
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
    return simulated_mv;  /* later the ADC, sampled at rest and averaged */
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
