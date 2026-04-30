#include "ErrorHandler.h"
#include "NTPManager.h"
#include "MainController.h" 
#include <LittleFS.h> 
#include <ArduinoJson.h> 
#include "PsramJsonAllocator.h"
#include "UnifiedLogger.h"
#include <cstring> 

namespace {
String inferDs18b20ContextFromMessage(const ErrorCode code, const String& message) {
    if (code != ErrorCode::DS18B20_READ_FAILED) {
        return "";
    }

    const char* prefix = "DS18B20 sensor ";
    if (!message.startsWith(prefix)) {
        return "";
    }

    const int prefixLen = static_cast<int>(strlen(prefix));
    int idx = prefixLen;
    int sensorNumber = 0;
    while (idx < message.length()) {
        const char c = message.charAt(idx);
        if (c < '0' || c > '9') {
            break;
        }
        sensorNumber = (sensorNumber * 10) + (c - '0');
        idx++;
    }

    if (sensorNumber <= 0) {
        return "";
    }

    return "ds18b20.sensor=" + String(sensorNumber);
}
#if defined(ARDUINO_ARCH_ESP32)
portMUX_TYPE g_errorHandlerMutexInitMux = portMUX_INITIALIZER_UNLOCKED;
#endif
} // namespace
 
// Static member initialization 
NTPManager* ErrorHandler::ntpManager = nullptr; 
MainController* ErrorHandler::mainController = nullptr; 
std::vector<ErrorEntry> ErrorHandler::errorLog; 
std::vector<ErrorEvent> ErrorHandler::errorHistory;
unsigned long ErrorHandler::nextEventId = 1;
bool ErrorHandler::errorStateDirty = false;
bool ErrorHandler::errorHistoryDirty = false;
#if defined(ARDUINO_ARCH_ESP32)
SemaphoreHandle_t ErrorHandler::stateMutex = nullptr;
#endif

void ErrorHandler::lockState() {
#if defined(ARDUINO_ARCH_ESP32)
    if (!stateMutex) {
        portENTER_CRITICAL(&g_errorHandlerMutexInitMux);
        if (!stateMutex) {
            stateMutex = xSemaphoreCreateRecursiveMutex();
        }
        portEXIT_CRITICAL(&g_errorHandlerMutexInitMux);
    }
    if (stateMutex) {
        xSemaphoreTakeRecursive(stateMutex, portMAX_DELAY);
    }
#endif
}

void ErrorHandler::unlockState() {
#if defined(ARDUINO_ARCH_ESP32)
    if (stateMutex) {
        xSemaphoreGiveRecursive(stateMutex);
    }
#endif
}

void ErrorHandler::setNTPManager(NTPManager* ntpMgr) {
    StateLockGuard guard;
    ntpManager = ntpMgr;
}

void ErrorHandler::setMainController(MainController* mc) {
    StateLockGuard guard;
    mainController = mc;
}

void ErrorHandler::reportError(ErrorCode code, ErrorSeverity severity, const String& message, const String& context) { 
    StateLockGuard guard;
    String effectiveContext = context; 
    if (effectiveContext.isEmpty()) { 
        effectiveContext = inferDs18b20ContextFromMessage(code, message); 
    } 

    int index = findErrorIndex(code, effectiveContext); 
 
    if (index >= 0) { 
        // Update existing error 
        errorLog[index].severity = severity; 
        errorLog[index].occurrenceCount++; 
        errorLog[index].lastOccurrence = millis(); 
        // Capture actual datetime if NTP is available
        if (ntpManager && ntpManager->isTimeSet()) {
            errorLog[index].lastOccurrenceDateTime = ntpManager->getFormattedDateTime();
        } else {
            errorLog[index].lastOccurrenceDateTime = "NTP not synchronized";
        }
        errorLog[index].resolved = false; 
        errorLog[index].message = message; // Update message in case it's different 
    } else { 
        // Add new error 
        ErrorEntry newEntry(code, severity, message, effectiveContext); 
        // Capture actual datetime if NTP is available 
        if (ntpManager && ntpManager->isTimeSet()) { 
            newEntry.timestampDateTime = ntpManager->getFormattedDateTime(); 
            newEntry.lastOccurrenceDateTime = newEntry.timestampDateTime; 
        } else {
            newEntry.timestampDateTime = "NTP not synchronized";
            newEntry.lastOccurrenceDateTime = "NTP not synchronized";
        }
        addErrorEntry(newEntry);
    }

    // Log to serial for immediate feedback
    UnifiedLogger::error("%s: %s (Code: %d, Severity: %s)\n",
                         errorCodeToString(code).c_str(),
                         message.c_str(),
                         static_cast<int>(code),
                         severityToString(severity).c_str());
 
    // Add to error history 
    addErrorToHistory(code, severity, message, effectiveContext); 

    // Mark error state as dirty for deferred saving
    errorStateDirty = true;
}

