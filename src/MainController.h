#ifndef MAINCONTROLLER_H
#define MAINCONTROLLER_H

#include <functional>
#include "ConfigManager.h"
#include "WiFiManager.h"
#include "NTPManager.h"
#include "RelayManager.h"
#include "ScheduleManager.h"
#include "ErrorHandler.h"
#include "RelayLogger.h"
#include "RGBLedManager.h"
#include "ChipTempSensorManager.h"
#include "PerformanceStats.h"
#include "LoggingPerformanceTest.h"
#include "DS3231Manager.h"
#include "MQTTFallbackManager.h"
#include "OTAManager.h"
#if defined(ARDUINO_ARCH_ESP32)
#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>
#include <freertos/semphr.h>
#endif

// Forward declarations to avoid circular dependencies
class MQTTManager;
class WebServerManager;
class DS18B20SensorManager;
class SHT4xSensorManager;
class MAX31855KSensorManager;


// Global pointer for HTTPS handlers to access WebServerManager
//extern WebServerManager* globalWebServerManager;

// Component indices - defined outside class for external use
enum ComponentIndex {
    DS18B20_INDEX = 0,
    SHT4X_INDEX = 1,
    SCHEDULE_INDEX = 2,
    MQTT_INDEX = 3,
    WEB_SERVER_INDEX = 4,
    RELAY_INDEX = 5,
    COMPONENT_COUNT = 6  // Total number of components
};

class MainController {
public:
    struct PerformanceStatsSnapshot {
        PerformanceStats mainLoopStats;
        PerformanceStats componentStats[COMPONENT_COUNT];
        RelayLoggerStats relayLoggerStats;
    };
    struct ControlQueueDiagnosticsSnapshot {
        uint32_t enqueueAccepted = 0;
        uint32_t enqueueRejected = 0;
        uint32_t commandsStarted = 0;
        uint32_t commandsSucceeded = 0;
        uint32_t commandsFailed = 0;
        uint32_t commandsTimedOut = 0;
        uint32_t queueTimeouts = 0;
        uint32_t executionTimeouts = 0;
        uint32_t maxQueueDepth = 0;
        uint32_t maxQueueWaitMs = 0;
        uint32_t p95QueueWaitMs = 0;
        uint64_t totalQueueWaitMs = 0;
        uint32_t maxExecutionMs = 0;
        uint32_t p95ExecutionMs = 0;
        uint64_t totalExecutionMs = 0;
        uint32_t currentQueueDepth = 0;
        uint32_t runningCount = 0;
        uint32_t currentInFlight = 0;
        uint32_t maxInFlight = 0;
        bool processingActive = false;
    };
    struct MemoryDiagnosticsSnapshot {
        uint32_t totalHeap = 0;
        uint32_t freeHeapCurrent = 0;
        uint32_t freeHeapMin = 0;
        uint32_t freeHeapMax = 0;
        uint32_t maxAllocHeapCurrent = 0;
        uint32_t maxAllocHeapMin = 0;
        uint32_t totalPsram = 0;
        uint32_t freePsramCurrent = 0;
        uint32_t freePsramMin = 0;
        uint32_t freePsramMax = 0;
        uint32_t maxAllocPsramCurrent = 0;
        uint32_t maxAllocPsramMin = 0;
    };
    struct WiFiStabilitySnapshot {
        bool connected = false;
        int32_t rssi = 0;
        int32_t statusCode = 0;
        int32_t lastDisconnectStatusCode = 0;
        uint32_t reconnectTransitions = 0;
        uint32_t disconnectTransitions = 0;
        unsigned long lastReconnectAgeMs = 0;
        unsigned long lastDisconnectAgeMs = 0;
    };

    enum class ControlCommandType : uint8_t {
        NONE = 0,
        NOOP = 1,
        APPLY_SETTINGS_JSON = 2,
        RELAY_SET_MODE = 3,
        RELAY_TIMER_START = 4,
        RELAY_TIMER_STOP = 5,
        DS18_CLEAR_SCAN_RESULT = 6,
        DS18_REQUEST_SCAN = 7,
        OTA_REFRESH_CONFIG_STATUS = 8,
        OTA_ARM_WINDOW = 9,
        MONITORING_RESET_STATS = 10,
        RELAY_CLEAR_LOGS = 11,
        ERROR_CLEAR_LOGS = 12,
        MONITORING_RESET_QUEUE_DIAGNOSTICS = 13
    };

