#include "scene_cache.h"

#include <string.h>

namespace SceneCache {

namespace {

static constexpr int kWidth = 320;
static constexpr int kHeight = 240;

static M5Canvas* cache = nullptr;
static Owner cacheOwner = Owner::NONE;
static uint64_t cacheKey = 0;
static bool cacheValid = false;
static bool initialized = false;

struct CacheStats {
    uint32_t hits = 0;
    uint32_t misses = 0;
    uint32_t rebuilds = 0;
    uint32_t lastCopyUs = 0;
    uint32_t maxCopyUs = 0;
};

static CacheStats stats;

} // namespace

bool init() {
    if (initialized) return cache != nullptr;
    initialized = true;

    cache = new M5Canvas(&M5.Display);
    if (!cache) return false;
    cache->setPsram(true);
    cache->createSprite(kWidth, kHeight);
    if (cache->width() != kWidth || cache->height() != kHeight ||
        !cache->getBuffer()) {
        delete cache;
        cache = nullptr;
        return false;
    }
    return true;
}

M5Canvas* rebuildTarget() {
    if (!initialized) init();
    if (!cache) return nullptr;
    cacheValid = false;
    ++stats.misses;
    return cache;
}

void commit(Owner owner, uint64_t key) {
    if (!cache) return;
    cacheOwner = owner;
    cacheKey = key;
    cacheValid = true;
    ++stats.rebuilds;
}

bool restore(M5Canvas& target, Owner owner, uint64_t key, int y, int h) {
    if (!cacheValid || !cache || cacheOwner != owner || cacheKey != key ||
        target.width() != kWidth || target.height() != kHeight ||
        !target.getBuffer()) {
        return false;
    }

    if (y < 0) {
        h += y;
        y = 0;
    }
    if (y + h > kHeight) h = kHeight - y;
    if (h <= 0) return false;

    const uint32_t startUs = micros();
    uint16_t* dst = static_cast<uint16_t*>(target.getBuffer());
    const uint16_t* src = static_cast<const uint16_t*>(cache->getBuffer());
    memcpy(dst + y * kWidth, src + y * kWidth,
           static_cast<size_t>(kWidth * h) * sizeof(uint16_t));
    stats.lastCopyUs = micros() - startUs;
    if (stats.lastCopyUs > stats.maxCopyUs) stats.maxCopyUs = stats.lastCopyUs;
    ++stats.hits;
    return true;
}

void invalidate() {
    cacheValid = false;
    cacheOwner = Owner::NONE;
}

void printStats() {
    Serial.printf("[SCENECACHE] ready=%u valid=%u owner=%u hits=%lu misses=%lu "
                  "rebuilds=%lu copy=%luus maxcopy=%luus\n",
                  cache ? 1u : 0u,
                  cacheValid ? 1u : 0u,
                  static_cast<unsigned>(cacheOwner),
                  static_cast<unsigned long>(stats.hits),
                  static_cast<unsigned long>(stats.misses),
                  static_cast<unsigned long>(stats.rebuilds),
                  static_cast<unsigned long>(stats.lastCopyUs),
                  static_cast<unsigned long>(stats.maxCopyUs));
}

void resetStats() {
    stats = CacheStats{};
}

} // namespace SceneCache