std::vector<ErrorEntry> ErrorHandler::getActiveErrors() {
    StateLockGuard guard;
    std::vector<ErrorEntry> activeErrors;
    for (const auto& entry : errorLog) {
        if (!entry.resolved) {
            activeErrors.push_back(entry);
        }
    }
    return activeErrors;
}

int ErrorHandler::getTotalErrorCount() {
    StateLockGuard guard;
    int total = 0;
    for (const auto& entry : errorLog) {
        total += entry.occurrenceCount;
    }
    return total;
}

unsigned long ErrorHandler::getLastErrorTime() {
    StateLockGuard guard;
    unsigned long latestTime = 0;
    for (const auto& entry : errorLog) {
        if (entry.lastOccurrence > latestTime) {
            latestTime = entry.lastOccurrence;
        }
    }
    return latestTime;
}

int ErrorHandler::getErrorsInLastHour() {
    StateLockGuard guard;
    unsigned long oneHourAgo = millis() - (60 * 60 * 1000); // 1 hour in milliseconds
    int count = 0;
    for (const auto& entry : errorLog) {
        if (entry.lastOccurrence > oneHourAgo) {
            count += entry.occurrenceCount;
        }
    }
    return count;
}

bool ErrorHandler::loadErrorState() { 
    StateLockGuard guard;
    if (!LittleFS.exists("/error_state.json")) {
        return true; // No saved state is not an error
    }

    File file = LittleFS.open("/error_state.json", "r");
    if (!file) {
        UnifiedLogger::error("Failed to open error state file\n");
        return false;
    }

    PsramJsonAllocator jsonAllocator;
    JsonDocument doc(&jsonAllocator);
    DeserializationError error = deserializeJson(doc, file);
    file.close();

    if (error) {
        UnifiedLogger::error("Failed to parse error state: %s\n", error.c_str());
        return false;
    }

    errorLog.clear(); 
    JsonArray errors = doc["error_log"]; 
    for (JsonObject errorObj : errors) { 
        // Convert severity string back to enum
        ErrorSeverity severity = ErrorSeverity::INFO; // default
        String severityStr = errorObj["severity"];
        if (severityStr == "WARNING") severity = ErrorSeverity::WARNING;
        else if (severityStr == "ERROR") severity = ErrorSeverity::ERROR;
        else if (severityStr == "FATAL") severity = ErrorSeverity::FATAL;

        const ErrorCode code = static_cast<ErrorCode>(errorObj["code"].as<int>()); 
        const String message = errorObj["message"].as<String>(); 

        String context = ""; 
        if (errorObj["context"].is<String>()) { 
            context = errorObj["context"].as<String>(); 
        } 
        if (context.isEmpty()) { 
            context = inferDs18b20ContextFromMessage(code, message); 
        } 

        ErrorEntry entry(code, severity, message, context); 
        entry.timestamp = errorObj["timestamp"]; 
        entry.occurrenceCount = errorObj["occurrences"]; 
        entry.lastOccurrence = errorObj["last_occurrence"]; 
        entry.resolved = errorObj["resolved"]; 

        // Handle datetime fields (may not exist in older saved data)
        if (errorObj["timestamp_datetime"].is<String>()) {
            entry.timestampDateTime = errorObj["timestamp_datetime"].as<String>();
        } else {
            entry.timestampDateTime = "Unknown (pre-NTP data)";
        }
        if (errorObj["last_occurrence_datetime"].is<String>()) {
            entry.lastOccurrenceDateTime = errorObj["last_occurrence_datetime"].as<String>();
        } else {
            entry.lastOccurrenceDateTime = "Unknown (pre-NTP data)";
        }

        errorLog.push_back(entry);
    }

    UnifiedLogger::info("[ErrorHandler] Loaded %d error entries from persistent storage\n", errorLog.size());
    return true;
}