    enum class ControlCommandStatus : uint8_t {
        INVALID = 0,
        QUEUED,
        RUNNING,
        SUCCEEDED,
        FAILED,
        TIMED_OUT
    };

    struct ControlCommand {
        ControlCommandType type = ControlCommandType::NONE;
        int32_t arg0 = 0;
        int32_t arg1 = 0;
        char textArg[64] = {0};
    };

    struct ControlCommandResult {
        uint32_t commandId = 0;
        ControlCommandStatus status = ControlCommandStatus::INVALID;
        int32_t errorCode = 0;
        char message[96] = {0};
        unsigned long updatedAtMs = 0;
    };

    MainController();
    ~MainController();

    // Main lifecycle methods
    void setup();
    void loop();

    // Initialization state
    bool isWebServerInitialized() const { return webServerInitialized; }

    // Safety mechanism state check
    bool isSafetyShutdownActive() const { return safetyShutdownTriggered; }

    // RTC/Time status (used by WebServerManager monitoring/API)
    bool isRtcEnabled() const;
    bool isRtcInitialized() const { return ds3231Manager.isInitialized(); }
    bool isRtcPresent() const { return ds3231Manager.isPresent(); }
    bool rtcHasLostPower() const { return ds3231Manager.hasLostPower(); }
    String getRtcFormattedDateTime() const;
    time_t getLastRtcSyncEpoch() const { return lastRtcSyncEpoch; }
    String getLastRtcSyncFormattedDateTime() const;

    // High-level time source label
    // - "NTP" when we have at least one successful NTP sync
    // - "DS3231" when system time is set but NTP never succeeded and RTC is present
    // - "UNSET" otherwise
    String getTimeSource() const;

    // OTA read snapshots
    OtaStatusSnapshot getOtaStatus() const;
    String getOtaLogText() const;

    // Control-plane queue API (main-loop owned mutation path)
    bool enqueueControlCommand(const ControlCommand& command, uint32_t& outCommandId);
    bool enqueueSettingsUpdateCommand(const String& jsonBody, uint32_t& outCommandId, String& outError);
    bool getControlCommandResult(uint32_t commandId, ControlCommandResult& outResult) const;
    bool clearControlCommandResult(uint32_t commandId);

    // Shared-state ownership checklist (async runtime):
    // 1) MainController::loop() is the control-plane owner for mutating shared managers/settings.
    // 2) Web/API paths should dispatch mutations through the control command queue.
    // 3) Non-loop code should prefer read-only snapshots/views of shared state.

private:
    struct ControlCommandSlot {
        bool allocated = false;
        bool queued = false;
        bool running = false;
        uint32_t commandId = 0;
        ControlCommand command;
        ControlCommandResult result;
        unsigned long queuedAtMs = 0;
        unsigned long startedAtMs = 0;
    };

    static const uint8_t CONTROL_QUEUE_CAPACITY = 8;
    static const uint8_t CONTROL_COMMANDS_PER_LOOP = 2;
    static const unsigned long CONTROL_COMMAND_TIMEOUT_MS = 10000UL;

    ControlCommandSlot controlCommandSlots[CONTROL_QUEUE_CAPACITY];
    uint8_t controlQueueIndices[CONTROL_QUEUE_CAPACITY];
    uint8_t controlQueueHead;
    uint8_t controlQueueTail;
    uint8_t controlQueueCount;
    bool controlQueueProcessing;
    uint32_t nextControlCommandId;
    uint32_t nextSettingsUpdateToken;

