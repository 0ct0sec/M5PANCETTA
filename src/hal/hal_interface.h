/**
 * HAL Interface - Hardware Abstraction Layer
 *
 * ==[ PIG GLOVES ]== thin interface between logic and iron.
 * All testable modules receive a HAL* instead of calling Arduino/ESP32 directly.
 *
 * On real hardware: HalESP32 implements this with real APIs.
 * In tests: HalMock gives deterministic, controllable behavior.
 */
#pragma once

#include <cstdint>
#include <cstddef>

// ==[ RESULT TYPE ]== lightweight error propagation
// Use instead of bare bool for operations that can fail with a reason.
enum class HalError : uint8_t {
    OK = 0,
    BUFFER_TOO_SMALL,
    INVALID_ARGUMENT,
    STORAGE_WRITE_FAIL,
    SERIALIZE_FAIL,
};

template<typename T>
struct Result {
    T value;
    HalError error;

    Result(T val) : value(val), error(HalError::OK) {}
    Result(HalError err) : value{}, error(err) {}
    Result(T val, HalError err) : value(val), error(err) {}

    bool ok() const { return error == HalError::OK; }
    explicit operator bool() const { return ok(); }
};

// Specialization for void-like results
template<>
struct Result<void> {
    HalError error;

    Result() : error(HalError::OK) {}
    Result(HalError err) : error(err) {}

    bool ok() const { return error == HalError::OK; }
    explicit operator bool() const { return ok(); }
};

using Status = Result<void>;

// ==[ HAL INTERFACE ]== abstract base for hardware access
struct HAL {
    virtual ~HAL() = default;

    // --- Clock ---
    virtual uint32_t millis() = 0;

    // --- Random ---
    virtual uint32_t random() = 0;
    // Convenience: random in range [0, max)
    uint32_t random(uint32_t max) {
        return (max == 0) ? 0 : random() % max;
    }

    // --- Persistent Storage (NVS) ---
    virtual uint32_t storageGetUInt(const char* ns, const char* key, uint32_t defaultVal = 0) = 0;
    virtual Status   storagePutUInt(const char* ns, const char* key, uint32_t val) = 0;

    // --- Feedback ---
    virtual void playSound(uint8_t soundId) = 0;
    virtual void hapticBuzz() = 0;

    // --- Terminal (optional observer, null-safe) ---
    virtual bool   terminalIsVisible() { return false; }
    virtual void   terminalPush(const char* /*fmt*/, ...) {}

    // --- XP / Config (thin bridge — keeps Config namespace for now) ---
    virtual void   addXP(uint16_t amount) = 0;
    virtual bool   isSessionActive() = 0;
    virtual uint16_t getSessionPMKIDCount() = 0;
    virtual uint16_t getSessionHSCount() = 0;
};

// ==[ GLOBAL HAL ACCESSOR ]== set once at boot, used by modules
// This is the migration bridge: modules can call hal() while we
// transition from namespace globals to DI.
// In native tests, hal_mock.h provides inline implementations.
#ifndef HAL_GLOBAL_IMPL_DEFINED
namespace HalGlobal {
    void set(HAL* instance);
    HAL* get();
}
#endif
