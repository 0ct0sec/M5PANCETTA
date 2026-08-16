/**
 * C5Monster UART Bridge — Serial communication with ESP32-C5 + JanOS
 *
 * ==[ DUAL-BAND BRIDGE ]== Non-blocking UART bridge. Parses JanOS CLI responses
 * and populates 5GHz scan data for spectrum, hunt, wardrive, and recon.
 *
 * Pin mapping (Core2 Grove Port A, UART2 shared with local GPS):
 *   G33 = Serial2 RX (C5Monster TX → Core2)
 *   G32 = Serial2 TX (Core2 → C5Monster RX)
 * CoreS3 SE uses UART1 on G44/G43 so GPS can remain on UART2.
 */
#pragma once

#include <Arduino.h>
#include <HardwareSerial.h>
#include "c5monster_protocol.h"
#include "c5monster_observer_math.h"

namespace C5Monster {

// ==[ STATUS ]==
enum class Status : uint8_t {
    DISCONNECTED,   // UART not initialized or no response
    CONNECTED,      // Responded to probe, ready
    BUSY,           // Command in progress
    ERROR           // Last command failed
};

enum class Operation : uint8_t {
    NONE,
    CHANNEL_VIEW,
    PACKET_MONITOR,
    DEAUTH_DETECTOR,
    GPS_RAW,
    CONTINUOUS_OTHER
};

// ==[ SCAN RESULT BUFFER ]==
struct ScanResults {
    C5Protocol::ScanEntry entries[C5Protocol::MAX_SCAN_ENTRIES];
    uint8_t count;
    uint32_t timestampMs;  // when this completed result set was published
    uint32_t revision;     // increments for every completed set, including empty
};

// ==[ GPS FIX ]== parsed from checksum-valid JanOS "[GPS RAW] $..RMC" lines
struct GPSFix {
    double latitude;
    double longitude;
    float speedKmh;
    uint32_t timestampMs;
    uint32_t revision;
};

// Optional targeted evidence published by newer JanOS builds. Older firmware
// simply leaves this empty and callers retain scan-snapshot behavior.
struct TargetObservationTelemetry {
    C5Protocol::TargetObservation observation;
    uint32_t receivedAtMs;
    uint32_t receivedAtUs;
    uint32_t ageMs;
    uint32_t revision;
};

// ==[ INIT / LIFECYCLE ]==
bool begin(uint8_t rxPin, uint8_t txPin, uint32_t baud = 115200);
void stop();
void service();  // non-blocking RX parse, call every frame
void maintainConnection(uint32_t now);  // non-blocking startup/retry, call every frame

// ==[ PROBE ]==
bool probe();          // Blocking manual resync + ping
Status getStatus();
bool isConnected();
uint32_t getLastActivityMs();

// ==[ COMMAND INTERFACE ]==
bool sendCommand(const char* cmd, uint32_t timeoutMs = 3000);
bool sendCommandExpect(const char* cmd, const char* expected, uint32_t timeoutMs = 5000);
bool sendCommandThen(const char* first, const char* second, uint32_t timeoutMs = 3000);
bool isBusy();
bool hasActiveOperation();
Operation getActiveOperation();
void emergencyStop();  // Send "stop" immediately

// ==[ 5GHz SCAN DATA ]==
const ScanResults& getScanResults();
bool hasNewResults(uint32_t sinceMs);
uint8_t get5GHzNetworkCount();
int8_t get5GHzChannelPeakRSSI(uint8_t channel);  // 36-165
uint16_t get5GHzChannelNetworkCount(uint8_t channel);

// ==[ C5 GPS DATA ]==
const GPSFix& getGPSFix();
bool hasFreshGPSFix(uint32_t maxAgeMs = 18000);
uint32_t getGPSFixAgeMs();

// ==[ PASSIVE OBSERVER DATA ]==
// channel_view is a network census, not airtime utilization.
static constexpr uint8_t PACKET_MONITOR_HISTORY_CAPACITY =
    C5ObserverMath::PACKET_RATE_HISTORY_CAPACITY;

struct ChannelSurveyTelemetry {
    uint32_t ageMs;
    uint32_t revision;
    bool capturing;
    bool hasCompletedSample;
};

struct PacketMonitorTelemetry {
    uint8_t channel;
    uint32_t packetsPerSecond;
    uint32_t ageMs;
    uint32_t revision;
    uint16_t history[PACKET_MONITOR_HISTORY_CAPACITY];
    uint8_t historyCount;
};

uint16_t getObservedChannelNetworkCount(uint8_t channel);
bool isChannelSurveyFresh(uint32_t maxAgeMs = 30000);
bool getChannelSurveyTelemetry(ChannelSurveyTelemetry& telemetry);
bool getPacketMonitorSample(uint8_t& channel, uint32_t& packetsPerSecond,
                            uint32_t& ageMs);
bool getPacketMonitorTelemetry(PacketMonitorTelemetry& telemetry);
bool getTargetObservation(TargetObservationTelemetry& telemetry);

// ==[ RAW OUTPUT LOG ]== for C5Monster menu display
static constexpr uint8_t OUTPUT_LOG_LINES = 32;
static constexpr uint8_t OUTPUT_LINE_LEN = 128;
struct OutputLog {
    char lines[OUTPUT_LOG_LINES][OUTPUT_LINE_LEN];
    uint8_t head;     // next write index
    uint8_t count;    // how many valid lines
};
const OutputLog& getOutputLog();
void clearOutputLog();

// ==[ CONFIGURATION ]==
void setRxPin(uint8_t pin);
void setTxPin(uint8_t pin);
void setBaudRate(uint32_t baud);
uint8_t getRxPin();
uint8_t getTxPin();

} // namespace C5Monster
