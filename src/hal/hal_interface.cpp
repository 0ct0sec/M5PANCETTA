/**
 * HAL Interface - Global accessor implementation
 *
 * ==[ PIG GLOVES ]== singleton bridge for gradual migration.
 * Set once at boot in main.cpp, consumed by any module that
 * receives hal=nullptr and needs to resolve its own HAL*.
 */
#include "hal_interface.h"

namespace HalGlobal {

static HAL* _instance = nullptr;

void set(HAL* instance) {
    _instance = instance;
}

HAL* get() {
    return _instance;
}

}  // namespace HalGlobal
