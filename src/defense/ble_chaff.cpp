/**
 * BLE Chaff — Decoy Tracker Beacon Broadcaster
 *
 * ==[ SMOKESCREEN ]== 5 fake tracker types, rotated every 2.5s.
 * each cycle: stop adv → randomize payload → start adv.
 * NimBLE handles scan/adv interleaving via GAP scheduler.
 */

#include "ble_chaff.h"
#define DEFENSE_PIPELINE_INTERNAL 1
#include "recon.h"
#include "xband.h"
#undef DEFENSE_PIPELINE_INTERNAL
#include "../util/debug_log.h"

#if defined(RECON_BLE_SUPPORT) && RECON_BLE_SUPPORT && !defined(CONFIG_BT_NIMBLE_ROLE_BROADCASTER_DISABLED)
#define CHAFF_ENABLED 1
#include <NimBLEDevice.h>
#include <esp_random.h>
#else
#define CHAFF_ENABLED 0
#endif

namespace BLEChaff {

#if CHAFF_ENABLED

static bool active = false;
static uint32_t lastCycle = 0;
static uint32_t nextCycleJitterMs = 0;  // stochastic delta for the pending cycle
static uint8_t currentType = 0;
static constexpr uint32_t CYCLE_MS_NORMAL  = 2500;
static constexpr uint32_t CYCLE_MS_DESERT  = 5000;  // fewer emissions when alone
static constexpr uint32_t CYCLE_MS_CROWDED = 1500;  // faster blend in noise
// ==[ CYCLE JITTER ]== the fixed 2.5s period was trivially filterable: an
// observer with 10 scans could correlate dwell-time and strip decoys. We now
// perturb each cycle by ±40% uniform, so the payload-rotation clock looks
// stochastic instead of a metronome.
static constexpr uint32_t CYCLE_JITTER_PCT = 40;  // ±40% of base
static constexpr uint8_t TYPE_COUNT = 5;

// ==[ ENVIRONMENT-WEIGHTED TYPE SELECTION ]==
// map chaff types to Recon ThreatTypes for histogram matching
// 0=AirTag, 1=SmartTag, 2=Tile, 3=FastPair, 4=Flipper
static uint8_t selectNextType() {
    const Recon::TrackerEntry* devs = Recon::getBleDevices();
    int devCount = Recon::getBleDeviceTableSize();

    // count nearby devices matching each chaff type
    uint8_t weights[TYPE_COUNT] = {1, 1, 1, 1, 1};  // base weight 1 (uniform fallback)
    // BLE can stay alive even when PSRAM pressure left Recon without its
    // catalog. Keep chaff usable with the uniform fallback in that case.
    if (devs && devCount > 0) {
        for (int i = 0; i < devCount; i++) {
            switch (devs[i].type) {
                case Recon::ThreatType::AIRTAG:     weights[0] += 3; break;
                case Recon::ThreatType::SMARTTAG:
                case Recon::ThreatType::SMARTTAG_UNREGISTERED: weights[1] += 3; break;
                case Recon::ThreatType::TILE:        weights[2] += 3; break;
                case Recon::ThreatType::FAST_PAIR:
                case Recon::ThreatType::FMDN:        weights[3] += 3; break;
                case Recon::ThreatType::FLIPPER:     weights[4] += 3; break;
                default: break;
            }
        }
    }

    // weighted random: sum weights, pick a random point
    uint16_t total = 0;
    for (int i = 0; i < TYPE_COUNT; i++) total += weights[i];
    uint16_t r = esp_random() % total;
    uint16_t accum = 0;
    for (uint8_t i = 0; i < TYPE_COUNT; i++) {
        accum += weights[i];
        if (r < accum) return i;
    }
    return 0;  // fallback
}

static uint32_t getCycleMs() {
    XBand::CrowdTier tier = XBand::getCrowdTier();
    if (tier == XBand::CrowdTier::DESERTED) return CYCLE_MS_DESERT;
    if (tier >= XBand::CrowdTier::CROWDED)  return CYCLE_MS_CROWDED;
    return CYCLE_MS_NORMAL;
}

// Pick the duration of the next cycle, with uniform jitter around the base.
// ESP32 has no cheap log() for a true exponential draw; uniform is sufficient
// to defeat fixed-period dwell-time correlation.
static uint32_t pickNextCycleMs() {
    uint32_t base = getCycleMs();
    uint32_t span = (base * CYCLE_JITTER_PCT) / 100;   // ±40% of base
    uint32_t lo = (base > span) ? (base - span) : 1;
    uint32_t hi = base + span;
    return lo + (esp_random() % (hi - lo + 1));
}

// fill buffer with random bytes
static void fillRandom(uint8_t* buf, int len) {
    for (int i = 0; i < len; i += 4) {
        uint32_t r = esp_random();
        int n = (len - i < 4) ? (len - i) : 4;
        memcpy(buf + i, &r, n);
    }
}

static void buildPayload(NimBLEAdvertising* pAdv, uint8_t type) {
    NimBLEAdvertisementData advData;

    // flags make chaff harder to filter — real devices always have them
    // appearance adds device class credibility
    // 31-byte adv limit: budget per type carefully

    switch (type) {
        case 0: {
            // fake AirTag: flags(3) + mfg(28) = 31 bytes
            advData.setFlags(0x06);  // LE General + BR/EDR Not Supported
            uint8_t mfg[26];
            mfg[0] = 0x4C; mfg[1] = 0x00;  // Apple company ID
            mfg[2] = 0x12;                   // FindMy subtype
            mfg[3] = 0x19;                   // payload length
            fillRandom(mfg + 4, 22);          // rotating key (trimmed 1 byte for flags)
            advData.setManufacturerData(std::string((char*)mfg, sizeof(mfg)));
            break;
        }
        case 1: {
            // fake SmartTag: flags(3) + appearance(4) + svcdata(12) = 19 bytes
            advData.setFlags(0x06);
            advData.setAppearance(0x0200);  // TAG category
            uint8_t svc[8];
            fillRandom(svc, sizeof(svc));
            advData.setServiceData(NimBLEUUID((uint16_t)0xFD5A),
                std::string((char*)svc, sizeof(svc)));
            break;
        }
        case 2: {
            // fake Tile: flags(3) + appearance(4) + svcdata(10) + name(6) = 23 bytes
            advData.setFlags(0x06);
            advData.setAppearance(0x0200);  // TAG category
            advData.setName("Tile");
            uint8_t svc[6];
            fillRandom(svc, sizeof(svc));
            advData.setServiceData(NimBLEUUID((uint16_t)0xFEED),
                std::string((char*)svc, sizeof(svc)));
            break;
        }
        case 3: {
            // fake FastPair: flags(3) + appearance(4) + svcdata(7) = 14 bytes
            advData.setFlags(0x06);
            advData.setAppearance(0x0040);  // PHONE generic
            uint8_t svc[3];
            fillRandom(svc, sizeof(svc));
            advData.setServiceData(NimBLEUUID((uint16_t)0xFE2C),
                std::string((char*)svc, sizeof(svc)));
            break;
        }
        case 4: {
            // decoy Flipper: flags(3) + appearance(4) + mfg(12) = 19 bytes
            advData.setFlags(0x06);
            advData.setAppearance(0x0080);  // GENERIC
            uint8_t mfg[10];
            mfg[0] = 0x8F; mfg[1] = 0x03;  // Flipper company ID
            fillRandom(mfg + 2, 8);
            advData.setManufacturerData(std::string((char*)mfg, sizeof(mfg)));
            break;
        }
    }

    pAdv->setAdvertisementData(advData);
}

void start() {
    if (active || !Recon::isBleInitialized()) return;
    active = true;
    lastCycle = 0;  // force immediate first cycle
    nextCycleJitterMs = 0;
    currentType = 0;
    HAMLET_LOGLN("[CHAFF] smokescreen ON — broadcasting decoys");
}

void stop() {
    if (!active) return;
    NimBLEAdvertising* pAdv = NimBLEDevice::getAdvertising();
    if (pAdv->isAdvertising()) pAdv->stop();
    active = false;
    HAMLET_LOGLN("[CHAFF] smokescreen OFF");
}

void update(uint32_t now) {
    if (!active || !Recon::isBleInitialized()) return;
    // lastCycle=0 triggers an immediate first cycle on start(); thereafter
    // we gate on the jittered duration picked the last time we rotated.
    if (lastCycle != 0 && now - lastCycle < nextCycleJitterMs) return;

    NimBLEAdvertising* pAdv = NimBLEDevice::getAdvertising();
    if (pAdv->isAdvertising()) pAdv->stop();

    currentType = selectNextType();
    buildPayload(pAdv, currentType);
    pAdv->setMinInterval(0x20);  // 20ms
    pAdv->setMaxInterval(0x40);  // 40ms — fast rotation for visibility
    pAdv->start(0);              // indefinite until next cycle

    lastCycle = now;
    nextCycleJitterMs = pickNextCycleMs();  // commit next jittered window
}

bool isActive() { return active; }

#else  // !CHAFF_ENABLED

void start() {}
void stop() {}
void update(uint32_t) {}
bool isActive() { return false; }

#endif

}  // namespace BLEChaff

// ==[ FREE FUNCTION WRAPPERS ]== called from Recon via extern
void bleChaff_toggle() {
    if (BLEChaff::isActive()) BLEChaff::stop();
    else BLEChaff::start();
}
bool bleChaff_isActive() { return BLEChaff::isActive(); }
void bleChaff_update(uint32_t now) { BLEChaff::update(now); }
void bleChaff_stop() { BLEChaff::stop(); }
