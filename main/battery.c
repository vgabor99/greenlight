#include <stdbool.h>
#include <stdint.h>
#include "esp_log.h"
#include "battery.h"
#include "persistent_store.h"

static const char *TAG = "greenlight";

/* Battery: simulated until the hardware is there. The voltage and USB power are set over the console and kept
 * in NVS ("sim_mv", "sim_usb"), so they survive the sleep, a power cycle and a reflash like a real battery's
 * charge. "bat_low" latches LOW_BATTERY: while it is set, the exit threshold applies (hysteresis). USB power
 * overrides a low battery: the device then runs from USB. */
volatile uint16_t sim_mv = SIM_MV_DEFAULT;
volatile bool     sim_usb;
volatile bool     bat_latched;
volatile bool     bat_changed;  /* the console changed the simulation: report it */

void battery_load(void)
{
    uint16_t mv;
    if (nvs_get_u16(nvs, "sim_mv", &mv) == ESP_OK) {
        sim_mv = mv;
    }
    sim_usb     = nv_get("sim_usb");
    bat_latched = nv_get("bat_low");
    ESP_LOGI(TAG, "battery: %u mV (simulated), usb %d, latched %d", sim_mv, sim_usb, bat_latched);
}

uint16_t battery_mv(void)
{
    return sim_mv;  /* later the ADC, sampled at rest and averaged */
}

bool usb_powered(void)
{
    return sim_usb;  /* later the VBUS sense pin */
}

bool battery_low(void)
{
    return !usb_powered() && battery_mv() < (bat_latched ? BAT_EXIT_MV : BAT_ENTER_MV);
}

void bat_latch(bool on)
{
    bat_latched = on;
    nv_set("bat_low", on);
}
