#include "tab5_board.h"

#if HAMLET_TARGET_TAB5

#include <M5Unified.h>
#include <WiFi.h>
#include "../util/debug_log.h"

namespace Tab5Board {

// PI4IOE5V6408-2 @ 0x44, P0 = WLAN_PWR_EN (active high).
static constexpr uint8_t kPi4ioe2Addr = 0x44;
static constexpr uint8_t kWlanPwrBit = 0x01;

static void enableC6Power() {
    // Best-effort: M5Unified may already have the expander up. If the write
    // fails the C6 may still be powered from factory firmware defaults.
    uint8_t output = 0;
    if (M5.In_I2C.readRegister(kPi4ioe2Addr, 0x05, &output, 1, 400000)) {
        output |= kWlanPwrBit;
        M5.In_I2C.writeRegister(kPi4ioe2Addr, 0x05, &output, 1, 400000);
    }
}

void begin() {
    enableC6Power();
    delay(50);
    WiFi.setPins(HAMLET_C6_SDIO_CLK, HAMLET_C6_SDIO_CMD,
                 HAMLET_C6_SDIO_D0, HAMLET_C6_SDIO_D1,
                 HAMLET_C6_SDIO_D2, HAMLET_C6_SDIO_D3,
                 HAMLET_C6_SDIO_RST);
    HAMLET_LOGLN("[TAB5] C6 SDIO pins armed; transmitters still follow settings warnings");
}

}  // namespace Tab5Board

#else

namespace Tab5Board {
void begin() {}
}

#endif
