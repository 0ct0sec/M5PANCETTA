#pragma once
/**
 * platform.h — HAMLET CORE target detection + feature flags
 *
 * Three targets:
 *   M5Stack Core2 v1.1 — ESP32, 320x240, 8MB PSRAM, native Wi-Fi/BLE.
 *   M5Stack CoreS3 SE  — ESP32-S3, 320x240, 8MB PSRAM, native Wi-Fi/BLE.
 *   M5Stack Tab5       — ESP32-P4 + C6, 1280x720 logical landscape, 32MB PSRAM.
 *                        No native Wi-Fi on the P4; radio is a hosted C6.
 *
 * These flags describe the BOARD. The IMU is the deliberate exception: a base
 * can be stacked or removed without reflashing, so HAMLET_HAS_IMU only says
 * "a build for this target may see an IMU". The authoritative check is the
 * runtime M5.Imu.isEnabled() probe cached in Pedometer.
 */

// ==[ TARGET ]== keyed off explicit build flags, then board macros.
// Anything that is not CoreS3 or Tab5 stays Core2. Tab5 MUST be first so a
// P4 build is never treated as Core2 (AXP192 boot / 8MB PSRAM / wrong SD).
#if defined(HAMLET_TAB5) || defined(ARDUINO_M5STACK_TAB5) || defined(ARDUINO_M5STACK_TAB5_P4)
  #define HAMLET_TARGET_CORE2     0
  #define HAMLET_TARGET_CORES3SE  0
  #define HAMLET_TARGET_TAB5      1
#elif defined(ARDUINO_M5STACK_CORES3) || defined(HAMLET_CORE3SE)
  #define HAMLET_TARGET_CORE2     0
  #define HAMLET_TARGET_CORES3SE  1
  #define HAMLET_TARGET_TAB5      0
#else
  #define HAMLET_TARGET_CORE2     1
  #define HAMLET_TARGET_CORES3SE  0
  #define HAMLET_TARGET_TAB5      0
#endif

// House-map compositor: all six rooms on one landscape panel.
#define HAMLET_HOUSE_MAP          HAMLET_TARGET_TAB5

// ==[ DISPLAY ]== world/tile contract is always 320x240. Panel size lives
// in ui/display_profile.h. Do not change these macros for Tab5.
#define HAMLET_DISPLAY_W      320
#define HAMLET_DISPLAY_H      240

// ==[ MEMORY ]==
#if HAMLET_TARGET_TAB5
  #define HAMLET_PSRAM_MB       32
#else
  #define HAMLET_PSRAM_MB       8
#endif
#define HAMLET_FLASH_MB       16

// ==[ RADIO ]== hosted C6 still speaks the Wi-Fi/BLE APIs. Transmitters stay
// disarmed until the operator accepts the existing settings warning.
// CSI is an env-level capability (HAMLET_WIFI_CSI in platformio.ini), not a
// legal gate; the Tab5 env omits it because hosted C6 does not expose CSI.
#define HAMLET_HAS_NATIVE_WIFI   1
#define HAMLET_HAS_RAW_80211     1
#define HAMLET_HAS_PROMISCUOUS   1
#define HAMLET_HAS_ESPNOW        1
#define HAMLET_BLE_CLASSIC       1

// ==[ CAPABILITIES ]== shared
#define HAMLET_HAS_TOUCH      1
#define HAMLET_HAS_SD         1
#define HAMLET_HAS_RTC        1
#define HAMLET_HAS_MIC        1

// ==[ CAPABILITIES ]== per target
#if HAMLET_TARGET_TAB5
  #define HAMLET_HAS_VIBRO    0
  #define HAMLET_HAS_TOUCH_SLEEP_WAKE 0
  // Tab5 TF slot in SPI mode (docs): MISO G39, CS G42, SCK G43, MOSI G44.
  #define HAMLET_SD_SCLK_PIN  43
  #define HAMLET_SD_MOSI_PIN  44
  #define HAMLET_SD_MISO_PIN  39
  #define HAMLET_SD_CS_PIN    42
  // ESP32-C6 hosted SDIO (Wi-Fi coprocessor)
  #define HAMLET_C6_SDIO_CLK  12
  #define HAMLET_C6_SDIO_CMD  13
  #define HAMLET_C6_SDIO_D0   11
  #define HAMLET_C6_SDIO_D1   10
  #define HAMLET_C6_SDIO_D2   9
  #define HAMLET_C6_SDIO_D3   8
  #define HAMLET_C6_SDIO_RST  15
#elif HAMLET_TARGET_CORE2
  #define HAMLET_HAS_VIBRO    1
  #define HAMLET_HAS_TOUCH_SLEEP_WAKE 1
  #define HAMLET_LED_PIN      25
  #define HAMLET_SD_SCLK_PIN  18
  #define HAMLET_SD_MOSI_PIN  23
  #define HAMLET_SD_MISO_PIN  38
  #define HAMLET_SD_CS_PIN    4
#else
  #define HAMLET_HAS_VIBRO    0
  #define HAMLET_HAS_TOUCH_SLEEP_WAKE 0
  #define HAMLET_SD_SCLK_PIN  36
  #define HAMLET_SD_MOSI_PIN  37
  #define HAMLET_SD_MISO_PIN  35
  #define HAMLET_SD_CS_PIN    4
#endif

// ==[ IMU ]== advisory only — Pedometer probes the bus at runtime
#if HAMLET_TARGET_TAB5
  #define HAMLET_HAS_IMU        1
  #define HAMLET_IMU_BMI270     1
  #define HAMLET_IMU_MPU6886    0
#else
  #define HAMLET_HAS_IMU        1
  #define HAMLET_IMU_MPU6886    1
#endif

// ==[ BUTTONS ]==
#if HAMLET_TARGET_TAB5
  #define HAMLET_BUTTON_COUNT   0   // full capacitive panel; no Core virtual strip
#else
  #define HAMLET_BUTTON_COUNT   3
#endif

#define HAMLET_FULL_FEATURES  1

#define HAMLET_PIG_PX         2
#define HAMLET_BIRD_PX        4
