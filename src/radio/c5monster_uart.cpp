/**
 * C5Monster UART Bridge — Implementation
 *
 * ==[ DUAL-BAND BRIDGE ]== Non-blocking UART bridge. Parses JanOS CLI responses
 * and populates 5GHz scan data for spectrum, hunt, wardrive, and recon.
 */

#include "c5monster_uart.h"
#include "c5monster_gps_math.h"
#include "c5monster_observer_math.h"
#include "c5monster_scan_math.h"
#include "c5_uart_policy.h"
#include "../hal/platform.h"
#if !HAMLET_TARGET_CORES3SE
#include "../core/gps.h"
#endif
#include "../util/debug_log.h"
#include "../util/psram_block.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <limits.h>

namespace C5Monster {

// ==[ UART ]==
static HardwareSerial c5Serial(C5UartPolicy::UART_NUM);
static Status status = Status::DISCONNECTED;
static bool serialStarted = false;

enum class ConnectPhase : uint8_t {
    IDLE,
    STARTUP_WAIT,
    STOPPING,
    PINGING,
    RETRY_WAIT
};
static ConnectPhase connectPhase = ConnectPhase::IDLE;
static uint32_t nextConnectAttemptMs = 0;
static constexpr uint32_t CONNECT_STARTUP_GRACE_MS = 5000;
static constexpr uint32_t CONNECT_RETRY_MS = 5000;
static constexpr uint32_t CONNECT_STOP_TIMEOUT_MS = 3000;
static constexpr uint32_t CONNECT_PING_TIMEOUT_MS = 2000;

// ==[ PIN CONFIG ]==
// Core2 Grove: G33=RX, G32=TX (UART2); G16/G17 are PSRAM.
// CoreS3 SE M-Bus: G44=RXD0, G43=TXD0 (UART1) — GPS owns G18/G17 (UART2)
static uint8_t rxPin = C5UartPolicy::RX_PIN;
static uint8_t txPin = C5UartPolicy::TX_PIN;
static constexpr uint8_t uartNum = C5UartPolicy::UART_NUM;
static uint32_t baudRate = C5UartPolicy::DEFAULT_BAUD;

// ==[ RX STATE MACHINE ]==
static uint32_t lastActivityMs = 0;
static uint32_t lastCommandMs = 0;
static uint32_t commandTimeoutMs = 3000;
static bool awaitingResponse = false;
static bool scanResultsCapturing = false;
static bool backgroundScanActive = false;
static uint32_t backgroundScanStartedMs = 0;

// ==[ COMMAND RESPONSE MATCHING ]==
static char lastCommand[128] = {0};
static char expectedResponse[64] = {0};
static bool expectedResponseSeen = false;
static bool commandSuccess = false;
static bool commandComplete = false;

// ==[ SCAN RESULTS ]==
static ScanResults scanResults = {};
static ScanResults* pendingScanResults = nullptr;
static GPSFix gpsFix = {};
static TargetObservationTelemetry targetObservation = {};

// ==[ OUTPUT LOG ]==
static OutputLog* outputLog = nullptr;
static char currentLine[OUTPUT_LINE_LEN + 1] = {};
static size_t currentLineLen = 0;

// ==[ CONTINUOUS OBSERVER STATE ]==
static constexpr uint8_t MAX_OBSERVER_CHANNEL = 196;
struct ObserverState {
    uint16_t channelNetworks[MAX_OBSERVER_CHANNEL + 1];
    uint32_t channelSurveyTimestampMs;
    uint32_t channelSurveyRevision;
    bool channelSurveyCapturing;
    C5ObserverMath::PacketRateHistory packetHistory;
    uint8_t packetChannel;
    uint32_t packetPps;
    uint32_t packetTimestampMs;
    uint32_t packetRevision;
};
static ObserverState* observerState = nullptr;
static Operation activeOperation = Operation::NONE;

// ==[ TWO-STEP COMMAND SEQUENCING ]==
// Targeted JanOS operations require select_networks to finish before the
// action command is written. One fixed buffer avoids heap work in service().
static char deferredCommand[128] = {};
static uint32_t deferredCommandTimeoutMs = 3000;

// ==[ INTERNAL HELPERS ]==

static const char* trimLeadingWs(const char* p) {
    while (p && isspace((unsigned char)*p)) p++;
    return p;
}

static bool ensurePendingScanResults() {
    if (!pendingScanResults) {
        pendingScanResults = PsramBlock::allocFor<ScanResults>();
    }
    return pendingScanResults != nullptr;
}

static bool ensureOutputLog() {
    if (!outputLog) outputLog = PsramBlock::allocFor<OutputLog>();
    return outputLog != nullptr;
}

static bool ensureObserverState() {
    if (!observerState) observerState = PsramBlock::allocFor<ObserverState>();
    return observerState != nullptr;
}

static void resetCommandState() {
    lastCommandMs = 0;
    commandTimeoutMs = 3000;
    awaitingResponse = false;
    lastCommand[0] = '\0';
    expectedResponse[0] = '\0';
    expectedResponseSeen = false;
    commandSuccess = false;
    commandComplete = true;
}

static bool timeReached(uint32_t now, uint32_t deadline) {
    return static_cast<int32_t>(now - deadline) >= 0;
}

static void scheduleConnectRetry(uint32_t now) {
    connectPhase = ConnectPhase::RETRY_WAIT;
    nextConnectAttemptMs = now + CONNECT_RETRY_MS;
}

static bool sendAsyncProbePing() {
    if (!sendCommand(C5Protocol::CMD_PING, CONNECT_PING_TIMEOUT_MS)) {
        return false;
    }
    // Require JanOS's liveness payload as well as its trailing prompt. This is
    // set after sendCommand() writes; parsing only occurs from service().
    strncpy(expectedResponse, "pong", sizeof(expectedResponse) - 1);
    expectedResponse[sizeof(expectedResponse) - 1] = '\0';
    expectedResponseSeen = false;
    return true;
}

static bool isStopCommand(const char* cmd) {
    return C5ObserverMath::commandEquals(cmd, C5Protocol::CMD_STOP);
}

static Operation classifyOperation(const char* cmd) {
    if (!cmd) return Operation::NONE;
    if (C5ObserverMath::commandEquals(cmd, C5Protocol::CMD_CHANNEL_VIEW)) {
        return Operation::CHANNEL_VIEW;
    }
    if (C5ObserverMath::commandEquals(cmd, C5Protocol::CMD_PACKET_MONITOR)) {
        return Operation::PACKET_MONITOR;
    }
    if (C5ObserverMath::commandEquals(cmd, C5Protocol::CMD_DEAUTH_DETECTOR)) {
        return Operation::DEAUTH_DETECTOR;
    }
    if (C5ObserverMath::commandEquals(cmd, C5Protocol::CMD_START_GPS_RAW)) {
        return Operation::GPS_RAW;
    }

    static const char* const CONTINUOUS_COMMANDS[] = {
        C5Protocol::CMD_START_SNIFFER,
        C5Protocol::CMD_START_SNIFFER_NOSCAN,
        C5Protocol::CMD_START_SNIFFER_DOG,
        C5Protocol::CMD_START_DEAUTH,
        C5Protocol::CMD_START_EVIL_TWIN,
        "start_portal",
        C5Protocol::CMD_SAE_OVERFLOW,
        C5Protocol::CMD_START_KARMA,
        C5Protocol::CMD_START_BLACKOUT,
        C5Protocol::CMD_START_WARDRIVE,
        C5Protocol::CMD_START_WARDRIVE_PROMISC,
        C5Protocol::CMD_START_HANDSHAKE,
        C5Protocol::CMD_START_BEACON_SPAM,
        C5Protocol::CMD_START_ROGUEAP,
        C5Protocol::CMD_SCAN_AIRTAG,
        C5Protocol::CMD_ARP_BAN,
    };
    for (const char* token : CONTINUOUS_COMMANDS) {
        if (C5ObserverMath::commandEquals(cmd, token)) {
            return Operation::CONTINUOUS_OTHER;
        }
    }
    return Operation::NONE;
}

static void clearActiveOperation() {
    activeOperation = Operation::NONE;
    if (observerState) observerState->channelSurveyCapturing = false;
}

static void beginOperation(const char* cmd) {
    activeOperation = classifyOperation(cmd);
    if (activeOperation == Operation::NONE || !ensureObserverState()) return;

    if (activeOperation == Operation::CHANNEL_VIEW) {
        memset(observerState->channelNetworks, 0,
               sizeof(observerState->channelNetworks));
        observerState->channelSurveyTimestampMs = 0;
        observerState->channelSurveyCapturing = false;
    } else if (activeOperation == Operation::PACKET_MONITOR) {
        const char* p = C5ObserverMath::skipPrefix(cmd);
        while (*p && !isspace((unsigned char)*p)) p++;
        while (isspace((unsigned char)*p)) p++;
        char* end = nullptr;
        const unsigned long channel = strtoul(p, &end, 10);
        observerState->packetChannel =
            (end != p && channel > 0 && channel <= MAX_OBSERVER_CHANNEL)
                ? (uint8_t)channel
                : 0;
        observerState->packetPps = 0;
        observerState->packetTimestampMs = 0;
        C5ObserverMath::resetPacketRateHistory(
            observerState->packetHistory);
    }
}

static bool processObserverLine(const char* line) {
    if (!line || activeOperation == Operation::NONE ||
        !ensureObserverState()) {
        return false;
    }

    const char* trimmed = C5ObserverMath::skipPrefix(line);
    if (activeOperation == Operation::CHANNEL_VIEW) {
        if (strcmp(trimmed, "channel_view_start") == 0) {
            memset(observerState->channelNetworks, 0,
                   sizeof(observerState->channelNetworks));
            observerState->channelSurveyCapturing = true;
            return true;
        }
        if (strcmp(trimmed, "channel_view_end") == 0) {
            observerState->channelSurveyCapturing = false;
            observerState->channelSurveyTimestampMs = millis();
            if (observerState->channelSurveyTimestampMs == 0) {
                observerState->channelSurveyTimestampMs = 1;
            }
            observerState->channelSurveyRevision++;
            if (observerState->channelSurveyRevision == 0) {
                observerState->channelSurveyRevision = 1;
            }
            return true;
        }

        uint8_t channel = 0;
        uint16_t count = 0;
        if (observerState->channelSurveyCapturing &&
            C5ObserverMath::parseChannelCount(trimmed, channel, count)) {
            observerState->channelNetworks[channel] = count;
            return true;
        }
        return false;
    }

    if (activeOperation == Operation::PACKET_MONITOR) {
        uint32_t pps = 0;
        if (C5ObserverMath::parsePacketRate(trimmed, pps)) {
            observerState->packetPps = pps;
            C5ObserverMath::pushPacketRate(
                observerState->packetHistory, pps);
            observerState->packetTimestampMs = millis();
            if (observerState->packetTimestampMs == 0) {
                observerState->packetTimestampMs = 1;
            }
            observerState->packetRevision++;
            if (observerState->packetRevision == 0) {
                observerState->packetRevision = 1;
            }
            return true;
        }
    }
    return false;
}

static void logOutputLine(const char* line) {
    if (!ensureOutputLog()) return;
    uint8_t idx = (outputLog->head + outputLog->count) % OUTPUT_LOG_LINES;
    strncpy(outputLog->lines[idx], line, OUTPUT_LINE_LEN - 1);
    outputLog->lines[idx][OUTPUT_LINE_LEN - 1] = '\0';
    if (outputLog->count < OUTPUT_LOG_LINES) {
        outputLog->count++;
    } else {
        outputLog->head = (outputLog->head + 1) % OUTPUT_LOG_LINES;
    }
}

static bool isScanCommand(const char* cmd) {
    if (!cmd) return false;
    return strcmp(cmd, C5Protocol::CMD_SCAN_NETWORKS) == 0 ||
           strcmp(cmd, C5Protocol::CMD_SHOW_SCAN_RESULTS) == 0;
}

static bool isScanOutputCommand(const char* cmd) {
    return isScanCommand(cmd);
}

static bool isShowScanResultsCommand(const char* cmd) {
    return cmd && strcmp(cmd, C5Protocol::CMD_SHOW_SCAN_RESULTS) == 0;
}

static void publishScanResults() {
    if (!pendingScanResults) {
        scanResultsCapturing = false;
        return;
    }
    scanResults.count = pendingScanResults->count;
    if (scanResults.count > 0) {
        memcpy(scanResults.entries, pendingScanResults->entries,
               scanResults.count * sizeof(scanResults.entries[0]));
    }
    scanResults.timestampMs = millis();
    if (scanResults.timestampMs == 0) scanResults.timestampMs = 1;
    scanResults.revision++;
    if (scanResults.revision == 0) scanResults.revision = 1;
    scanResultsCapturing = false;
    backgroundScanActive = false;
    backgroundScanStartedMs = 0;
}

static bool isValidC5Config(uint8_t rxPin, uint8_t txPin, uint32_t baudRate) {
    return C5UartPolicy::isValidConfig(rxPin, txPin, baudRate);
}

static bool isScanSummaryLine(const char* line) {
    if (!line) return false;
    const char* trimmed = trimLeadingWs(line);
    if (!trimmed || trimmed[0] == '\0') return false;
    // Scan result lines start with '"'; summary/status lines never do.
    if (trimmed[0] == '"') return false;
    return strncmp(trimmed, C5Protocol::RESP_SCAN_PRINTED,
                   strlen(C5Protocol::RESP_SCAN_PRINTED)) == 0;
}

static bool isCommandErrorLine(const char* line) {
    if (!line) return false;
    const char* trimmed = trimLeadingWs(line);
    if (!trimmed || trimmed[0] == '\0') return false;

    return strncmp(trimmed, C5Protocol::RESP_ERROR_CODE, strlen(C5Protocol::RESP_ERROR_CODE)) == 0 ||
           strncmp(trimmed, C5Protocol::RESP_ERROR_USAGE, strlen(C5Protocol::RESP_ERROR_USAGE)) == 0 ||
           strncmp(trimmed, C5Protocol::RESP_SCAN_IN_PROGRESS, strlen(C5Protocol::RESP_SCAN_IN_PROGRESS)) == 0;
}

static void processLine(const char* line) {
    logOutputLine(line);
    lastActivityMs = millis();
    Serial.printf("[C5MON] << %s\n", line);

    C5GpsMath::RmcFix parsedFix = {};
    if (C5GpsMath::parseRmcFix(line, parsedFix)) {
        gpsFix.latitude = parsedFix.latitude;
        gpsFix.longitude = parsedFix.longitude;
        gpsFix.speedKmh = parsedFix.speedKmh;
        gpsFix.timestampMs = millis();
        if (gpsFix.timestampMs == 0) gpsFix.timestampMs = 1;
        gpsFix.revision++;
        if (gpsFix.revision == 0) gpsFix.revision = 1;
    }

    C5Protocol::TargetObservation parsedObservation{};
    const bool targetObservationAccepted =
        C5Protocol::parseTargetObservation(
            C5ObserverMath::skipPrefix(line), parsedObservation);
    if (targetObservationAccepted) {
        targetObservation.observation = parsedObservation;
        targetObservation.receivedAtMs = millis();
        if (targetObservation.receivedAtMs == 0u) {
            targetObservation.receivedAtMs = 1u;
        }
        targetObservation.receivedAtUs = micros();
        ++targetObservation.revision;
        if (targetObservation.revision == 0u) {
            targetObservation.revision = 1u;
        }
    }

    const bool observerSampleAccepted = processObserverLine(line);
    
    if (!line[0]) return;
    
    // Check for command completion
    if (awaitingResponse && !commandComplete) {
        // Track if expected response substring was seen
        if (expectedResponse[0] && strstr(line, expectedResponse)) {
            expectedResponseSeen = true;
        }
        
        // Check for explicit error markers first
        if (isCommandErrorLine(line)) {
            commandSuccess = false;
            commandComplete = true;
            // A JanOS command error proves the UART peer is alive. Preserve
            // bridge connectivity while reporting the command as failed.
            status = Status::CONNECTED;
            if (classifyOperation(lastCommand) == activeOperation) {
                clearActiveOperation();
            }
            if (strcmp(lastCommand, C5Protocol::CMD_SCAN_NETWORKS) == 0) {
                backgroundScanActive = false;
                backgroundScanStartedMs = 0;
                scanResultsCapturing = false;
            }
        }
        // JanOS prompt: bare ">" or "> " means the command finished.
        // Echoes and samples such as "> scan_networks" and "> 50pkts" are
        // payload-bearing lines, not prompts.
        // For commands requiring explicit output markers, wait until that marker
        // appears before treating the prompt as complete (probe uses this).
        else if (C5ObserverMath::isBarePrompt(line) &&
                 (!expectedResponse[0] || expectedResponseSeen)) {
            commandSuccess = true;
            commandComplete = true;
            if (status == Status::BUSY) status = Status::CONNECTED;
            expectedResponse[0] = '\0';
            expectedResponseSeen = false;
        }
        // Some continuous observer variants stream their first sample without
        // returning a prompt. A parsed sample is a stronger acknowledgement
        // than the echoed command and keeps STOP/recovery available.
        else if (observerSampleAccepted &&
                 classifyOperation(lastCommand) == activeOperation) {
            commandSuccess = true;
            commandComplete = true;
            if (status == Status::BUSY) status = Status::CONNECTED;
        }
        // observe_bssid is a bounded one-shot command, not a continuous
        // observer operation. Its parsed RF_OBS payload is nevertheless a
        // definitive acknowledgement for JanOS variants that omit a prompt.
        else if (targetObservationAccepted &&
                 C5ObserverMath::commandEquals(
                     lastCommand, C5Protocol::CMD_OBSERVE_BSSID)) {
            commandSuccess = true;
            commandComplete = true;
            if (status == Status::BUSY) status = Status::CONNECTED;
        }
        if (commandComplete && isShowScanResultsCommand(lastCommand) &&
            scanResultsCapturing) {
            if (commandSuccess) {
                publishScanResults();
            } else {
                scanResultsCapturing = false;
            }
        }
    }
    
    // Parse scan results
    C5Protocol::ScanEntry entry = {};
    if (scanResultsCapturing && C5ScanMath::parseScanLine(line, entry)) {
        if (pendingScanResults &&
            pendingScanResults->count < C5Protocol::MAX_SCAN_ENTRIES) {
            pendingScanResults->entries[pendingScanResults->count++] = entry;
        }
    }
    
    // Publish on JanOS's explicit footer; the prompt path above is the
    // fallback for firmware variants that omit it.
    if (scanResultsCapturing && isScanSummaryLine(line)) {
        publishScanResults();
        if (awaitingResponse && !commandComplete &&
            isScanCommand(lastCommand)) {
            commandSuccess = true;
            commandComplete = true;
            status = Status::CONNECTED;
            expectedResponse[0] = '\0';
            expectedResponseSeen = false;
        }
    }

    const char* trimmed = trimLeadingWs(line);
    if (trimmed &&
        (strncmp(trimmed, "Scan failed", 11) == 0 ||
         strncmp(trimmed, "Background scan stopped", 23) == 0)) {
        backgroundScanActive = false;
        backgroundScanStartedMs = 0;
        scanResultsCapturing = false;
    }
}

// ==[ PUBLIC API ]==

bool begin(uint8_t rx, uint8_t tx, uint32_t baud) {
    if (serialStarted) {
        c5Serial.end();
        serialStarted = false;
    }

    if (!isValidC5Config(rx, tx, baud)) {
        Serial.printf("[C5MON] invalid UART config rx=G%d tx=G%d baud=%lu\n", rx, tx, baud);
        status = Status::DISCONNECTED;
        return false;
    }

#if !HAMLET_TARGET_CORES3SE
    // Core2 exposes only one UART2 route for both the local GPS and C5 bridge.
    // Enabling C5 transfers ownership; disabling it can restore GPS in settings.
    GPS::stopUART();
#endif

    rxPin = rx;
    txPin = tx;
    baudRate = baud;
    
    c5Serial.begin(baud, SERIAL_8N1, rxPin, txPin);
    serialStarted = true;
    currentLineLen = 0;
    resetCommandState();
    deferredCommand[0] = '\0';
    clearActiveOperation();
    backgroundScanActive = false;
    backgroundScanStartedMs = 0;
    scanResultsCapturing = false;
    connectPhase = ConnectPhase::STARTUP_WAIT;
    nextConnectAttemptMs = millis() + CONNECT_STARTUP_GRACE_MS;
    if (observerState) memset(observerState, 0, sizeof(*observerState));
    memset(&targetObservation, 0, sizeof(targetObservation));
    
    Serial.printf("[C5MON] UART%d init rx=G%d tx=G%d baud=%lu\n", uartNum, rxPin, txPin, baud);
    
    // Probe after short delay (C5Monster may need boot time)
    delay(100);
    status = Status::DISCONNECTED;
    
    return true;
}

void stop() {
    if (serialStarted) {
        c5Serial.end();
        serialStarted = false;
    }
    status = Status::DISCONNECTED;
    currentLineLen = 0;
    resetCommandState();
    deferredCommand[0] = '\0';
    clearActiveOperation();
    backgroundScanActive = false;
    backgroundScanStartedMs = 0;
    scanResultsCapturing = false;
    connectPhase = ConnectPhase::IDLE;
    nextConnectAttemptMs = 0;
    if (observerState) memset(observerState, 0, sizeof(*observerState));
    memset(&targetObservation, 0, sizeof(targetObservation));
}

void service() {
    if (!serialStarted) return;
    
    // Drain available bytes
    while (c5Serial.available()) {
        char c = (char)c5Serial.read();
        if (c == '\n' || c == '\r') {
            if (currentLineLen > 0) {
                currentLine[currentLineLen] = '\0';
                processLine(currentLine);
                currentLineLen = 0;
            }
        } else if (currentLineLen < sizeof(currentLine) - 1) {
            currentLine[currentLineLen++] = c;
        }
    }
    
    // Check command timeout
    if (awaitingResponse && !commandComplete) {
        if (millis() - lastCommandMs > commandTimeoutMs) {
            Serial.printf("[C5MON] command timeout after %lums: %s\n", commandTimeoutMs, lastCommand);
            commandComplete = true;
            commandSuccess = false;
            if (isScanOutputCommand(lastCommand)) {
                scanResultsCapturing = false;
            }
            if (status == Status::BUSY) status = Status::ERROR;
        }
    }

    if (backgroundScanActive && backgroundScanStartedMs != 0 &&
        millis() - backgroundScanStartedMs > 20000u) {
        Serial.println("[C5MON] background scan stale after 20000ms");
        backgroundScanActive = false;
        backgroundScanStartedMs = 0;
        scanResultsCapturing = false;
    }

    if (deferredCommand[0] && commandComplete) {
        char nextCommand[sizeof(deferredCommand)];
        strncpy(nextCommand, deferredCommand, sizeof(nextCommand) - 1);
        nextCommand[sizeof(nextCommand) - 1] = '\0';
        const uint32_t nextTimeoutMs = deferredCommandTimeoutMs;
        deferredCommand[0] = '\0';
        if (commandSuccess) {
            status = Status::CONNECTED;
            sendCommand(nextCommand, nextTimeoutMs);
        }
    }
}

void maintainConnection(uint32_t now) {
    if (!serialStarted) return;

    if (connectPhase == ConnectPhase::IDLE) {
        if (status == Status::CONNECTED ||
            (status == Status::BUSY && !commandComplete)) {
            return;
        }
        scheduleConnectRetry(now);
        return;
    }

    if (connectPhase == ConnectPhase::STARTUP_WAIT ||
        connectPhase == ConnectPhase::RETRY_WAIT) {
        if (!timeReached(now, nextConnectAttemptMs)) return;

        Serial.println("[C5MON] automatic link resync...");
        connectPhase = ConnectPhase::STOPPING;
        if (!sendCommand(C5Protocol::CMD_STOP, CONNECT_STOP_TIMEOUT_MS)) {
            scheduleConnectRetry(now);
        }
        return;
    }

    if (connectPhase == ConnectPhase::STOPPING) {
        if (!commandComplete) return;

        // A failed STOP commonly means JanOS was still booting. Try the small
        // liveness exchange anyway; if that also misses, the retry timer owns
        // the next attempt without blocking the render loop.
        connectPhase = ConnectPhase::PINGING;
        if (!sendAsyncProbePing()) {
            scheduleConnectRetry(now);
        }
        return;
    }

    if (connectPhase == ConnectPhase::PINGING && commandComplete) {
        if (commandSuccess) {
            status = Status::CONNECTED;
            connectPhase = ConnectPhase::IDLE;
            nextConnectAttemptMs = 0;
            Serial.println("[C5MON] automatic link ready");
        } else {
            Serial.println("[C5MON] automatic link retry scheduled");
            scheduleConnectRetry(now);
        }
    }
}

bool probe() {
    if (!serialStarted) {
        Serial.println("[C5MON] probe FAIL: serial not started");
        return false;
    }
    service();
    connectPhase = ConnectPhase::STOPPING;
    Serial.println("[C5MON] resyncing C5Monster...");
    status = Status::DISCONNECTED;
    // JanOS operations survive a Core reboot. Reclaim the remote state before
    // probing so a stale packet monitor/attack cannot overlap the auto-scan
    // scheduler after the host has forgotten which operation was active.
    const bool stopped = sendCommandExpect(
        C5Protocol::CMD_STOP, "Stop command received", 6000);
    if (!stopped) {
        Serial.println("[C5MON] stop sync incomplete; trying ping");
    }
    // Use JanOS's short dedicated liveness exchange. The full help listing is
    // large enough to outlive the probe timeout and leave a fresh boot marked
    // disconnected while its UART is still streaming.
    Serial.println("[C5MON] probing C5Monster...");
    connectPhase = ConnectPhase::PINGING;
    const bool connected =
        sendCommandExpect(C5Protocol::CMD_PING, "pong", CONNECT_PING_TIMEOUT_MS);
    if (connected) {
        connectPhase = ConnectPhase::IDLE;
        nextConnectAttemptMs = 0;
    } else {
        scheduleConnectRetry(millis());
    }
    return connected;
}

Status getStatus() {
    return status;
}

bool isConnected() {
    return connectPhase == ConnectPhase::IDLE &&
           (status == Status::CONNECTED || status == Status::BUSY);
}

uint32_t getLastActivityMs() {
    return lastActivityMs;
}

bool sendCommand(const char* cmd, uint32_t timeoutMs) {
    if (!cmd || !cmd[0]) return false;
    if (!serialStarted) return false;
    service();
    if (isBusy() && !isStopCommand(cmd)) return false;
    if (activeOperation != Operation::NONE && !isStopCommand(cmd)) return false;
    if (isScanOutputCommand(cmd) && !ensurePendingScanResults()) return false;

    if (isStopCommand(cmd)) {
        deferredCommand[0] = '\0';
        clearActiveOperation();
    }
    
    // Send command
    c5Serial.println(cmd);
    c5Serial.flush();
    
    strncpy(lastCommand, cmd, sizeof(lastCommand) - 1);
    lastCommand[sizeof(lastCommand) - 1] = '\0';
    expectedResponse[0] = '\0';
    expectedResponseSeen = false;
    awaitingResponse = true;
    commandSuccess = false;
    commandComplete = false;
    lastCommandMs = millis();
    commandTimeoutMs =
        isScanOutputCommand(cmd) && timeoutMs < 20000u ? 20000u : timeoutMs;
    status = Status::BUSY;
    beginOperation(cmd);
    
    Serial.printf("[C5MON] >> %s\n", cmd);

    // Current JanOS prints the completed CSV set asynchronously after
    // scan_networks. show_scan_results is retained for compatible variants.
    if (isScanOutputCommand(cmd)) {
        pendingScanResults->count = 0;
        scanResultsCapturing = true;
        if (strcmp(cmd, C5Protocol::CMD_SCAN_NETWORKS) == 0) {
            backgroundScanActive = true;
            backgroundScanStartedMs = millis();
            if (backgroundScanStartedMs == 0) backgroundScanStartedMs = 1;
        }
    }

    return true;
}

bool sendCommandExpect(const char* cmd, const char* expected, uint32_t timeoutMs) {
    if (!cmd || !cmd[0] || !expected || !expected[0]) return false;
    if (!serialStarted) return false;
    service();
    if (isBusy() && !isStopCommand(cmd)) return false;
    if (activeOperation != Operation::NONE && !isStopCommand(cmd)) return false;
    if (isScanOutputCommand(cmd) && !ensurePendingScanResults()) return false;

    if (isStopCommand(cmd)) {
        deferredCommand[0] = '\0';
        clearActiveOperation();
    }
    
    c5Serial.println(cmd);
    c5Serial.flush();
    
    strncpy(lastCommand, cmd, sizeof(lastCommand) - 1);
    lastCommand[sizeof(lastCommand) - 1] = '\0';
    strncpy(expectedResponse, expected, sizeof(expectedResponse) - 1);
    expectedResponse[sizeof(expectedResponse) - 1] = '\0';
    expectedResponseSeen = false;
    if (isScanOutputCommand(cmd)) {
        pendingScanResults->count = 0;
        scanResultsCapturing = true;
        if (strcmp(cmd, C5Protocol::CMD_SCAN_NETWORKS) == 0) {
            backgroundScanActive = true;
            backgroundScanStartedMs = millis();
            if (backgroundScanStartedMs == 0) backgroundScanStartedMs = 1;
        }
    }
    awaitingResponse = true;
    commandSuccess = false;
    commandComplete = false;
    lastCommandMs = millis();
    commandTimeoutMs =
        isScanOutputCommand(cmd) && timeoutMs < 20000u ? 20000u : timeoutMs;
    status = Status::BUSY;
    beginOperation(cmd);
    
    // Poll for response (blocking with timeout)
    uint32_t start = millis();
    while (!commandComplete && millis() - start < commandTimeoutMs) {
        service();
        delay(10);
    }
    
    // expectedResponse is matched inline in processLine — no log scan needed.
    if (commandComplete) {
        status = commandSuccess ? Status::CONNECTED : Status::ERROR;
        return commandSuccess;
    }
    
    status = Status::DISCONNECTED;
    return false;
}

bool sendCommandThen(const char* first, const char* second, uint32_t timeoutMs) {
    if (!first || !first[0] || !second || !second[0]) return false;
    if (deferredCommand[0] || isBusy() ||
        activeOperation != Operation::NONE) {
        return false;
    }
    if (!sendCommand(first, timeoutMs)) return false;

    strncpy(deferredCommand, second, sizeof(deferredCommand) - 1);
    deferredCommand[sizeof(deferredCommand) - 1] = '\0';
    deferredCommandTimeoutMs = timeoutMs;
    return true;
}

bool isBusy() {
    return (status == Status::BUSY && !commandComplete) ||
           backgroundScanActive;
}

bool hasActiveOperation() {
    return activeOperation != Operation::NONE;
}

Operation getActiveOperation() {
    return activeOperation;
}

void emergencyStop() {
    if (serialStarted) {
        c5Serial.println(C5Protocol::CMD_STOP);
        c5Serial.flush();
        status = Status::CONNECTED;
        awaitingResponse = false;
        commandComplete = true;
        commandSuccess = true;
        deferredCommand[0] = '\0';
        clearActiveOperation();
        backgroundScanActive = false;
        backgroundScanStartedMs = 0;
        scanResultsCapturing = false;
        connectPhase = ConnectPhase::IDLE;
        nextConnectAttemptMs = 0;
    }
}

const ScanResults& getScanResults() {
    return scanResults;
}

bool hasNewResults(uint32_t sinceMs) {
    return scanResults.timestampMs != 0 && scanResults.timestampMs != sinceMs;
}

uint8_t get5GHzNetworkCount() {
    uint8_t count = 0;
    for (uint8_t i = 0; i < scanResults.count; i++) {
        if (scanResults.entries[i].is5GHz) count++;
    }
    return count;
}

int8_t get5GHzChannelPeakRSSI(uint8_t channel) {
    int8_t peak = -128;
    for (uint8_t i = 0; i < scanResults.count; i++) {
        if (scanResults.entries[i].is5GHz && scanResults.entries[i].channel == channel) {
            if (scanResults.entries[i].rssi > peak) peak = scanResults.entries[i].rssi;
        }
    }
    return peak;
}

uint16_t get5GHzChannelNetworkCount(uint8_t channel) {
    uint16_t count = 0;
    for (uint8_t i = 0; i < scanResults.count; i++) {
        if (scanResults.entries[i].is5GHz && scanResults.entries[i].channel == channel) count++;
    }
    return count;
}

const GPSFix& getGPSFix() {
    return gpsFix;
}

bool hasFreshGPSFix(uint32_t maxAgeMs) {
    return gpsFix.timestampMs != 0 &&
           millis() - gpsFix.timestampMs <= maxAgeMs;
}

uint32_t getGPSFixAgeMs() {
    if (gpsFix.timestampMs == 0) return UINT32_MAX;
    return millis() - gpsFix.timestampMs;
}

uint16_t getObservedChannelNetworkCount(uint8_t channel) {
    if (!observerState || channel == 0 || channel > MAX_OBSERVER_CHANNEL) {
        return 0;
    }
    return observerState->channelNetworks[channel];
}

bool isChannelSurveyFresh(uint32_t maxAgeMs) {
    return observerState && observerState->channelSurveyTimestampMs != 0 &&
           millis() - observerState->channelSurveyTimestampMs <= maxAgeMs;
}

bool getChannelSurveyTelemetry(ChannelSurveyTelemetry& telemetry) {
    if (!observerState) return false;
    telemetry.capturing = observerState->channelSurveyCapturing;
    telemetry.revision = observerState->channelSurveyRevision;
    telemetry.hasCompletedSample =
        observerState->channelSurveyTimestampMs != 0;
    telemetry.ageMs = telemetry.hasCompletedSample
        ? millis() - observerState->channelSurveyTimestampMs
        : UINT32_MAX;
    return telemetry.capturing || telemetry.hasCompletedSample;
}

bool getPacketMonitorSample(uint8_t& channel, uint32_t& packetsPerSecond,
                            uint32_t& ageMs) {
    if (!observerState || observerState->packetChannel == 0 ||
        observerState->packetTimestampMs == 0) {
        return false;
    }
    channel = observerState->packetChannel;
    packetsPerSecond = observerState->packetPps;
    ageMs = millis() - observerState->packetTimestampMs;
    return true;
}

bool getPacketMonitorTelemetry(PacketMonitorTelemetry& telemetry) {
    if (!observerState || observerState->packetChannel == 0) return false;

    telemetry.channel = observerState->packetChannel;
    telemetry.packetsPerSecond = observerState->packetPps;
    telemetry.revision = observerState->packetRevision;
    telemetry.ageMs = observerState->packetTimestampMs != 0
        ? millis() - observerState->packetTimestampMs
        : UINT32_MAX;
    telemetry.historyCount =
        C5ObserverMath::copyPacketRatesChronological(
            observerState->packetHistory, telemetry.history,
            PACKET_MONITOR_HISTORY_CAPACITY);
    return true;
}

bool getTargetObservation(TargetObservationTelemetry& telemetry) {
    if (targetObservation.revision == 0u ||
        targetObservation.receivedAtMs == 0u) {
        return false;
    }
    telemetry = targetObservation;
    telemetry.ageMs = millis() - targetObservation.receivedAtMs;
    return true;
}

const OutputLog& getOutputLog() {
    static const OutputLog emptyOutputLog = {};
    return outputLog ? *outputLog : emptyOutputLog;
}

void clearOutputLog() {
    if (!outputLog) return;
    outputLog->head = 0;
    outputLog->count = 0;
}

void setRxPin(uint8_t pin) { rxPin = pin; }
void setTxPin(uint8_t pin) { txPin = pin; }
void setBaudRate(uint32_t baud) { baudRate = baud; }
uint8_t getRxPin() { return rxPin; }
uint8_t getTxPin() { return txPin; }

} // namespace C5Monster
