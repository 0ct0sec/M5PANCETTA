#pragma once
/**
 * platform.h — HAMLET CORE target detection + feature flags
 *
 * Two targets, both 320x240 / 8MB PSRAM / 16MB flash:
 *   M5Stack Core2 v1.1 — ESP32-D0WDQ6-V3. MPU6886 and vibration motor onboard.
 *   M5Stack CoreS3 SE  — ESP32-S3. No IMU, no motor, no internal battery.
 *                        An M5GO Bottom2 supplies all three (its IMU is the
 *                        same MPU6886 part Core2 carries onboard).
 *
 * These flags describe the BOARD. The IMU is the deliberate exception: a base
 * can be stacked or removed without reflashing, so HAMLET_HAS_IMU only says
 * "a build for this target may see an IMU". The authoritative check is the
 * runtime M5.Imu.isEnabled() probe cached in Pedometer.
 *
 * See docs/reference/cores3se-port.md.
 */

// ==[ TARGET ]== keyed off the board definition's own macro
#if defined(ARDUINO_M5STACK_CORES3)
  #define HAMLET_TARGET_CORE2     0
  #define HAMLET_TARGET_CORES3SE  1
#else
  #define HAMLET_TARGET_CORE2     1
  #define HAMLET_TARGET_CORES3SE  0
#endif

// ==[ DISPLAY ]== identical panel (320x240 ILI9342C) on both targets
#define HAMLET_DISPLAY_W      320
#define HAMLET_DISPLAY_H      240

// ==[ MEMORY ]== identical budget on both targets
#define HAMLET_PSRAM_MB       8
#define HAMLET_FLASH_MB       16

// ==[ CAPABILITIES ]== shared
#define HAMLET_HAS_TOUCH      1   // FT6336U capacitive on both
#define HAMLET_HAS_SD         1
#define HAMLET_HAS_RTC        1   // BM8563 on both
#define HAMLET_BLE_CLASSIC    1
#define HAMLET_HAS_MIC        1   // Core2 SPM1423 / CoreS3 SE ES7210

// ==[ CAPABILITIES ]== per target
#if HAMLET_TARGET_CORE2
  #define HAMLET_HAS_VIBRO    1   // AXP192 drives the motor
  // M5Unified registers the FT6336U interrupt on G39 as the sleep wake source.
  #define HAMLET_HAS_TOUCH_SLEEP_WAKE 1
  #define HAMLET_LED_PIN      25  // M5GO Bottom2 SK6812 strip
  // Core2 TF slot shares VSPI with the display. M5Unified owns bus setup.
  #define HAMLET_SD_SCLK_PIN  18
  #define HAMLET_SD_MOSI_PIN  23
  #define HAMLET_SD_MISO_PIN  38
  #define HAMLET_SD_CS_PIN    4
#else
  #define HAMLET_HAS_VIBRO    0   // CoreS3 SE has no motor at all
  // The pinned M5Unified board profile leaves the CoreS3 SE wake pin unset.
  // A zero-timer sleep therefore has no return path, so the UI must not offer
  // either sleep mode.
  #define HAMLET_HAS_TOUCH_SLEEP_WAKE 0
  // CoreS3/SE onboard TF slot. M5Unified's board pin table is the source:
  // sd_spi_sclk=G36, copi=G37, cipo=G35, cs=G4.
  #define HAMLET_SD_SCLK_PIN  36
  #define HAMLET_SD_MOSI_PIN  37
  #define HAMLET_SD_MISO_PIN  35
  #define HAMLET_SD_CS_PIN    4
  // HAMLET_LED_PIN is deliberately left undefined. GPIO25 does not exist on
  // the S3 (exposed: G0-G2, G5-G14, G17, G18, G35-G37, G43, G44) and the pin
  // the Bottom2 LED line reaches across M-BUS is unverified — the M-BUS GPIO
  // map moved between ESP32 and ESP32-S3. AmbientLED compiles to a no-op
  // until this is confirmed on hardware; define it here once it is.
#endif

// ==[ IMU ]== advisory only — Pedometer probes the bus at runtime
// Core2: MPU6886 onboard. CoreS3 SE: none onboard; M5GO Bottom2 adds MPU6886.
// Axis map (held landscape, USB-C left):
//   ax = RIGHT, ay = UP, az = TOWARD USER
//   upright  -> ay ~ +1g      flat face-up -> az ~ +1g
//   rollDeg  = atan2f(-ax, ay)   (ax negated: right bank must read positive)
//   pitchDeg = atan2f(az,  ay)
#define HAMLET_HAS_IMU        1
#define HAMLET_IMU_MPU6886    1

// ==[ BUTTONS ]==
#define HAMLET_BUTTON_COUNT   3   // virtual touch zones

// ==[ FEATURES ]==
#define HAMLET_FULL_FEATURES  1   // nuclear chain, narrator, extended trees, etc.

// ==[ PIXEL GRIDS ]==
#define HAMLET_PIG_PX         2   // fat-pixel grid for pig (72x42 body)
#define HAMLET_BIRD_PX        4   // fat-pixel grid for weather/scenery objects