    struct PendingSettingsUpdate {
        bool active = false;
        uint32_t token = 0;
        uint32_t commandId = 0;
        String jsonBody;
    };
    struct ControlQueueDiagnosticsData {
        uint32_t enqueueAccepted = 0;
        uint32_t enqueueRejected = 0;
        uint32_t commandsStarted = 0;
        uint32_t commandsSucceeded = 0;
        uint32_t commandsFailed = 0;
        uint32_t commandsTimedOut = 0;
        uint32_t queueTimeouts = 0;
        uint32_t executionTimeouts = 0;
        uint32_t maxQueueDepth = 0;
        uint32_t maxQueueWaitMs = 0;
        uint64_t totalQueueWaitMs = 0;
        uint32_t maxExecutionMs = 0;
        uint64_t totalExecutionMs = 0;
        uint32_t maxInFlight = 0;
        uint16_t queueWaitSamples[128] = {0};
        uint16_t executionSamples[128] = {0};
        uint16_t queueWaitSampleCount = 0;
        uint16_t executionSampleCount = 0;
        uint16_t queueWaitSampleWriteIndex = 0;
        uint16_t executionSampleWriteIndex = 0;
    };
    struct MemoryDiagnosticsData {
        bool initialized = false;
        uint32_t totalHeap = 0;
        uint32_t freeHeapCurrent = 0;
        uint32_t freeHeapMin = 0;
        uint32_t freeHeapMax = 0;
        uint32_t maxAllocHeapCurrent = 0;
        uint32_t maxAllocHeapMin = 0;
        uint32_t totalPsram = 0;
        uint32_t freePsramCurrent = 0;
        uint32_t freePsramMin = 0;
        uint32_t freePsramMax = 0;
        uint32_t maxAllocPsramCurrent = 0;
        uint32_t maxAllocPsramMin = 0;
    };
    struct WiFiStabilityData {
        uint32_t reconnectTransitions = 0;
        uint32_t disconnectTransitions = 0;
        unsigned long lastReconnectMs = 0;
        unsigned long lastDisconnectMs = 0;
        int32_t lastDisconnectStatusCode = 0;
    };
    PendingSettingsUpdate pendingSettingsUpdate;
    ControlQueueDiagnosticsData controlQueueDiagnostics;
    MemoryDiagnosticsData memoryDiagnostics;
    WiFiStabilityData wifiStability;
#if defined(ARDUINO_ARCH_ESP32)
    mutable SemaphoreHandle_t controlQueueMutex;
#endif

private:
    // Managers
    ConfigManager configManager;
    WiFiManager wifiManager;
    MQTTManager* mqttManager;
    WebServerManager* webServerManager;
    NTPManager ntpManager;
    DS3231Manager ds3231Manager;
    DS18B20SensorManager* ds18b20Manager;
    SHT4xSensorManager* sht4xManager;
    MAX31855KSensorManager* max31855kManager;
    ChipTempSensorManager* chipTempManager;
    RGBLedManager* ledManager;
    RelayManager* relayManager;
    ScheduleManager* scheduleManager;
    MQTTFallbackManager* mqttFallbackManager;
    OTAManager otaManager;
    ErrorHandler errorHandler;
    RelayLogger relayLogger;

    // State variables
    unsigned long lastMsg;
    bool webServerInitialized;
    bool wifiEverConnected;
    bool mdnsResponderActive;
    bool lastWiFiConnectedState;
    bool criticalConfigInvalid = false;
    unsigned long bootTimeMillis;  // Store boot time in millis
    unsigned long lastLogFlushTime;  // Track when the last periodic flush occurred
    String bootDateTime;  // Calculated boot datetime when NTP syncs

    // RTC (DS3231) tracking
    unsigned long lastRtcSyncMillis = 0;
    time_t lastRtcSyncEpoch = 0;
    
    // Performance monitoring
    PerformanceStats mainLoopStats;
    PerformanceStats componentStats[6]; // Array for component-level operations
    unsigned long schedulerEvalStart = 0;  // Start time for scheduler evaluation tracking

    // File logger performance monitoring
public:
    RelayLoggerStats relayLoggerStats;
private:

    // Safety mechanism state
    unsigned long safetyHighTempStartTime = 0; // When temperature first exceeded threshold
    bool safetyShutdownTriggered = false;      // Whether safety shutdown has occurred