// Private helper methods
void ErrorHandler::addErrorEntry(const ErrorEntry& entry) {
    StateLockGuard guard;
    errorLog.push_back(entry);
    cleanupOldErrors();
}

int ErrorHandler::findErrorIndex(ErrorCode code, const String& context) { 
    StateLockGuard guard;
    for (size_t i = 0; i < errorLog.size(); ++i) { 
        if (errorLog[i].code == code && errorLog[i].context == context) { 
            return i; 
        } 
    } 
    return -1; 
} 

void ErrorHandler::cleanupOldErrors() {
    StateLockGuard guard;
    // Remove resolved errors if we have too many entries
    if (errorLog.size() > MAX_ERROR_LOG_SIZE) {
        // Remove oldest resolved errors first
        for (auto it = errorLog.begin(); it != errorLog.end(); ) {
            if (it->resolved) {
                it = errorLog.erase(it);
            } else {
                ++it;
            }
        }

        // If still too many, remove oldest errors regardless of resolution status
        while (errorLog.size() > MAX_ERROR_LOG_SIZE) {
            errorLog.erase(errorLog.begin());
        }
    }
}

String ErrorHandler::errorCodeToString(ErrorCode code) {
    switch (code) {
        case ErrorCode::WIFI_CONNECTION_FAILED: return "WIFI_CONNECTION_FAILED";
        case ErrorCode::WIFI_CONNECTION_LOST: return "WIFI_CONNECTION_LOST";
        case ErrorCode::WIFI_INVALID_CREDENTIALS: return "WIFI_INVALID_CREDENTIALS";
        case ErrorCode::MQTT_CONNECTION_FAILED: return "MQTT_CONNECTION_FAILED";
        case ErrorCode::MQTT_CONNECTION_LOST_ERROR: return "MQTT_CONNECTION_LOST_ERROR";
        case ErrorCode::MQTT_INVALID_CREDENTIALS: return "MQTT_INVALID_CREDENTIALS";
        case ErrorCode::MQTT_PUBLISH_FAILED: return "MQTT_PUBLISH_FAILED";
        case ErrorCode::DS18B20_READ_FAILED: return "DS18B20_READ_FAILED";
        case ErrorCode::DS18B20_CRC_ERROR: return "DS18B20_CRC_ERROR";
        case ErrorCode::DS18B20_NO_SENSOR_FOUND: return "DS18B20_NO_SENSOR_FOUND";
        case ErrorCode::SHT4X_READ_FAILED: return "SHT4X_READ_FAILED";
        case ErrorCode::SHT4X_CRC_ERROR: return "SHT4X_CRC_ERROR";
        case ErrorCode::SHT4X_INITIALIZATION_FAILED: return "SHT4X_INITIALIZATION_FAILED";
        case ErrorCode::MAX31855K_READ_FAILED: return "MAX31855K_READ_FAILED";
        case ErrorCode::MAX31855K_FAULT: return "MAX31855K_FAULT";
        case ErrorCode::CONFIG_LOAD_FAILED: return "CONFIG_LOAD_FAILED";
        case ErrorCode::CONFIG_SAVE_FAILED: return "CONFIG_SAVE_FAILED";
        case ErrorCode::CONFIG_VALIDATION_FAILED: return "CONFIG_VALIDATION_FAILED";
        case ErrorCode::CONFIG_FILE_CORRUPTED: return "CONFIG_FILE_CORRUPTED";
        case ErrorCode::MEMORY_ALLOCATION_FAILED: return "MEMORY_ALLOCATION_FAILED";
        case ErrorCode::WATCHDOG_RESET: return "WATCHDOG_RESET";
        case ErrorCode::FILESYSTEM_ERROR: return "FILESYSTEM_ERROR";
        case ErrorCode::FILESYSTEM_FULL: return "FILESYSTEM_FULL";
        case ErrorCode::LED_INITIALIZATION_FAILED: return "LED_INITIALIZATION_FAILED";
        case ErrorCode::LED_UPDATE_FAILED: return "LED_UPDATE_FAILED";
        case ErrorCode::RELAY_TIMEOUT_VIOLATION: return "RELAY_TIMEOUT_VIOLATION";
        case ErrorCode::RELAY_SAFETY_SHUTDOWN: return "RELAY_SAFETY_SHUTDOWN";
        case ErrorCode::IP_WHITELIST_CONFIG_ERROR: return "IP_WHITELIST_CONFIG_ERROR";
        case ErrorCode::IP_WHITELIST_ACCESS_DENIED: return "IP_WHITELIST_ACCESS_DENIED";
        case ErrorCode::IP_WHITELIST_INVALID_FORMAT: return "IP_WHITELIST_INVALID_FORMAT";
        case ErrorCode::SCHEDULE_INVALID_TIME_FORMAT: return "SCHEDULE_INVALID_TIME_FORMAT";
        case ErrorCode::SCHEDULE_INVALID_LEVEL: return "SCHEDULE_INVALID_LEVEL";
        case ErrorCode::SCHEDULE_NTP_NOT_SYNCHRONIZED: return "SCHEDULE_NTP_NOT_SYNCHRONIZED";
        case ErrorCode::SCHEDULE_EXECUTION_FAILED: return "SCHEDULE_EXECUTION_FAILED";
        case ErrorCode::SCHEDULE_CONFIG_LOAD_FAILED: return "SCHEDULE_CONFIG_LOAD_FAILED";
        case ErrorCode::SCHEDULE_TIME_PARSE_ERROR: return "SCHEDULE_TIME_PARSE_ERROR";
        case ErrorCode::SCHEDULE_RELAY_SET_FAILED: return "SCHEDULE_RELAY_SET_FAILED";
        default: return "UNKNOWN_ERROR";
    }
}

