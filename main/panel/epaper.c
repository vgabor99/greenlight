/* Driver for the Waveshare 4.2" e-paper module (400x300, SSD1683 controller).
 * Partial refresh follows Waveshare's EPD_4IN2_V2 example code. The panel's own full-refresh waveform
 * hangs (BUSY never releases), so epaper_clear() emulates it with repeated partial updates. */

#include <string.h>
#include "board/board.h"
#include "panel/epaper.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "epd";

/* SSD1683 commands */
#define CMD_DRIVER_OUTPUT_CONTROL    0x01
#define CMD_DEEP_SLEEP               0x10
#define CMD_DATA_ENTRY_MODE          0x11
#define CMD_SOFTWARE_RESET           0x12
#define CMD_TEMPERATURE_SENSOR       0x18
#define CMD_MASTER_ACTIVATION        0x20  /* run the display update sequence */
#define CMD_DISPLAY_UPDATE_CONTROL_1 0x21  /* RAM content options */
#define CMD_DISPLAY_UPDATE_CONTROL_2 0x22  /* the display update sequence */
#define CMD_WRITE_RAM_NEW            0x24  /* the black/white RAM: the image to show */
#define CMD_WRITE_RAM_OLD            0x26  /* the "red" RAM, which mode 2 uses as the previous image */
#define CMD_BORDER_WAVEFORM          0x3C
#define CMD_RAM_X_RANGE              0x44
#define CMD_RAM_Y_RANGE              0x45
#define CMD_RAM_X_COUNTER            0x4E
#define CMD_RAM_Y_COUNTER            0x4F

#define UPDATE_PARTIAL 0xFF  /* load temp+LUT, display mode 2 (partial waveform), ~440 ms */
#define WHITE_PASSES   1     /* passes to white in epaper_clear(); more passes make the white grainier */

static spi_device_handle_t spi;
static uint8_t             old_ram[EPAPER_FRAME_BYTES];

/* The panel's signal pins: epaper_power_off() drives and holds them low. */
static const int signal_pins[] = {EPAPER_PIN_SCK, EPAPER_PIN_MOSI, EPAPER_PIN_CS, EPAPER_PIN_DC, EPAPER_PIN_RST};
#define SIGNAL_PIN_COUNT (sizeof(signal_pins) / sizeof(signal_pins[0]))

/* ---- SPI ---- */

static void send_command(uint8_t command)
{
    gpio_set_level(EPAPER_PIN_DC, 0);
    spi_transaction_t transaction = {.length = 8, .tx_buffer = &command};
    ESP_ERROR_CHECK(spi_device_polling_transmit(spi, &transaction));
}

static void send_data(const uint8_t *data, size_t length)
{
    gpio_set_level(EPAPER_PIN_DC, 1);
    while (length) {
        size_t            chunk = length > 4000 ? 4000 : length;
        spi_transaction_t transaction = {.length = chunk * 8, .tx_buffer = data};
        ESP_ERROR_CHECK(spi_device_polling_transmit(spi, &transaction));
        data += chunk;
        length -= chunk;
    }
}

static void send_command_with_data(uint8_t command, const uint8_t *data, size_t length)
{
    send_command(command);
    send_data(data, length);
}

/* SSD1683: BUSY is high while the controller is working. */
static esp_err_t wait_while_busy(void)
{
    vTaskDelay(pdMS_TO_TICKS(5));  /* give BUSY time to rise after the command */
    for (int i = 0; i < 1000; i++) {  /* ~10 s */
        if (!gpio_get_level(EPAPER_PIN_BUSY)) {
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    ESP_LOGE(TAG, "timeout waiting for BUSY");
    return ESP_ERR_TIMEOUT;
}

static void init_pins_and_spi(void)
{
    /* epaper_power_off() holds the pins low; the hold survives a reset and would block the SPI signals. */
    for (size_t i = 0; i < SIGNAL_PIN_COUNT; i++) {
        gpio_hold_dis(signal_pins[i]);
    }
    gpio_config_t outputs = {
        .pin_bit_mask = (1ULL << EPAPER_PIN_DC) | (1ULL << EPAPER_PIN_RST),
        .mode         = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&outputs));
    gpio_config_t busy_input = {.pin_bit_mask = 1ULL << EPAPER_PIN_BUSY, .mode = GPIO_MODE_INPUT};
    ESP_ERROR_CHECK(gpio_config(&busy_input));

    spi_bus_config_t bus = {
        .mosi_io_num     = EPAPER_PIN_MOSI,
        .miso_io_num     = -1,
        .sclk_io_num     = EPAPER_PIN_SCK,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = 4096,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO));
    spi_device_interface_config_t device = {
        .clock_speed_hz = 4 * 1000 * 1000,
        .mode           = 0,
        .spics_io_num   = EPAPER_PIN_CS,
        .queue_size     = 1,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &device, &spi));
}

