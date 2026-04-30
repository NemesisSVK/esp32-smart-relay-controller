#ifndef ERRORHANDLER_H
#define ERRORHANDLER_H

#include <Arduino.h>
#include <vector>
#include <ArduinoJson.h>
#if defined(ARDUINO_ARCH_ESP32)
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#endif

// Forward declarations
class NTPManager;
class MainController;

// Error codes categorized by component
enum class ErrorCode {
    // Network errors (1000-1999)
    WIFI_CONNECTION_FAILED = 1000,
    WIFI_CONNECTION_LOST = 1001,
    WIFI_INVALID_CREDENTIALS = 1002,
    MQTT_CONNECTION_FAILED = 1100,
    MQTT_CONNECTION_LOST_ERROR = 1101,
    MQTT_INVALID_CREDENTIALS = 1102,
    MQTT_PUBLISH_FAILED = 1103,

    // Sensor errors (2000-2999)
    DS18B20_READ_FAILED = 2000,
    DS18B20_CRC_ERROR = 2001,
    DS18B20_NO_SENSOR_FOUND = 2002,
    SHT4X_READ_FAILED = 2100,
    SHT4X_CRC_ERROR = 2101,
    SHT4X_INITIALIZATION_FAILED = 2102,
    MAX31855K_READ_FAILED = 2200,
    MAX31855K_FAULT = 2201,

    // Configuration errors (3000-3999)
    CONFIG_LOAD_FAILED = 3000,
    CONFIG_SAVE_FAILED = 3001,
    CONFIG_VALIDATION_FAILED = 3002,
    CONFIG_FILE_CORRUPTED = 3003,

    // System errors (4000-4999)
    MEMORY_ALLOCATION_FAILED = 4000,
    WATCHDOG_RESET = 4001,
    FILESYSTEM_ERROR = 4002,
    FILESYSTEM_FULL = 4003,

    // LED errors (5000-5999)
    LED_INITIALIZATION_FAILED = 5000,
    LED_UPDATE_FAILED = 5001,

    // Relay errors (5500-5999)
    RELAY_TIMEOUT_VIOLATION = 5500,
    RELAY_SAFETY_SHUTDOWN = 5501,

    // IP Whitelist errors (6000-6999)
    IP_WHITELIST_CONFIG_ERROR = 6000,
    IP_WHITELIST_ACCESS_DENIED = 6001,
    IP_WHITELIST_INVALID_FORMAT = 6002,

    // Scheduling errors (7000-7999)
    SCHEDULE_INVALID_TIME_FORMAT = 7000,
    SCHEDULE_INVALID_LEVEL = 7001,
    SCHEDULE_NTP_NOT_SYNCHRONIZED = 7002,
    SCHEDULE_EXECUTION_FAILED = 7004,
    SCHEDULE_CONFIG_LOAD_FAILED = 7005,
    SCHEDULE_TIME_PARSE_ERROR = 7006,
    SCHEDULE_RELAY_SET_FAILED = 7007
};

// Error severity levels
enum class ErrorSeverity {
    INFO,     // Non-critical, system continues normally
    WARNING,  // Degraded functionality, automatic recovery attempted
    ERROR,    // Critical failure, manual intervention may be required
    FATAL     // System restart required
};

// Error entry structure
struct ErrorEntry {
    ErrorCode code;
    ErrorSeverity severity;
    String message;
    String context;
    unsigned long timestamp;
    String timestampDateTime;  // Actual datetime string
    int occurrenceCount;
    unsigned long lastOccurrence;
    String lastOccurrenceDateTime;  // Actual datetime string
    bool resolved;

    ErrorEntry(ErrorCode c, ErrorSeverity s, const String& msg, const String& ctx = "")
        : code(c), severity(s), message(msg), context(ctx), timestamp(millis()),
          occurrenceCount(1), lastOccurrence(millis()), resolved(false) {}
};

// Error event structure for history
struct ErrorEvent {
    unsigned long event_id;
    unsigned long timestamp;
    String datetime;
    String datetime_start;
    ErrorCode code;
    String code_name;
    ErrorSeverity severity;
    String message;
    int counter;
    String context;
    
    ErrorEvent() : event_id(0), timestamp(0), code(ErrorCode::WIFI_CONNECTION_FAILED),
                  severity(ErrorSeverity::INFO), counter(1) {}
};

class ErrorHandler {
public:
    // Manager setup
    static void setNTPManager(NTPManager* ntpMgr);
    static void setMainController(MainController* mc);

    // Core error reporting
    static void reportError(ErrorCode code, ErrorSeverity severity, const String& message, const String& context = "");

    // Error data access
    static std::vector<ErrorEntry> getActiveErrors();
    static int getTotalErrorCount();
    static unsigned long getLastErrorTime();
    static int getErrorsInLastHour();

    // Error history management
    static bool addErrorToHistory(ErrorCode code, ErrorSeverity severity,
                                  const String& message, const String& context = "");
    static String getErrorHistoryJson();
    static int getErrorHistoryCount();
    static bool clearErrorHistory();

    // Persistence
    static bool loadErrorState();
    static bool loadErrorHistory();

    // Dirty flag flushing
    static void flushErrorStateIfDirty();
    static void flushErrorHistoryIfDirty();

    // Force flush methods for critical operations
    static void forceFlushErrorState();
    static void forceFlushErrorHistory();

    // Utility methods
    static String severityToString(ErrorSeverity severity);
    static String errorCodeToString(ErrorCode code);

private:
    static NTPManager* ntpManager;
    static MainController* mainController;
    static std::vector<ErrorEntry> errorLog;
    static const int MAX_ERROR_LOG_SIZE = 50;

    // Error history members
    static std::vector<ErrorEvent> errorHistory;
    static unsigned long nextEventId;
    static const int MAX_ERROR_HISTORY = 200;

    // Dirty flags for deferred saving
    static bool errorStateDirty;
    static bool errorHistoryDirty;

#if defined(ARDUINO_ARCH_ESP32)
    static SemaphoreHandle_t stateMutex;
#endif

    static void lockState();
    static void unlockState();
    class StateLockGuard {
    public:
        StateLockGuard() {
            ErrorHandler::lockState();
        }
        ~StateLockGuard() {
            ErrorHandler::unlockState();
        }
    };

    // Helper methods
    static void addErrorEntry(const ErrorEntry& entry);
    static int findErrorIndex(ErrorCode code, const String& context);
    static void cleanupOldErrors();
    static bool saveErrorState();
    
    // Error history helper methods
    static bool saveErrorHistory();
    static void trimHistoryToLimit();
    static bool addHistoryEvent(const ErrorEvent& event);
    static void buildErrorHistoryJsonDocument(JsonDocument& doc);
};

// File paths for error history
static const char ERROR_HISTORY_FILE[] = "/error_history.json";
static const char ERROR_HISTORY_BACKUP_FILE[] = "/error_history_backup.json";

#endif // ERRORHANDLER_H
