#include <stdbool.h>
#include <stdint.h>
#include "esp_log.h"
#include "board/battery.h"
#include "board/battery_sense.h"
#include "board/usb_sense.h"
#include "persistent_store.h"

static const char *TAG = "greenlight";

/* The voltage is measured at boot (at rest, before the radio and the panel start) and again at each mid-wake
 * check (battery_measure()); USB power is read from the USB sense pin. The console can override both (bench tests
 * with the USB cable in); the overrides are kept in NVS. The latch makes the exit
 * threshold apply (hysteresis). USB power overrides a low battery: the device then runs from USB. */
static volatile uint16_t measured_mv;
static volatile uint16_t simulated_mv;  /* 0: the measured voltage applies */
static volatile battery_usb_override_t usb_override;
static volatile bool     low_latched;
static volatile uint32_t simulation_changes;  /* announced by the console */

static void log_status(void)
{
    static const char *const usb_source[] = {"sensed", "override", "override"};
    ESP_LOGI(TAG, "battery: %u mV (%s), usb %d (%s; sense %d), latched %d", battery_voltage_mv(),
             simulated_mv ? "simulated" : "measured", battery_on_usb_power(), usb_source[usb_override],
             usb_sense_read(), low_latched);
}

void battery_init(void)
{
    usb_sense_init();
    measured_mv = battery_sense_read_mv(true);
    uint16_t mv;
    if (persistent_store_get_u16(STORE_KEY_SIMULATED_MV, &mv)) {
        simulated_mv = mv;
    }
    uint8_t usb = persistent_store_get_u8(STORE_KEY_SIMULATED_USB);
    usb_override = usb > BATTERY_USB_OFF ? BATTERY_USB_SENSED : (battery_usb_override_t)usb;
    low_latched         = persistent_store_get_u8(STORE_KEY_LOW_BATTERY);
    log_status();
}

void battery_measure(void)
{
    measured_mv = battery_sense_read_mv(false);
}

uint16_t battery_voltage_mv(void)
{
    return simulated_mv ? simulated_mv : measured_mv;
}

bool battery_on_usb_power(void)
{
    switch (usb_override) {
    case BATTERY_USB_ON:  return true;
    case BATTERY_USB_OFF: return false;
    default:              return usb_sense_read();
    }
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

void battery_simulate_usb_power(battery_usb_override_t usb)
{
    usb_override = usb;
    persistent_store_set_u8(STORE_KEY_SIMULATED_USB, usb);
}

void battery_announce_simulation(void)
{
    log_status();
    simulation_changes++;
}

uint32_t battery_simulation_changes(void)
{
    return simulation_changes;
}