static void hardware_reset(void)
{
    gpio_set_level(EPAPER_PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(200));
    gpio_set_level(EPAPER_PIN_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(EPAPER_PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(200));
}

/* ---- Public API ---- */

esp_err_t epaper_init(void)
{
    init_pins_and_spi();
    hardware_reset();
    esp_err_t err = wait_while_busy();
    if (err != ESP_OK) {
        return err;
    }

    send_command(CMD_SOFTWARE_RESET);
    if ((err = wait_while_busy()) != ESP_OK) {
        return err;
    }
    send_command_with_data(CMD_DRIVER_OUTPUT_CONTROL,                           /* 300 gate lines */
                           (uint8_t[]){(EPAPER_HEIGHT - 1) & 0xFF, (EPAPER_HEIGHT - 1) >> 8, 0x00}, 3);
    send_command_with_data(CMD_TEMPERATURE_SENSOR, (uint8_t[]){0x80}, 1);       /* the internal one */
    send_command_with_data(CMD_DISPLAY_UPDATE_CONTROL_1, (uint8_t[]){0x40, 0x00}, 2);
    send_command_with_data(CMD_BORDER_WAVEFORM, (uint8_t[]){0x05}, 1);
    send_command_with_data(CMD_DATA_ENTRY_MODE, (uint8_t[]){0x03}, 1);          /* X+, Y+ */
    send_command_with_data(CMD_RAM_X_RANGE, (uint8_t[]){0x00, (EPAPER_WIDTH - 1) / 8}, 2);
    send_command_with_data(CMD_RAM_Y_RANGE, (uint8_t[]){0x00, 0x00, (EPAPER_HEIGHT - 1) & 0xFF, (EPAPER_HEIGHT - 1) >> 8},
                           4);
    return wait_while_busy();
}

/* Reset the RAM address counter; it does not reliably wrap after a full write. */
static void reset_ram_cursor(void)
{
    send_command_with_data(CMD_RAM_X_COUNTER, (uint8_t[]){0x00}, 1);
    send_command_with_data(CMD_RAM_Y_COUNTER, (uint8_t[]){0x00, 0x00}, 2);
}

/* Run the update sequence that `mode` selects and wait for it. */
static esp_err_t run_update(uint8_t mode)
{
    send_command_with_data(CMD_DISPLAY_UPDATE_CONTROL_2, (uint8_t[]){mode}, 1);
    send_command(CMD_MASTER_ACTIVATION);
    return wait_while_busy();
}

/* One strong mode-2 pass towards `image`. Mode 2 only drives pixels where new (0x24) != old (0x26), and
 * the controller scrambles its buffers after each update, so both are rewritten every pass with
 * old = inverse of new, which makes every pixel get driven. */
static esp_err_t drive_pass(const uint8_t *image)
{
    for (size_t i = 0; i < sizeof(old_ram); i++) {
        old_ram[i] = ~image[i];
    }
    reset_ram_cursor();
    send_command(CMD_WRITE_RAM_NEW);
    send_data(image, sizeof(old_ram));
    reset_ram_cursor();
    send_command(CMD_WRITE_RAM_OLD);
    send_data(old_ram, sizeof(old_ram));
    return run_update(UPDATE_PARTIAL);
}

/* Software "full refresh" to white. The panel's own full-refresh waveform (mode 1) hangs, so drive every
 * pixel to white with the working mode-2 waveform. Going to black first, or doing more passes, only made
 * the white background grainier. */
esp_err_t epaper_clear(void)
{
    send_command_with_data(CMD_DISPLAY_UPDATE_CONTROL_1, (uint8_t[]){0x40, 0x00}, 2);  /* as in init */
    send_command_with_data(CMD_BORDER_WAVEFORM, (uint8_t[]){0x05}, 1);

    static uint8_t white[EPAPER_FRAME_BYTES];
    memset(white, 0xFF, sizeof(white));
    for (int i = 0; i < WHITE_PASSES; i++) {
        esp_err_t err = drive_pass(white);
        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}

/* Partial refresh, as in Waveshare's EPD_4IN2_V2_Display_Partial(): only the new image (0x24) is
 * written; the controller diffs it against the previous frame (0x26) and drives just the changed
 * pixels (0x22 = 0xFF). <1 s, no flash, but ghosting builds up; epaper_clear() resets it. */
esp_err_t epaper_refresh_partial(const uint8_t *frame)
{
    send_command_with_data(CMD_DISPLAY_UPDATE_CONTROL_1, (uint8_t[]){0x00, 0x00}, 2);  /* normal compare of both RAMs */
    /* Border (outside the active area): Waveshare uses 0x80 here (border floats at VCOM), which fades to
     * grey over many updates. 0x05 drives it to white on every update like the init/clear settings, but
     * touches all rows and columns each time. With 0x80 the border degraded to black, so keep 0x05. */
    send_command_with_data(CMD_BORDER_WAVEFORM, (uint8_t[]){0x05}, 1);
    reset_ram_cursor();
    send_command(CMD_WRITE_RAM_NEW);
    send_data(frame, EPAPER_FRAME_BYTES);
    return run_update(UPDATE_PARTIAL);
}

int epaper_busy_level(void)
{
    return gpio_get_level(EPAPER_PIN_BUSY);
}

/* Deep sleep (0x10 = 0x01): the controller stops driving the panel. Only a hardware reset wakes it. */
static void enter_deep_sleep(void)
{
    wait_while_busy();
    send_command_with_data(CMD_DEEP_SLEEP, (uint8_t[]){0x01}, 1);
    vTaskDelay(pdMS_TO_TICKS(100));
}

/* Release the SPI bus and drive every signal low, so nothing feeds the panel through its input pins while the
 * supply goes away. The pins stay low through ESP32 deep sleep. */
void epaper_power_off(void)
{
    enter_deep_sleep();
    spi_bus_remove_device(spi);
    spi_bus_free(SPI2_HOST);
    for (size_t i = 0; i < SIGNAL_PIN_COUNT; i++) {
        gpio_reset_pin(signal_pins[i]);
        gpio_set_direction(signal_pins[i], GPIO_MODE_OUTPUT);
        gpio_set_level(signal_pins[i], 0);
        gpio_hold_en(signal_pins[i]);
    }
}
