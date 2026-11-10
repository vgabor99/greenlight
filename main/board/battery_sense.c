#include <stdbool.h>
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "board/battery_sense.h"
#include "board/board.h"

static const char *TAG = "greenlight";

#define SAMPLES       16
#define DIVIDER_RATIO 2               /* equal resistors: VBAT = 2 x pin */
#define ATTENUATION   ADC_ATTEN_DB_12 /* about 0-3.1 V at the pin; a full cell gives 2.1 V */

static adc_oneshot_unit_handle_t adc_unit;
static adc_cali_handle_t         calibration;
static adc_channel_t             channel;
static SemaphoreHandle_t         lock;  /* the state machine and the console both measure */
static bool                      ready;

static bool set_up(void)
{
    lock = xSemaphoreCreateMutex();
    if (lock == NULL) {
        ESP_LOGE(TAG, "battery sense: no memory for the lock");
        return false;
    }
    adc_unit_t unit_id;
    esp_err_t  err = adc_oneshot_io_to_channel(BATTERY_SENSE_PIN, &unit_id, &channel);
    if (err == ESP_OK) {
        adc_oneshot_unit_init_cfg_t unit_config = {.unit_id = unit_id};
        err = adc_oneshot_new_unit(&unit_config, &adc_unit);
    }
    if (err == ESP_OK) {
        adc_oneshot_chan_cfg_t channel_config = {.atten = ATTENUATION, .bitwidth = ADC_BITWIDTH_DEFAULT};
        err = adc_oneshot_config_channel(adc_unit, channel, &channel_config);
    }
    if (err == ESP_OK) {
        adc_cali_curve_fitting_config_t cali_config = {
            .unit_id = unit_id, .chan = channel, .atten = ATTENUATION, .bitwidth = ADC_BITWIDTH_DEFAULT};
        err = adc_cali_create_scheme_curve_fitting(&cali_config, &calibration);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "battery sense: ADC setup failed (%s)", esp_err_to_name(err));
        return false;
    }
    return true;
}

uint16_t battery_sense_read_mv(bool log)
{
    static bool tried;  /* the first call comes before the console task: no race, and a failed setup is not retried */
    if (!tried) {
        tried = true;
        ready = set_up();
    }
    if (!ready) {
        return 0;
    }
    int mv, sum = 0, count = 0;
    xSemaphoreTake(lock, portMAX_DELAY);
    adc_oneshot_get_calibrated_result(adc_unit, calibration, channel, &mv);  /* first sample discarded */
    for (int i = 0; i < SAMPLES; i++) {
        if (adc_oneshot_get_calibrated_result(adc_unit, calibration, channel, &mv) == ESP_OK) {
            sum += mv;
            count++;
        }
    }
    xSemaphoreGive(lock);
    if (count == 0) {
        ESP_LOGE(TAG, "battery sense: ADC read failed");
        return 0;
    }
    int pin_mv = sum / count;
    if (log) {
        ESP_LOGI(TAG, "battery sense: pin %d mV, vbat %d mV (%d samples)", pin_mv, pin_mv * DIVIDER_RATIO, count);
    }
    return pin_mv * DIVIDER_RATIO;
}