    // Private methods
    void initializeSensors();
    void initializeSHT4xSensor();
    void initializeRelayManager();
    void updateOtherSensorReadings();
    void handleSerialCommands();
    void updateBootDateTime();
    String formatDateTime(time_t t) const;
    void refreshMdnsResponder(bool forceRestart);
    void stopMdnsResponder();
    void resetControlCommandQueue();
    void processControlCommandQueue();
    bool executeControlCommand(ControlCommandSlot& slot, unsigned long nowMs);
    void clearPendingSettingsUpdate();
    void releaseControlCommandResources(const ControlCommand& command);
    void resetControlQueueDiagnostics();
    void resetControlQueueDiagnosticsUnlocked();
    static uint32_t computeP95SampleMs(const uint16_t* samples, uint16_t count);
    void pushQueueWaitSampleUnlocked(uint32_t queueWaitMs);
    void pushExecutionSampleUnlocked(uint32_t executionMs);
    bool isControlCommandQueueFull() const;
    bool lockControlQueue() const;
    void unlockControlQueue() const;
    uint32_t countRunningControlCommandsUnlocked() const;
    ControlCommandSlot* findControlCommandSlotById(uint32_t commandId);
    const ControlCommandSlot* findControlCommandSlotById(uint32_t commandId) const;
    static bool isTerminalControlCommandStatus(ControlCommandStatus status);
    static void writeControlCommandMessage(ControlCommandResult& result, const char* message);

    // LED status management
    LedStatus determineLedStatus();
    void updateLedStatus(unsigned long nowMs);
    static const unsigned long LED_BASE_UPDATE_INTERVAL_MS = 250;
    static const unsigned long LED_RED_BLINK_INTERVAL_MS = 50;
    static const unsigned long LED_YELLOW_BLINK_INTERVAL_MS = 100;

    unsigned long lastLedBaseUpdateMs;
    LedStatus lastLedBaseStatus;

    bool ledBlinkOn;
    unsigned long lastLedBlinkToggleMs;
    LedStatus lastLedRenderedStatus;
    
    // Performance monitoring methods
    void updateComponentStats(int index, unsigned long duration);
    void updateMainLoopStats(unsigned long duration);
    void resetPerformanceStats();
    void updateMemoryDiagnostics();
    void updateMemoryDiagnosticsUnlocked(uint32_t freeHeap,
                                         uint32_t maxAllocHeap,
                                         uint32_t totalHeap,
                                         uint32_t freePsram,
                                         uint32_t maxAllocPsram,
                                         uint32_t totalPsram);
    void resetMemoryDiagnosticsUnlocked();
    void timeComponentUpdate(int componentIndex, std::function<void()> updateFunction);

    // Relay logger performance monitoring methods
    void resetRelayLoggerStats();
    void updateRelayLoggerStat(PerformanceStats& stats, unsigned long duration);
    void updateFileLoggerStat(PerformanceStats& stats, unsigned long duration);
    void resetRelayLoggerStatsUnlocked();
    void updateFileLoggerStatUnlocked(PerformanceStats& stats, unsigned long duration);

    void lockPerformanceStats() const;
    void unlockPerformanceStats() const;
#if defined(ARDUINO_ARCH_ESP32)
    mutable portMUX_TYPE performanceStatsLock = portMUX_INITIALIZER_UNLOCKED;
#endif


public:
    // Public access for component timing updates
    void updateComponentStatsPublic(int index, unsigned long duration) {
        updateComponentStats(index, duration);
    }
    
    // Public snapshot for web interface/async-safe reads.
    PerformanceStatsSnapshot getPerformanceStatsSnapshot() const;
    ControlQueueDiagnosticsSnapshot getControlQueueDiagnosticsSnapshot() const;
    MemoryDiagnosticsSnapshot getMemoryDiagnosticsSnapshot() const;
    WiFiStabilitySnapshot getWiFiStabilitySnapshot() const;

    // File logger performance monitoring
    void updateRelayLoggerStats(int operationIndex, unsigned long duration);

    // Performance testing
    bool runLoggingPerformanceTest();

    // Scheduler evaluation timing methods
    void startSchedulerEvaluation();
    void endSchedulerEvaluation();

    // Log flush interval (calculated from config)
    unsigned long LOG_FLUSH_INTERVAL_MS;
};

#endif // MAINCONTROLLER_H
