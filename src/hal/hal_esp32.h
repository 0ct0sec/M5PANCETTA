/**
 * HAL ESP32 - Real hardware implementation
 *
 * ==[ THE REAL DEAL ]== wires HAL interface to Arduino/ESP32 APIs.
 * Created once in main.cpp, passed to init() functions.
 */
#pragma once

#include "hal_interface.h"
#include "../audio/sfx.h"
#include <Arduino.h>
#include <Preferences.h>
#include <esp_random.h>
#include <cstdarg>
#include <cstdio>

// Forward declarations for modules we bridge to
namespace Config {
    void addXP(uint32_t amount);
    bool isSessionActive();
    uint8_t getSessionPMKIDCount();
    uint8_t getSessionHSCount();
}

namespace Haptic {
    void buzz();
}

namespace DefhogTerminal {
    bool isVisible();
    void pushLineHype(const char* fmt, ...);
}

class HalESP32 : public HAL {
public:
    // --- Clock ---
    uint32_t millis() override {
        return ::millis();
    }

    // --- Random ---
    uint32_t random() override {
        return esp_random();
    }

    // --- Persistent Storage ---
    uint32_t storageGetUInt(const char* ns, const char* key, uint32_t defaultVal = 0) override {
        Preferences prefs;
        prefs.begin(ns, true);  // read-only
        uint32_t val = prefs.getUInt(key, defaultVal);
        prefs.end();
        return val;
    }

    Status storagePutUInt(const char* ns, const char* key, uint32_t val) override {
        Preferences prefs;
        prefs.begin(ns, false);
        size_t written = prefs.putUInt(key, val);
        prefs.end();
        return (written > 0) ? Status() : Status(HalError::STORAGE_WRITE_FAIL);
    }

    // --- Feedback ---
    void playSound(uint8_t soundId) override {
        SFX::play(static_cast<SFX::Event>(soundId));
    }

    void hapticBuzz() override {
        Haptic::buzz();
    }

    // --- Terminal ---
    bool terminalIsVisible() override {
        return DefhogTerminal::isVisible();
    }

    // Note: variadic forwarding to DefhogTerminal requires va_list version.
    // Callers that need terminal output should format into a buffer first,
    // then call terminalPush("%s", buf). Direct variadic forwarding loses
    // the va_list across the virtual dispatch boundary.
    void terminalPush(const char* fmt, ...) override {
        char buf[128];
        va_list args;
        va_start(args, fmt);
        vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);
        DefhogTerminal::pushLineHype("%s", buf);
    }

    // --- Config bridge ---
    void addXP(uint16_t amount) override {
        Config::addXP(amount);
    }

    bool isSessionActive() override {
        return Config::isSessionActive();
    }

    uint16_t getSessionPMKIDCount() override {
        return Config::getSessionPMKIDCount();
    }

    uint16_t getSessionHSCount() override {
        return Config::getSessionHSCount();
    }
};