String ErrorHandler::severityToString(ErrorSeverity severity) {
    switch (severity) {
        case ErrorSeverity::INFO: return "INFO";
        case ErrorSeverity::WARNING: return "WARNING";
        case ErrorSeverity::ERROR: return "ERROR";
        case ErrorSeverity::FATAL: return "FATAL";
        default: return "UNKNOWN";
    }
}

bool ErrorHandler::saveErrorState() {
    StateLockGuard guard;
    PsramJsonAllocator jsonAllocator;
    JsonDocument doc(&jsonAllocator);
    JsonArray errors = doc["error_log"].to<JsonArray>();

    for (const auto& entry : errorLog) { 
        JsonObject errorObj = errors.add<JsonObject>(); 
        errorObj["code"] = static_cast<int>(entry.code); 
        errorObj["severity"] = severityToString(entry.severity); 
        errorObj["message"] = entry.message; 
        if (!entry.context.isEmpty()) { 
            errorObj["context"] = entry.context; 
        } 
        errorObj["timestamp"] = entry.timestamp; 
        errorObj["occurrences"] = entry.occurrenceCount; 
        errorObj["last_occurrence"] = entry.lastOccurrence; 
        errorObj["resolved"] = entry.resolved; 
        errorObj["timestamp_datetime"] = entry.timestampDateTime; 
        errorObj["last_occurrence_datetime"] = entry.lastOccurrenceDateTime;
    }

    doc["saved_at"] = millis();

    File file = LittleFS.open("/error_state.json", "w");
    if (!file) {
        UnifiedLogger::error("Failed to save error state to filesystem\n");
        return false;
    }

    serializeJson(doc, file);
    file.close();
    return true;
}

