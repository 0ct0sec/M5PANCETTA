/**
 * GPS - Global NMEA feed manager
 *
 * ==[ SAT PIG ]== UART2 TinyGPS++ instance shared by wardrive + status bar.
 * always-on mode keeps fix alive across all modes. sleep mode = wardrive only.
 */
#pragma once

#include <Arduino.h>

namespace GPS {
    // ==[ LIFECYCLE ]==
    void init();          // call at boot — starts UART if always-on
    void update();        // call every loop — feed NMEA bytes
    void deinit();        // tear down UART (wardrive exit when not always-on)

    // ==[ SERVICE ]== drain + parse only, no per-frame bookkeeping.
    // Call from inside work that can hold the loop for >100ms (scan harvest, SD
    // flush) so the driver ring buffer cannot overflow mid-sentence. A dropped
    // byte fails the checksum, and a feed that never passes a checksum never
    // produces a fix. update() calls this itself; extra calls are harmless.
    void service();

    // ==[ CONTROL ]==
    void startUART();     // init UART2 with current config pins/baud
    void stopUART();      // end UART2
    bool isInitialized(); // UART is running and feeding

    // ==[ STATUS ]==
    bool hasUARTData();   // fresh bytes on configured RX, checksum not implied
    bool hasNMEA();       // fresh checksum-valid NMEA sentences
    bool hasFix();        // valid location with age < 3s
    uint8_t getSatCount();

    // ==[ TRANSITIONS ]== consumable edge flags (cleared on read)
    bool consumeFixAcquired();
    bool consumeFixLost();
    bool consumeNmeaDetected();

    // ==[ POSITION ]==
    double getLatitude();
    double getLongitude();
    float getAltitude();
    float getSpeedKmh();
    bool hasCourse();
    float getCourseDeg();   // course over ground, not handset heading
    float getHdop();        // horizontal dilution of precision (0 = unknown)
    uint32_t getFixAgeMs();
    uint32_t getEpochUtc(); // Unix epoch from NMEA date/time, 0 if unavailable

    // ==[ DISTANCE TRACKING ]== haversine accumulator (wardrive uses this)
    void resetDistance();
    float getDistanceKm();

    // ==[ DIAGNOSTICS ]== always compiled — HAMLET_DEBUG_LOG is off in
    // production, so this is the only feed telemetry a flashed device can give.
    // Read it with the `GPS` serial command. Decision tree:
    //   charsProcessed == 0                  -> wrong RX pin / module unpowered
    //   chars > 0, passedChecksum == 0        -> wrong baud (or a shredded feed)
    //   passed > 0, sentencesWithFix == 0     -> parsing fine, no sky/cold start
    //   drainCapHits > 0 or maxPending ~= buf -> loop starved the UART
    struct Diagnostics {
        bool enabled;
        bool alwaysOn;
        bool initialized;
        uint8_t rxPin;
        uint8_t txPin;
        uint8_t rxDip;              // 0 = not an M003-V21 DIP route
        uint8_t txDip;
        uint32_t baud;
        uint32_t rxBufferBytes;
        uint32_t charsProcessed;    // bytes fed to TinyGPS++
        uint32_t passedChecksum;    // valid NMEA sentences
        uint32_t failedChecksum;    // corrupt/garbage sentences
        uint32_t sentencesWithFix;
        uint32_t bytesDrained;      // bytes pulled off UART this session
        uint16_t maxPendingBytes;   // peak backlog seen at service entry
        uint32_t drainCapHits;      // passes that hit the cap with bytes left
        uint32_t lastRawAgeMs;      // UINT32_MAX = never seen
        uint32_t lastNmeaAgeMs;
        bool rawFresh;
        bool nmeaFresh;
        bool fix;
        uint8_t sats;
        uint32_t fixAgeMs;
        double lat;
        double lon;
        float hdop;
    };
    void getDiagnostics(Diagnostics& out);
    void resetDiagnostics();
    size_t getRawTail(char* out, size_t cap);  // newest bytes, non-printable -> '.'
}
