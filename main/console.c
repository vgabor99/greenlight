#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_log.h"
#include "battery.h"
#include "console.h"
#include "display_model.h"

static const char *TAG = "greenlight";

#define SIMULATED_MV_MIN 2500
#define SIMULATED_MV_MAX 4500

static void handle_command(const char *line)
{
    int value;
    if (line[0] == '\r' || line[0] == '\n') {
        return;
    } else if (sscanf(line, "bat %d", &value) == 1) {
        if (value < SIMULATED_MV_MIN || value > SIMULATED_MV_MAX) {
            ESP_LOGW(TAG, "console: bat %d out of range (%d-%d)", value, SIMULATED_MV_MIN, SIMULATED_MV_MAX);
            return;
        }
        battery_simulate_voltage(value);
    } else if (sscanf(line, "usb %d", &value) == 1) {
        battery_simulate_usb_power(value != 0);
    } else if (strncmp(line, "bat", 3) != 0 && strncmp(line, "usb", 3) != 0) {
        ESP_LOGW(TAG, "console: unknown command (bat <mV>, usb 0|1)");
        return;
    }
    battery_announce_simulation();
    display_model_mark_changed();
}

static void console_task(void *arg)
{
    char line[32];
    for (;;) {
        if (fgets(line, sizeof(line), stdin)) {
            handle_command(line);
        } else {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
}

void console_init(void)
{
    usb_serial_jtag_driver_config_t config = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&config));
    usb_serial_jtag_vfs_use_driver();  /* blocking reads; writes are dropped while no host listens */
    usb_serial_jtag_vfs_set_rx_line_endings(ESP_LINE_ENDINGS_LF);
    setvbuf(stdin, NULL, _IONBF, 0);
    xTaskCreate(console_task, "console", 3072, NULL, 4, NULL);
}