// Error History Management Methods
bool ErrorHandler::addErrorToHistory(ErrorCode code, ErrorSeverity severity, 
                                      const String& message, const String& context) { 
    StateLockGuard guard;
    // Check if error with same code already exists in history 
    for (auto& event : errorHistory) { 
        if (event.code == code && event.context == context) { 
            // Update existing error: increment counter, update datetime, and update context 
            event.counter++; 
            event.timestamp = millis(); 
            event.severity = severity; 
            event.message = message; 
            event.context = context; // Update context with latest occurrence 

            // Set datetime if NTP is available
            if (ntpManager && ntpManager->isTimeSet()) {
                event.datetime = ntpManager->getFormattedDateTime();
            } else {
                event.datetime = "NTP not synchronized";
            }

            // Mark error history as dirty for deferred saving
            errorHistoryDirty = true;
            return true;
        }
    }
    
    // Create new error event if not found
    ErrorEvent event;
    event.event_id = nextEventId++;
    event.timestamp = millis();
    event.code = code;
    event.code_name = errorCodeToString(code);
    event.severity = severity;
    event.message = message;
    event.counter = 1;
    event.context = context;
    
    // Set datetime if NTP is available
    if (ntpManager && ntpManager->isTimeSet()) {
        event.datetime = ntpManager->getFormattedDateTime();
        event.datetime_start = event.datetime;
    } else {
        event.datetime = "NTP not synchronized";
        event.datetime_start = "NTP not synchronized";
    }
    
    // Add to history and mark as dirty for deferred saving
    bool result = addHistoryEvent(event);
    if (result) {
        errorHistoryDirty = true;
    }

    return result;
}

bool ErrorHandler::addHistoryEvent(const ErrorEvent& event) {
    StateLockGuard guard;
    errorHistory.push_back(event);
    trimHistoryToLimit();
    return true;
}

void ErrorHandler::trimHistoryToLimit() {
    StateLockGuard guard;
    while (errorHistory.size() > MAX_ERROR_HISTORY) {
        errorHistory.erase(errorHistory.begin()); // Remove oldest
    }
}

String ErrorHandler::getErrorHistoryJson() {
    StateLockGuard guard;
    PsramJsonAllocator jsonAllocator;
    JsonDocument doc(&jsonAllocator);
    
    // Add metadata
    doc["metadata"]["version"] = 1;
    doc["metadata"]["format"] = "ERROR_HISTORY_V1";
    
    // Set created_at to the oldest error timestamp or current time
    if (!errorHistory.empty()) {
        doc["metadata"]["created_at"] = errorHistory.front().datetime;
    } else {
        if (ntpManager && ntpManager->isTimeSet()) {
            doc["metadata"]["created_at"] = ntpManager->getFormattedDateTime();
        } else {
            doc["metadata"]["created_at"] = "NTP not synchronized";
        }
    }
    
    // Set last_updated to current time
    if (ntpManager && ntpManager->isTimeSet()) {
        doc["metadata"]["last_updated"] = ntpManager->getFormattedDateTime();
    } else {
        doc["metadata"]["last_updated"] = "NTP not synchronized";
    }
    
    doc["metadata"]["total_errors"] = errorHistory.size();
    doc["metadata"]["max_errors"] = MAX_ERROR_HISTORY;
    doc["metadata"]["next_event_id"] = nextEventId;
    doc["metadata"]["uncommitted_changes"] = errorHistoryDirty;

    // Add errors array
    JsonArray errors = doc["errors"].to<JsonArray>();
    
    for (const auto& event : errorHistory) {
        JsonObject errorObj = errors.add<JsonObject>();
        errorObj["event_id"] = event.event_id;
        errorObj["timestamp"] = event.timestamp;
        errorObj["datetime"] = event.datetime;
        errorObj["datetime_start"] = event.datetime_start;
        errorObj["code"] = static_cast<int>(event.code);
        errorObj["code_name"] = event.code_name;
        errorObj["severity"] = severityToString(event.severity);
        errorObj["message"] = event.message;
        errorObj["counter"] = event.counter;
        
        if (!event.context.isEmpty()) {
            errorObj["context"] = event.context;
        }
    }
    
    String jsonString;
    serializeJson(doc, jsonString);
    return jsonString;
}

