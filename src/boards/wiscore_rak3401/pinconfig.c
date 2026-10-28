#include "boards.h"
#include "uf2/configkeys.h"
#include "nrf_gpio.h"

__attribute__((used, section(".bootloaderConfig")))
const uint32_t bootloaderConfig[] =
{
  /* CF2 START */
  CFG_MAGIC0, CFG_MAGIC1,                       // magic
  5, 100,                                       // used entries, total entries

  204, 0x100000,                                // FLASH_BYTES = 0x100000
  205, 0x40000,                                 // RAM_BYTES = 0x40000
  208, (USB_DESC_VID << 16) | USB_DESC_UF2_PID, // BOOTLOADER_BOARD_ID = USB VID+PID, used for verification when updating bootloader via uf2
  209, 0xada52840,                              // UF2_FAMILY = 0xada52840
  210, 0x20,                                    // PINS_PORT_SIZE = PA_32

  0, 0, 0, 0, 0, 0, 0, 0
  /* CF2 END */
};

//--------------------------------------------------------------------+
// LoRa-OTA bootloader rev 8 (2026-10-06): keep the WisMesh 1W booster OFF while the bootloader runs.
// The app switches it on (RAK3401Board::begin): WB_IO2 = Arduino pin 34 = P1.02 powers the 3V3_S rail AND the RAK13302's 5 V boost for
// the SKY66122 PA; IO3 = pin 21 = P0.21 = the SKY66122's CSD+CPS. Up to rev 7 the bootloader never drove them (inputs, floating) and
// reset every pin to an input before starting the app. A LiFePO4 solar bench board came back from `start ota` and from LoRa installs
// with reset reason "power on" (a brownout loses the DFU request in GPREGRET -> normal boot: "blue LED blinks, then it boots normally",
// 2026-10-04); a board on USB never did. Driven LOW here = FEM shut down (< 1 uA), boost and 3V3_S off.
//--------------------------------------------------------------------+
#define LORAOTA_FEM_EN_PIN   _PINNUM(0, 21)   // IO3: SKY66122 CSD + CPS
#define LORAOTA_3V3S_PIN     _PINNUM(1, 2)    // WB_IO2 (Arduino 34): 3V3_S rail + RAK13302 5 V boost

static void loraota_booster_off(void) {
  nrf_gpio_pin_clear(LORAOTA_FEM_EN_PIN);  nrf_gpio_cfg_output(LORAOTA_FEM_EN_PIN);    // level first, then output: no glitch HIGH
  nrf_gpio_pin_clear(LORAOTA_3V3S_PIN);    nrf_gpio_cfg_output(LORAOTA_3V3S_PIN);
}

void board_init2(void) { loraota_booster_off(); }       // as early as board_init() allows (before USB / BLE start)

void board_teardown2(void) { loraota_booster_off(); }   // after all pins went back to inputs: start the app with the booster still off
