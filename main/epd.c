/* Driver for the Waveshare 4.2" e-paper module (400x300, SSD1683 controller).
 *
 * This panel hangs (BUSY stays high forever) on the full-refresh update (display mode 1, 0x22 = 0xF7),
 * so we use the quick mode-2 update (0x22 = 0xFF, ~420 ms) instead. That waveform drives black only
 * weakly, so epd_display() repeats it EPD_REFRESH_PASSES times; ~10-12 passes reach full black. */

#include "epd.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#define EPD_REFRESH_PASSES 12
#define EPD_UPDATE_MODE2   0xFF  /* enable clock+analog, load temp+LUT, display mode 2, display, power down */

static const char *TAG = "epd";
static spi_device_handle_t spi;
static uint8_t old_ram[EPD_STRIDE * EPD_HEIGHT];

static void cmd(uint8_t c)
{
    gpio_set_level(EPD_PIN_DC, 0);
    spi_transaction_t t = { .length = 8, .tx_buffer = &c };
    ESP_ERROR_CHECK(spi_device_polling_transmit(spi, &t));
}

static void data(const uint8_t *d, size_t len)
{
    gpio_set_level(EPD_PIN_DC, 1);
    while (len) {
        size_t n = len > 4000 ? 4000 : len;
        spi_transaction_t t = { .length = n * 8, .tx_buffer = d };
        ESP_ERROR_CHECK(spi_device_polling_transmit(spi, &t));
        d += n;
        len -= n;
    }
}

static void cmd_data(uint8_t c, const uint8_t *d, size_t len)
{
    cmd(c);
    data(d, len);
}

/* SSD1683: BUSY is high while the controller is working. */
static esp_err_t wait_busy(void)
{
    vTaskDelay(pdMS_TO_TICKS(5));  /* give BUSY time to rise after the command */
    for (int i = 0; i < 1000; i++) {  /* ~10 s */
        if (!gpio_get_level(EPD_PIN_BUSY)) {
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    ESP_LOGE(TAG, "timeout waiting for BUSY");
    return ESP_ERR_TIMEOUT;
}

esp_err_t epd_init(void)
{
    gpio_config_t out = {
        .pin_bit_mask = (1ULL << EPD_PIN_DC) | (1ULL << EPD_PIN_RST),
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&out));
    gpio_config_t in = { .pin_bit_mask = 1ULL << EPD_PIN_BUSY, .mode = GPIO_MODE_INPUT };
    ESP_ERROR_CHECK(gpio_config(&in));

    spi_bus_config_t bus = {
        .mosi_io_num = EPD_PIN_MOSI,
        .miso_io_num = -1,
        .sclk_io_num = EPD_PIN_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4096,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO));
    spi_device_interface_config_t dev = {
        .clock_speed_hz = 4 * 1000 * 1000,
        .mode = 0,
        .spics_io_num = EPD_PIN_CS,
        .queue_size = 1,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &dev, &spi));

    /* Hardware reset */
    gpio_set_level(EPD_PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(200));
    gpio_set_level(EPD_PIN_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(EPD_PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_err_t err = wait_busy();
    if (err != ESP_OK) {
        return err;
    }

    cmd(0x12);                                  /* software reset */
    if ((err = wait_busy()) != ESP_OK) {
        return err;
    }
    cmd_data(0x21, (uint8_t[]){0x40, 0x00}, 2); /* display update control */
    cmd_data(0x3C, (uint8_t[]){0x05}, 1);       /* border waveform */
    cmd_data(0x11, (uint8_t[]){0x03}, 1);       /* data entry mode: X+, Y+ */
    cmd_data(0x44, (uint8_t[]){0x00, (EPD_WIDTH - 1) / 8}, 2);
    cmd_data(0x45, (uint8_t[]){0x00, 0x00, (EPD_HEIGHT - 1) & 0xFF, (EPD_HEIGHT - 1) >> 8}, 4);
    return wait_busy();
}

/* Reset the RAM address counter; it does not reliably wrap after a full write. */
static void set_cursor(void)
{
    cmd_data(0x4E, (uint8_t[]){0x00}, 1);
    cmd_data(0x4F, (uint8_t[]){0x00, 0x00}, 2);
}

esp_err_t epd_display(const uint8_t *fb)
{
    /* Mode 2 only drives pixels where new (0x24) != old (0x26), and the controller scrambles its
     * buffers after each update, so rewrite both every pass with old = inverse of new. */
    for (size_t i = 0; i < sizeof(old_ram); i++) {
        old_ram[i] = ~fb[i];
    }
    for (int pass = 0; pass < EPD_REFRESH_PASSES; pass++) {
        set_cursor();
        cmd(0x24);
        data(fb, EPD_STRIDE * EPD_HEIGHT);
        set_cursor();
        cmd(0x26);
        data(old_ram, sizeof(old_ram));
        cmd_data(0x22, (uint8_t[]){EPD_UPDATE_MODE2}, 1);
        cmd(0x20);
        esp_err_t err = wait_busy();
        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}

void epd_sleep(void)
{
    cmd_data(0x10, (uint8_t[]){0x01}, 1);
}
