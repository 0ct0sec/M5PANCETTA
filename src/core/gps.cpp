/**
 * GPS - Global NMEA feed manager
 *
 * ==[ SAT PIG ]== single TinyGPS++ instance on UART2.
 * always-on: UART stays hot across modes, status bar shows fix.
 * sleep: wardrive starts/stops UART on demand.
 */

#include "gps.h"
#include "config.h"
#include "../hal/platform.h"
#include "gps_policy.h"
#include "../util/debug_log.h"
#include <HardwareSerial.h>
#include <TinyGPSPlus.h>
#include <string.h>
#include <time.h>

namespace GPS {

// ==[ STATE ]==
static HardwareSerial gpsSerial(2);  // UART2
static TinyGPSPlus gps;
static bool initialized = false;
static float distanceKm = 0.0f;
static double lastLat = 0.0;
static double lastLon = 0.0;
static bool hasLastFix = false;

// ==[ FEED FRESHNESS ]== distinguish electrical data from checksum-valid NMEA
static uint32_t lastRawDataMs = 0;
static uint32_t lastValidNmeaMs = 0;
static bool rawDataSeen = false;
static bool validNmeaSeen = false;
static bool rawDataFresh = false;
static bool nmeaFresh = false;

// ==[ FEED DIAGNOSTICS ]== always compiled. HAMLET_DEBUG_LOG is off in
// production builds, so the serial GPS command is the only way to tell a dead
// pin from a wrong baud from a starved feed on a device that is already flashed.
static uint32_t bytesDrained = 0;
static uint16_t maxPendingBytes = 0;
static uint32_t drainCapHits = 0;
static constexpr size_t GPS_RAW_TAIL_BYTES = 96;
static uint8_t rawTail[GPS_RAW_TAIL_BYTES];
static size_t rawTailLen = 0;

// ==[ TRANSITION EDGE DETECTION ]==
static bool prevHadFix = false;
static bool prevNmeaFresh = false;
static bool nmeaDetectedOnce = false;   // latch: one toast per UART session
static bool flagFixAcquired = false;
static bool flagFixLost = false;
static bool flagNmeaDetected = false;

// ==[ UART SERVICE BUDGET ]==
// 115200 baud carries at most ~11.52 KB/s at 8N1. Background/critical-power
// profiles can service GPS at only 10 Hz, so a single 256-byte read per frame
// cannot keep up and eventually drops complete NMEA sentences. Drain in small
// stack chunks and keep enough ring-buffer headroom for the interval between
// slow frames.
//
// Average throughput is not the binding constraint — burst tolerance is. The
// driver ISR drops bytes silently when its ring buffer fills (CORE_DEBUG_LEVEL=0
// eats the warning), and a byte lost mid-sentence fails the checksum, so
// TinyGPS++ never commits the position. The symptom is a live feed that never
// locks: "DAT / NMEA BAD" forever. Two invariants prevent that:
//
//   1. The ring buffer must cover the longest frame stall. An AT6668 bursts a
//      multi-constellation NMEA set once per second, and a wardrive frame that
//      harvests 512 scan results or flushes the CSV to SD can hold the loop for
//      hundreds of ms. At 11.52 KB/s a 4 KB buffer is ~355 ms of slack.
//   2. One service pass must be able to empty a full buffer. A per-pass cap
//      below the buffer size leaves a permanent backlog after every stall, so
//      the next stall overflows immediately from an already-full buffer.
static constexpr size_t GPS_READ_CHUNK_BYTES = 512;
static constexpr size_t GPS_RX_BUFFER_BYTES = 8192;  // 8 KB + 128 B slack for ISR overhead
static constexpr size_t GPS_MAX_DRAIN_BYTES = GPS_RX_BUFFER_BYTES;
static constexpr uint32_t GPS_MAX_BAUD = 115200;
static constexpr uint32_t GPS_UART_BITS_PER_BYTE = 10;
static constexpr uint32_t GPS_MAX_BYTES_PER_SEC = GPS_MAX_BAUD / GPS_UART_BITS_PER_BYTE;
static constexpr uint32_t GPS_MIN_SERVICE_HZ = 10;
static constexpr uint32_t GPS_MIN_BURST_TOLERANCE_MS = 300;
static_assert(GPS_MAX_DRAIN_BYTES * GPS_MIN_SERVICE_HZ >= GPS_MAX_BYTES_PER_SEC,
              "GPS drain budget must keep up with the fastest configured UART");
static_assert(GPS_MAX_DRAIN_BYTES >= GPS_RX_BUFFER_BYTES,
              "one service pass must be able to empty a full ring buffer");
static_assert(GPS_RX_BUFFER_BYTES * 1000u / GPS_MAX_BYTES_PER_SEC >=
                  GPS_MIN_BURST_TOLERANCE_MS,
              "GPS ring buffer must cover the longest expected frame stall");

// ==[ HAVERSINE ]== double inputs avoid catastrophic cancellation on small deltas
static constexpr double DEG2RAD = M_PI / 180.0;
static float haversineKm(double lat1, double lon1, double lat2, double lon2) {
    double dLat = (lat2 - lat1) * DEG2RAD;
    double dLon = (lon2 - lon1) * DEG2RAD;
    double a = sin(dLat * 0.5) * sin(dLat * 0.5) +
               cos(lat1 * DEG2RAD) * cos(lat2 * DEG2RAD) *
               sin(dLon * 0.5) * sin(dLon * 0.5);
    return (float)(6371.0 * 2.0 * atan2(sqrt(a), sqrt(1.0 - a)));
}

// ==[ LIFECYCLE ]==

void init() {
    if (!Config::getGPSEnabled()) return;
    if (Config::getGPSAlwaysOn()) {
        startUART();
        HAMLET_LOGLN("[GPS] always-on — UART2 hot at boot");
    }
}

// Keep the newest bytes so a diagnostic can answer "is this NMEA text or
// wrong-baud garbage?" without a live echo saturating the serial console.
static void recordRawTail(const uint8_t* src, size_t len) {
    if (len >= GPS_RAW_TAIL_BYTES) {
        memcpy(rawTail, src + (len - GPS_RAW_TAIL_BYTES), GPS_RAW_TAIL_BYTES);
        rawTailLen = GPS_RAW_TAIL_BYTES;
        return;
    }
    if (rawTailLen + len > GPS_RAW_TAIL_BYTES) {
        size_t shift = rawTailLen + len - GPS_RAW_TAIL_BYTES;
        memmove(rawTail, rawTail + shift, rawTailLen - shift);
        rawTailLen -= shift;
    }
    memcpy(rawTail + rawTailLen, src, len);
    rawTailLen += len;
}

void service() {
    if (!initialized) return;

    // Bulk-read UART into a small stack buffer. HardwareSerial::read() is
    // non-blocking; readBytes() can wait for the Stream timeout. The pass
    // drains everything pending — see the service-budget invariants above for
    // why a partial drain is worse than the encode cost of a full one.
    uint8_t buf[GPS_READ_CHUNK_BYTES];
    size_t drained = 0;
    uint32_t passedBefore = gps.passedChecksum();
    int avail = gpsSerial.available();
    if (avail > 0 && (uint32_t)avail > maxPendingBytes) {
        maxPendingBytes = (avail > (int)UINT16_MAX) ? UINT16_MAX : (uint16_t)avail;
    }
    while (avail > 0 && drained < GPS_MAX_DRAIN_BYTES) {
        size_t n = (size_t)avail;
        if (n > sizeof(buf)) n = sizeof(buf);
        size_t remaining = GPS_MAX_DRAIN_BYTES - drained;
        if (n > remaining) n = remaining;

        size_t got = gpsSerial.read(buf, n);
        if (got == 0) break;
        for (size_t i = 0; i < got; i++) {
            gps.encode((char)buf[i]);
        }
        recordRawTail(buf, got);
        drained += got;
        avail = gpsSerial.available();
    }
    bytesDrained += drained;
    // Hitting the cap with bytes still queued means the feed outran the loop —
    // the ring buffer was already at risk of dropping bytes upstream of us.
    if (drained >= GPS_MAX_DRAIN_BYTES && avail > 0) drainCapHits++;

    // Fresh UART bytes prove the electrical route is active. Only a checksum
    // pass proves that those bytes form NMEA; wrong-baud garbage must not light
    // the GPS/NMEA status.
    uint32_t nowMs = millis();
    if (drained > 0) {
        lastRawDataMs = nowMs;
        rawDataSeen = true;
    }
    if (gps.passedChecksum() != passedBefore) {
        lastValidNmeaMs = nowMs;
        validNmeaSeen = true;
    }
    rawDataFresh = GPSPolicy::isFeedFresh(rawDataSeen, nowMs, lastRawDataMs);
    nmeaFresh = GPSPolicy::isFeedFresh(validNmeaSeen, nowMs, lastValidNmeaMs);
}

void update() {
    if (!initialized) return;

    service();

    // accumulate haversine distance (filter jitter: >1 km/h)
    if (gps.location.isUpdated() && gps.location.isValid() && gps.location.age() < 3000 &&
        gps.speed.isValid() && gps.speed.kmph() > 1.0) {
        double lat = gps.location.lat();
        double lon = gps.location.lng();
        if (hasLastFix) {
            distanceKm += haversineKm(lastLat, lastLon, lat, lon);
        }
        lastLat = lat;
        lastLon = lon;
        hasLastFix = true;
    }

    // ==[ EDGE DETECT ]== fire transition flags
    bool fixNow = gps.location.isValid() && gps.location.age() < 3000;
    if (fixNow && !prevHadFix)  flagFixAcquired = true;
    if (!fixNow && prevHadFix)  flagFixLost = true;
    if (nmeaFresh && !prevNmeaFresh && !nmeaDetectedOnce) {
        flagNmeaDetected = true;
        nmeaDetectedOnce = true;
    }
    prevHadFix = fixNow;
    prevNmeaFresh = nmeaFresh;
}

void deinit() {
    // don't kill UART if always-on
    if (Config::getGPSEnabled() && Config::getGPSAlwaysOn()) return;
    stopUART();
}

// ==[ CONTROL ]==

void startUART() {
    if (initialized) return;
#if !HAMLET_TARGET_CORES3SE
    if (Config::getC5Enabled()) {
        HAMLET_LOGLN("[GPS] UART2 held by C5 bridge");
        return;
    }
#endif
    // A stopped UART must not resurrect the previous session's fix/satellites.
    gps = TinyGPSPlus();
    gpsSerial.setRxBufferSize(GPS_RX_BUFFER_BYTES);
    gpsSerial.begin(Config::getGPSBaud(), SERIAL_8N1,
                    Config::getGPSRxPin(), Config::getGPSTxPin());
    initialized = true;
    lastRawDataMs = 0;
    lastValidNmeaMs = 0;
    rawDataSeen = false;
    validNmeaSeen = false;
    rawDataFresh = false;
    nmeaFresh = false;
    prevHadFix = false;
    prevNmeaFresh = false;
    nmeaDetectedOnce = false;
    flagFixAcquired = false;
    flagFixLost = false;
    flagNmeaDetected = false;
    resetDiagnostics();
    HAMLET_LOGF("[GPS] UART2 rx=%d tx=%d baud=%lu\n",
                  Config::getGPSRxPin(), Config::getGPSTxPin(),
                  (unsigned long)Config::getGPSBaud());
}

void stopUART() {
    if (!initialized) return;
    gpsSerial.end();
    initialized = false;
    hasLastFix = false;
    lastRawDataMs = 0;
    lastValidNmeaMs = 0;
    rawDataSeen = false;
    validNmeaSeen = false;
    rawDataFresh = false;
    nmeaFresh = false;
    prevHadFix = false;
    prevNmeaFresh = false;
    nmeaDetectedOnce = false;
    flagFixAcquired = false;
    flagFixLost = false;
    flagNmeaDetected = false;
}

bool isInitialized() { return initialized; }

// ==[ STATUS ]==

bool hasNMEA() {
    return initialized && nmeaFresh;
}

bool hasUARTData() {
    return initialized && rawDataFresh;
}

bool hasFix() {
    if (!initialized) return false;
    return gps.location.isValid() && gps.location.age() < 3000;
}

uint8_t getSatCount() {
    if (!initialized || !gps.satellites.isValid()) return 0;
    uint32_t count = gps.satellites.value();
    return (uint8_t)(count > UINT8_MAX ? UINT8_MAX : count);
}

// ==[ POSITION ]==

double getLatitude() {
    if (!hasFix()) return 0.0;
    return gps.location.lat();
}

double getLongitude() {
    if (!hasFix()) return 0.0;
    return gps.location.lng();
}

float getAltitude() {
    if (!hasFix() || !gps.altitude.isValid()) return 0.0f;
    return (float)gps.altitude.meters();
}

float getSpeedKmh() {
    if (!hasFix() || !gps.speed.isValid()) return 0.0f;
    return (float)gps.speed.kmph();
}

bool hasCourse() {
    return hasFix() && gps.course.isValid();
}

float getCourseDeg() {
    if (!hasCourse()) return 0.0f;
    return (float)gps.course.deg();
}

float getHdop() {
    if (!hasFix() || !gps.hdop.isValid()) return 0.0f;
    return (float)gps.hdop.hdop();
}

uint32_t getFixAgeMs() {
    if (!initialized || !gps.location.isValid()) return UINT32_MAX;
    return gps.location.age();
}

uint32_t getEpochUtc() {
    if (!initialized || !hasFix()) return 0;
    if (!gps.date.isValid() || !gps.time.isValid()) return 0;
    if (gps.date.year() < 2024) return 0;

    struct tm t = {};
    t.tm_year = (int)gps.date.year() - 1900;
    t.tm_mon = (int)gps.date.month() - 1;
    t.tm_mday = (int)gps.date.day();
    t.tm_hour = (int)gps.time.hour();
    t.tm_min = (int)gps.time.minute();
    t.tm_sec = (int)gps.time.second();
    t.tm_isdst = 0;
    time_t epoch = mktime(&t);
    if (epoch <= 0) return 0;
    return (uint32_t)epoch;
}

// ==[ DISTANCE ]==

void resetDistance() {
    distanceKm = 0.0f;
    hasLastFix = false;
}

float getDistanceKm() {
    return distanceKm;
}

// ==[ TRANSITIONS ]==

bool consumeFixAcquired() {
    if (!flagFixAcquired) return false;
    flagFixAcquired = false;
    return true;
}

bool consumeFixLost() {
    if (!flagFixLost) return false;
    flagFixLost = false;
    return true;
}

bool consumeNmeaDetected() {
    if (!flagNmeaDetected) return false;
    flagNmeaDetected = false;
    return true;
}

// ==[ DIAGNOSTICS ]==
// stopUART() deliberately leaves these standing so the counters still describe
// the session that just ended; startUART() clears them for the new one.

void resetDiagnostics() {
    bytesDrained = 0;
    maxPendingBytes = 0;
    drainCapHits = 0;
    rawTailLen = 0;
}

void getDiagnostics(Diagnostics& out) {
    out.enabled = Config::getGPSEnabled();
    out.alwaysOn = Config::getGPSAlwaysOn();
    out.initialized = initialized;
    out.rxPin = Config::getGPSRxPin();
    out.txPin = Config::getGPSTxPin();
    out.rxDip = GPSPolicy::rxDipSwitch(out.rxPin);
    out.txDip = GPSPolicy::txDipSwitch(out.txPin);
    out.baud = Config::getGPSBaud();
    out.rxBufferBytes = GPS_RX_BUFFER_BYTES;

    out.charsProcessed = gps.charsProcessed();
    out.passedChecksum = gps.passedChecksum();
    out.failedChecksum = gps.failedChecksum();
    out.sentencesWithFix = gps.sentencesWithFix();
    out.bytesDrained = bytesDrained;
    out.maxPendingBytes = maxPendingBytes;
    out.drainCapHits = drainCapHits;

    uint32_t nowMs = millis();
    out.lastRawAgeMs = rawDataSeen ? (nowMs - lastRawDataMs) : UINT32_MAX;
    out.lastNmeaAgeMs = validNmeaSeen ? (nowMs - lastValidNmeaMs) : UINT32_MAX;
    out.rawFresh = rawDataFresh;
    out.nmeaFresh = nmeaFresh;

    out.fix = hasFix();
    out.sats = getSatCount();
    out.fixAgeMs = getFixAgeMs();
    out.lat = out.fix ? gps.location.lat() : 0.0;
    out.lon = out.fix ? gps.location.lng() : 0.0;
    out.hdop = gps.hdop.isValid() ? (float)gps.hdop.hdop() : 0.0f;
}

size_t getRawTail(char* out, size_t cap) {
    if (!out || cap == 0) return 0;
    size_t n = rawTailLen;
    if (n > cap - 1) n = cap - 1;
    // Newest bytes are the interesting ones when the tail is clipped.
    const uint8_t* src = rawTail + (rawTailLen - n);
    for (size_t i = 0; i < n; i++) {
        uint8_t c = src[i];
        out[i] = (c >= 32 && c <= 126) ? (char)c : '.';
    }
    out[n] = '\0';
    return n;
}

} // namespace GPS
