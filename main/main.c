#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "battery.h"
#include "board.h"
#include "console.h"
#include "display_model.h"
#include "inputs.h"
#include "persistent_store.h"
#include "state_machine.h"
#include "view_freshness.h"
#include "wake_reason.h"

void app_main(void)
{
    rf_switch_init();
    view_lock = xSemaphoreCreateMutex();
    nvs_open_state();
    battery_load();
    console_init();
    wake_cause_init();
    staleness_init();
    inputs_init();
    state_machine_run();
}