int ErrorHandler::getErrorHistoryCount() {
    StateLockGuard guard;
    return errorHistory.size();
}

bool ErrorHandler::clearErrorHistory() {
    StateLockGuard guard;
    // Clear in-memory data
    errorHistory.clear();
    nextEventId = 1;

    // Save empty history and reset dirty flag on success
    if (saveErrorHistory()) {
        errorHistoryDirty = false;
        UnifiedLogger::info("[ErrorHandler] Error history cleared successfully\n");
        return true;
    } else {
        UnifiedLogger::warning("[ErrorHandler] Failed to save cleared error history\n");
        return false;
    }
}

// Dirty flag flushing methods
void ErrorHandler::flushErrorStateIfDirty() {
    StateLockGuard guard;
    if (errorStateDirty) {
        saveErrorState();
        errorStateDirty = false;
    }
}

void ErrorHandler::flushErrorHistoryIfDirty() {
    StateLockGuard guard;
    if (errorHistoryDirty) {
        saveErrorHistory();
        errorHistoryDirty = false;
    }
}

void ErrorHandler::forceFlushErrorState() {
    StateLockGuard guard;
    saveErrorState();
}

void ErrorHandler::forceFlushErrorHistory() {
    StateLockGuard guard;
    saveErrorHistory();
}

bool ErrorHandler::saveErrorHistory() {
    StateLockGuard guard;
    // Pre-allocate JsonDocument with sufficient size for error history
    PsramJsonAllocator jsonAllocator;
    JsonDocument doc(&jsonAllocator);

    // Add metadata
    doc["metadata"]["version"] = 1;
    doc["metadata"]["format"] = "ERROR_HISTORY_V1";

    // Set created_at to the oldest error timestamp or current time
    if (!errorHistory.empty()) {
        doc["metadata"]["created_at"] = errorHistory.front().datetime;
    } else {
        if (ntpManager && ntpManager->isTimeSet()) {
            doc["metadata"]["created_at"] = ntpManager->getFormattedDateTime();
        } else {
            doc["metadata"]["created_at"] = "NTP not synchronized";
        }
    }

    // Set last_updated to current time
    if (ntpManager && ntpManager->isTimeSet()) {
        doc["metadata"]["last_updated"] = ntpManager->getFormattedDateTime();
    } else {
        doc["metadata"]["last_updated"] = "NTP not synchronized";
    }

    doc["metadata"]["total_errors"] = errorHistory.size();
    doc["metadata"]["max_errors"] = MAX_ERROR_HISTORY;
    doc["metadata"]["next_event_id"] = nextEventId;
    doc["metadata"]["uncommitted_changes"] = errorHistoryDirty;

    // Add errors array
    JsonArray errors = doc["errors"].to<JsonArray>();

    for (const auto& event : errorHistory) {
        JsonObject errorObj = errors.add<JsonObject>();
        errorObj["event_id"] = event.event_id;
        errorObj["timestamp"] = event.timestamp;
        errorObj["datetime"] = event.datetime;
        errorObj["datetime_start"] = event.datetime_start;
        errorObj["code"] = static_cast<int>(event.code);
        errorObj["code_name"] = event.code_name;
        errorObj["severity"] = severityToString(event.severity);
        errorObj["message"] = event.message;
        errorObj["counter"] = event.counter;

        if (!event.context.isEmpty()) {
            errorObj["context"] = event.context;
        }
    }

    // Atomic save process: write to backup file first, then rename
    File backupFile = LittleFS.open(ERROR_HISTORY_BACKUP_FILE, "w");
    if (!backupFile) {
        UnifiedLogger::error("Failed to open error history backup file for writing\n");
        return false;
    }

    // Write to backup file using direct stream serialization
    if (serializeJson(doc, backupFile) == 0) {
        UnifiedLogger::error("Failed to write error history to backup file\n");
        backupFile.close();
        return false;
    }

    backupFile.close();

    // Delete old main file
    if (LittleFS.exists(ERROR_HISTORY_FILE)) {
        if (!LittleFS.remove(ERROR_HISTORY_FILE)) {
            UnifiedLogger::error("Failed to remove old error history file\n");
            return false;
        }
    }

    // Rename backup to main file
    if (!LittleFS.rename(ERROR_HISTORY_BACKUP_FILE, ERROR_HISTORY_FILE)) {
        UnifiedLogger::error("Failed to rename error history backup file\n");
        return false;
    }

    UnifiedLogger::info("[ErrorHandler] Saved %d error events to history\n", errorHistory.size());

    return true;
}

