#pragma once

/* Pin assignment: GPIO numbers on a Seeed XIAO ESP32-C6 (D-label in comment) */

/* The e-paper panel (epd.c) */
#define EPD_PIN_SCK  19  /* D8 */
#define EPD_PIN_MOSI 18  /* D10 */
#define EPD_PIN_CS   20  /* D9 */
#define EPD_PIN_DC   22  /* D4 */
#define EPD_PIN_RST  23  /* D5 */
#define EPD_PIN_BUSY 21  /* D3 */

/* "Somebody is around" input: Panasonic EKMB PIR output (active high, floats when idle; external 100k
 * pull-down to GND, which keeps the output current under the sensor's 100 uA). Set PIR_ACTIVE_LEVEL to 0
 * for a push button to GND instead: the pin then gets the internal pull-up. Deep-sleep wake pin. */
#define PIR_PIN          0   /* D0 */
#define PIR_ACTIVE_LEVEL 1

/* Pair button between this pin and GND (internal pull-up, active low); deep-sleep wake pin (GPIO0-7) */
#define PAIR_BUTTON_PIN          1  /* D1 */
#define PAIR_BUTTON_ACTIVE_LEVEL 0

/* Antenna switch: RF_SWITCH_ENABLE_PIN low enables it, RF_ANTENNA_SELECT_PIN low selects the onboard ceramic
 * antenna. */
#define RF_SWITCH_ENABLE_PIN  3
#define RF_ANTENNA_SELECT_PIN 14

/* Route the radio to the onboard antenna. */
void board_init_rf_switch(void);
