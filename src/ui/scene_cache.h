#ifndef SCENE_CACHE_H
#define SCENE_CACHE_H

#include <M5Unified.h>
#include <stdint.h>

namespace SceneCache {

enum class Owner : uint8_t {
    NONE = 0,
    STREET,
    ROOM,
};

// One shared PSRAM canvas serves mutually-exclusive IDLE/HUNT and room modes.
// Allocation happens during display initialization, never in a render path.
bool init();
M5Canvas* rebuildTarget();
void commit(Owner owner, uint64_t key);
bool restore(M5Canvas& target, Owner owner, uint64_t key,
             int y = 0, int h = 240);
void invalidate();
void printStats();
void resetStats();

} // namespace SceneCache

#endif // SCENE_CACHE_H