bool ErrorHandler::loadErrorHistory() {
    StateLockGuard guard;
    if (!LittleFS.exists(ERROR_HISTORY_FILE)) {
        UnifiedLogger::info("[ErrorHandler] No error history file found, starting with empty history\n");
        return true; // No saved history is not an error
    }
    
    File file = LittleFS.open(ERROR_HISTORY_FILE, "r");
    if (!file) {
        UnifiedLogger::error("Failed to open error history file\n");
        return false;
    }
    
    PsramJsonAllocator jsonAllocator;
    JsonDocument doc(&jsonAllocator);
    DeserializationError error = deserializeJson(doc, file);
    file.close();
    
    if (error) {
        UnifiedLogger::error("Failed to parse error history: %s\n", error.c_str());
        return false;
    }
    
    // Clear existing history
    errorHistory.clear();
    
    // Load metadata
    if (!doc["metadata"]["next_event_id"].isNull()) {
        nextEventId = doc["metadata"]["next_event_id"];
    } else {
        nextEventId = 1; // Fallback if not present
    }
    
    // Load errors
    JsonArray errors = doc["errors"]; 
    for (JsonObject errorObj : errors) { 
        ErrorEvent event; 
        
        // Required fields
        event.event_id = errorObj["event_id"];
        event.timestamp = errorObj["timestamp"];
        event.datetime = errorObj["datetime"].as<String>();
        event.datetime_start = errorObj["datetime_start"].as<String>();
        event.code = static_cast<ErrorCode>(errorObj["code"].as<int>()); 
        event.code_name = errorObj["code_name"].as<String>(); 
        event.message = errorObj["message"].as<String>(); 
        event.counter = errorObj["counter"]; 
        
        // Optional fields
        if (errorObj["severity"].is<String>()) {
            String severityStr = errorObj["severity"];
            if (severityStr == "WARNING") event.severity = ErrorSeverity::WARNING;
            else if (severityStr == "ERROR") event.severity = ErrorSeverity::ERROR;
            else if (severityStr == "FATAL") event.severity = ErrorSeverity::FATAL;
            else event.severity = ErrorSeverity::INFO; // default
        } else {
            event.severity = ErrorSeverity::INFO; // default
        }
        
        if (errorObj["context"].is<String>()) { 
            event.context = errorObj["context"].as<String>(); 
        } 
        if (event.context.isEmpty()) { 
            event.context = inferDs18b20ContextFromMessage(event.code, event.message); 
        } 
         
        errorHistory.push_back(event); 
    } 
    
    // Ensure we don't exceed the limit after loading
    trimHistoryToLimit();
    
    UnifiedLogger::info("[ErrorHandler] Loaded %d error events from history\n", errorHistory.size());
    return true;
}
