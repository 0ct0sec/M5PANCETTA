/**
 * HAMLET CORE - Main Entry Point
 *
 * The front door serves two witnesses: CoreS3 SE first, Core2 for compatibility.
 * Platform gates keep board-specific power, microphone, IMU, and bus work out
 * of the wrong boot. Hardware gets one sworn statement; the preprocessor keeps
 * the stories from crossing.
 */

#include <M5Unified.h>
#include <Wire.h>
#include "hamlet.h"
#include "audio/sfx.h"
#include "core/power.h"
#include "core/frame_budget.h"
#include "hal/hal_esp32.h"
#include "hal/platform.h"

static HalESP32 hardwareHal;

#if HAMLET_TARGET_CORES3SE
// CoreS3 SE has no battery, so M5Unified can conservatively refuse
// setExtOutput(true) while the core is powered from native USB. A stacked
// M003-V21 GPS is a 5V M-Bus load, though, and remains completely silent when
// BUS_OUT_EN is left low. This mirrors M5Unified's CoreS3 output sequence, but
// only for this explicitly batteryless target.
static bool ensureCoreS3SEMBusPower() {
    if (M5.Power.getExtOutput()) return true;

    static constexpr uint8_t AW9523_ADDR = 0x58;
    static constexpr uint8_t OUTPUT_REG = 0x02;
    static constexpr uint8_t BUS_OUT_EN = 0b00000010;
    static constexpr uint8_t BOOST_EN = 0b10000000;
    static constexpr uint32_t I2C_FREQ = 100000;

    uint8_t outputs[2];
    if (!M5.In_I2C.readRegister(AW9523_ADDR, OUTPUT_REG, outputs,
                                sizeof(outputs), I2C_FREQ)) {
        return false;
    }
    outputs[0] |= BUS_OUT_EN;
    outputs[1] |= BOOST_EN;
    if (!M5.In_I2C.writeRegister(AW9523_ADDR, OUTPUT_REG, outputs,
                                 sizeof(outputs), I2C_FREQ)) {
        return false;
    }

    delay(20);
    return M5.Power.getExtOutput();
}
#endif

void setup() {
    // ==[ EARLY SERIAL ]== debug boot issues
    delay(2000);
    Serial.begin(115200);
    delay(500);
    Serial.flush();

    // ==[ CORE2 POWER PRE-INIT ]== the two raw-I2C blocks below poke Core2's
    // AXP192 and an ATECC608A over the internal bus (SDA=21/SCL=22), pins that
    // do not exist on the ESP32-S3. On CoreS3 SE i2c_set_pin rejects them and
    // the half-configured bus wedges M5.begin()'s own AXP2101 bring-up, leaving
    // the display rail off (blank screen). CoreS3 SE has no ATECC and no motor,
    // and M5.begin() powers the S3 rails itself, so skip the Core2-only dance.
#if HAMLET_TARGET_CORE2
    // ==[ ATECC608A WORKAROUND ]== Core2 for AWS: sleep crypto chip before M5.begin()
    // ATECC608A at 0x35 is 1 bit from AXP192 at 0x34 — corrupts chip ID read,
    // causing M5GFX to misdetect board as TimerCam. no LCD power, no power key.
    Wire1.begin(21, 22, 400000);
    // sleep ATECC608A at both possible addresses
    Wire1.beginTransmission(0x60);
    Wire1.write(0x01);
    Wire1.endTransmission();
    Wire1.beginTransmission(0x35);
    Wire1.write(0x01);
    Wire1.endTransmission();
    // force AXP192 LDO2 on (LCD power) — insurance against autodetect failure
    Wire1.beginTransmission(0x34);
    Wire1.write(0x28);          // LDO2/LDO3 voltage register
    Wire1.write(0xF0);          // LDO2 = 3300mV
    Wire1.endTransmission();
    // read-modify-write output enable register: set LDO2 bit
    Wire1.beginTransmission(0x34);
    Wire1.write(0x12);
    Wire1.endTransmission(false);
    Wire1.requestFrom((uint8_t)0x34, (uint8_t)1);
    uint8_t reg12 = Wire1.read();
    Wire1.beginTransmission(0x34);
    Wire1.write(0x12);
    Wire1.write(reg12 | 0x04);  // bit 2 = LDO2 enable
    Wire1.endTransmission();
    Wire1.end();                // release for M5Unified to re-init

    // ==[ EARLY MOTOR KILL ]== if a crash rebooted us with DLDO1 still
    // energised, the motor buzzes until M5.Power.setVibration(0) runs.
    // Kill it immediately via raw I2C before the long init chain.
    {
        TwoWire i2c(0);
        i2c.begin(25, 32, 400000);
        i2c.beginTransmission(0x34);  // Core2 PMIC
        i2c.write(0x99);              // motor rail voltage register
        i2c.write(0x00);              // 0V = motor off
        i2c.endTransmission();
        i2c.end();
    }
#endif  // HAMLET_TARGET_CORE2

    // ==[ M5 INIT ]== let M5Unified bind the selected board's rails and buses
    auto cfg = M5.config();
    cfg.serial_baudrate = 115200;
    cfg.led_brightness = 128;
#if defined(HAMLET_CORE3SE)
    // Configure the CoreS3 SE's ES7210 path without opening it. BathMic owns
    // the actual begin/end lifecycle so no sound input exists outside the tub.
    cfg.internal_mic = true;
#else
    // GPIO34 is the Core2 PDM microphone data pin and also a supported GPS RX
    // route. Keep the dormant Core2 microphone from competing with that route.
    cfg.internal_mic = false;
#endif
#if HAMLET_TARGET_CORES3SE
    // CoreS3 SE has no onboard IMU. Defer the probe until Pedometer::init(),
    // after the M-Bus rails above are enabled, so a Bottom2 MPU6886 at 0x68
    // is alive before M5Unified scans the internal G12/G11 I2C bus.
    cfg.internal_imu = false;
#endif
    cfg.output_power = true;

    M5.begin(cfg);
#if HAMLET_TARGET_CORES3SE
    const bool mBusPowerReady = ensureCoreS3SEMBusPower();
    Serial.printf("[POWER] CoreS3SE M-Bus 5V %s\n",
                  mBusPowerReady ? "enabled" : "FAILED");
#endif
    M5.Power.setVibration(0);  // harmless on motorless CoreS3 SE

    // ==[ BEEP PIPE ]== spin up the speaker for capture chirps
    M5.Speaker.begin();
    M5.Speaker.setVolume(128);

    // ==[ HARDWARE BRIDGE ]== progression/storage modules need real iron.
    HalGlobal::set(&hardwareHal);

    // ==[ CORE BOOT ]== hand control to the Hamlet state machine
    Hamlet::init();
}

void loop() {
    M5.update();
    Hamlet::update();
    FrameBudget::endFrame();
    FrameBudget::paceFrame();
}
