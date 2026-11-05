/* greenlight: a battery-powered e-paper display that shows whether the house is closed up ("Bezárva?"), fed by
 * Home Assistant over Zigbee. See docs/README.md. app_main() sets up the parts in order, then runs the state machine. */

#include "board/battery.h"
#include "board/board.h"
#include "board/inputs.h"
#include "board/wake_reason.h"
#include "console.h"
#include "display/display_model.h"
#include "display/view_freshness.h"
#include "persistent_store.h"
#include "state_machine.h"

void app_main(void)
{
    board_init_rf_switch();
    display_model_init();
    persistent_store_open();
    battery_init();
    console_init();
    wake_reason_init();
    view_freshness_init();
    inputs_init();
    state_machine_run();
}
