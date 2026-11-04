#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_log.h"
#include "battery.h"
#include "console.h"
#include "display.h"
#include "persistent_store.h"

static const char *TAG = "greenlight";

/* ---- Console (USB-Serial-JTAG): the battery simulation, see tools/console.py ----
 * "bat <mV>" sets the simulated battery voltage, "usb 0|1" the simulated USB power, "bat" or "usb" alone print
 * them. Applied at once; the state loops pick them up. */
static void console_task(void *arg)
{
    char line[32];
    for (;;) {
        if (!fgets(line, sizeof(line), stdin)) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        int v;
        if (line[0] == '\r' || line[0] == '\n') {
            continue;
        } else if (sscanf(line, "bat %d", &v) == 1) {
            if (v < 2500 || v > 4500) {
                ESP_LOGW(TAG, "console: bat %d out of range (2500-4500)", v);
                continue;
            }
            sim_mv = v;
            nvs_set_u16(nvs, "sim_mv", v);
            nvs_commit(nvs);
        } else if (sscanf(line, "usb %d", &v) == 1) {
            sim_usb = v != 0;
            nv_set("sim_usb", sim_usb);
        } else if (strncmp(line, "bat", 3) != 0 && strncmp(line, "usb", 3) != 0) {
            ESP_LOGW(TAG, "console: unknown command (bat <mV>, usb 0|1)");
            continue;
        }
        ESP_LOGI(TAG, "battery: %u mV (simulated), usb %d, latched %d", sim_mv, sim_usb, bat_latched);
        bat_changed = true;
        view_dirty  = true;
    }
}

void console_init(void)
{
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&cfg));
    usb_serial_jtag_vfs_use_driver();  /* blocking reads; writes are dropped while no host listens */
    usb_serial_jtag_vfs_set_rx_line_endings(ESP_LINE_ENDINGS_LF);
    setvbuf(stdin, NULL, _IONBF, 0);
    xTaskCreate(console_task, "console", 3072, NULL, 4, NULL);
}
