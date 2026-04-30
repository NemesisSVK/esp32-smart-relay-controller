#include "NTPManager.h"
#include <time.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <esp_sntp.h>
#include "UnifiedLogger.h"
#include "MainController.h"
#include "PsramJsonAllocator.h"

NTPManager::NTPManager() :
    mainController(nullptr),
    lastSyncTime(0),
    lastSyncStatus("Not initialized"),
    lastSyncServer(""),
    lastSyncDetailedStatus("Not initialized"),
    lastSyncServerIndex(-1),
    lastSyncSuccessful(false),
    syncInProgress(false),
    syncServerIndexInProgress(-1),
    syncServerStartMs(0),
    hadValidTimeBeforeSync(false),
    ntpLogBufferIndex(0),
    ntpLogCount(0),
    lastTimezoneLogMs(0) {
    // Initialize circular buffer
    for (int i = 0; i < NTP_LOG_BUFFER_SIZE; i++) {
        ntpLogBuffer[i].timestamp = 0;
        ntpLogBuffer[i].server = "";
        ntpLogBuffer[i].success = false;
        ntpLogBuffer[i].status_message = "";
        ntpLogBuffer[i].server_index = -1;
    }

    for (int i = 0; i < 3; i++) {
        lastServerTestLogMs[i] = 0;
        lastServerConfigLogMs[i] = 0;
        lastSyncAttemptLogMs[i] = 0;
    }

    // Load existing logs from storage
    loadNtpLogsFromStorage();
}

NTPManager::~NTPManager() {
    // Cleanup if needed
}

bool NTPManager::shouldLogNtpProgress(unsigned long& lastLogMs, unsigned long intervalMs) {
    const unsigned long now = millis();
    if (lastLogMs == 0 || (now - lastLogMs) >= intervalMs) {
        lastLogMs = now;
        return true;
    }
    return false;
}

void NTPManager::setMainController(MainController* mc) {
    mainController = mc;
}

bool NTPManager::setupNTP(const String& timezone, const String servers[3]) {
    timezoneString = timezone;

    for (int i = 0; i < 3; i++) {
        ntpServers[i] = servers[i];
    }

    if (!validateTimezone(timezoneString)) {
        lastSyncStatus = "Invalid timezone format";
        return false;
    }

    // Configure NTP with timezone
    configTzTime(timezoneString.c_str(),
                 getServer(0),
                 getServer(1),
                 getServer(2));

    lastSyncStatus = "NTP configured";
    UnifiedLogger::info("NTP configured with timezone: %s\n", timezoneString.c_str());
    UnifiedLogger::info("NTP servers: %s, %s, %s\n",
                  getServer(0), getServer(1), getServer(2));

    return true;
}

bool NTPManager::syncTime() {
    static bool warnedBlockingSyncUsage = false;
    if (!warnedBlockingSyncUsage) {
        warnedBlockingSyncUsage = true;
        UnifiedLogger::warning("NTP syncTime() is blocking compatibility API; prefer beginSync()/processSync().\n");
    }

    beginSync();
    while (isSyncInProgress()) {
        (void)processSync();
        delay(1);
    }
    return wasLastSyncSuccessful();
}

void NTPManager::beginSync() {
    if (syncInProgress) {
        return;
    }

    hadValidTimeBeforeSync = isTimeSet();
    syncInProgress = true;
    syncServerIndexInProgress = -1;
    syncServerStartMs = 0;

    for (int i = 0; i < 3; i++) {
        if (ntpServers[i].length() == 0) {
            continue;
        }

        syncServerIndexInProgress = i;
        lastSyncStatus = "CONNECTING";

        if (shouldLogNtpProgress(lastServerTestLogMs[i])) {
            UnifiedLogger::info("Testing NTP server %d: %s\n", i, ntpServers[i].c_str());
        }
        if (shouldLogNtpProgress(lastServerConfigLogMs[i])) {
            UnifiedLogger::info("Configuring NTP with server: %s\n", ntpServers[i].c_str());
        }

        // Configure NTP with single server - use only the current server.
        sntp_stop();
        configTzTime(timezoneString.c_str(), ntpServers[i].c_str());

        syncServerStartMs = millis();
        return;
    }

    // No configured servers.
    syncInProgress = false;
    lastSyncSuccessful = false;
    lastSyncDetailedStatus = "Sync failed - no servers configured";
    lastSyncStatus = "CONNECTING";

    time_t now = time(nullptr);
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);
    char datetimeBuffer[25];
    strftime(datetimeBuffer, sizeof(datetimeBuffer), "%Y-%m-%d %H:%M:%S", &timeinfo);
    String statusMsg = "Sync failed - " + String(datetimeBuffer);

    logNTPSyncEvent("All servers", false, statusMsg, -1);
    saveNtpLogsToStorage();

    UnifiedLogger::error("NTP sync failed: no servers configured\n");
}

bool NTPManager::processSync() {
    if (!syncInProgress) {
        return false;
    }

    if (syncServerIndexInProgress < 0 || syncServerIndexInProgress >= 3 || ntpServers[syncServerIndexInProgress].length() == 0) {
        syncInProgress = false;
        lastSyncSuccessful = false;
        lastSyncDetailedStatus = "Sync failed - invalid server state";
        lastSyncStatus = "CONNECTING";
        return true;
    }

    const int i = syncServerIndexInProgress;
    const bool sntpSynced = (sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED);
    const unsigned long elapsedMs = millis() - syncServerStartMs;

    if (!sntpSynced && elapsedMs < NTP_SERVER_TIMEOUT_MS) {
        return false;
    }

    // Additional debugging - check timezone configuration.
    if (shouldLogNtpProgress(lastTimezoneLogMs, 300000UL)) {
        UnifiedLogger::info("NTP timezone: %s\n", timezoneString.c_str());
    }
    if (shouldLogNtpProgress(lastSyncAttemptLogMs[i])) {
        UnifiedLogger::info("Attempting NTP sync with %s (10 second timeout)...\n", ntpServers[i].c_str());
    }

    if (sntpSynced) {
        time_t now = time(nullptr);

        // Verify we got a valid recent time (not cached old time).
        if (now > 1577836800) {  // After 2020
            lastSyncTime = now;
            lastSyncServer = ntpServers[i];
            lastSyncServerIndex = i;
            lastSyncSuccessful = true;
            lastSyncDetailedStatus = "Sync successful - " + ntpServers[i];
            lastSyncStatus = "SYNCHRONIZED";

            struct tm timeinfo;
            localtime_r(&now, &timeinfo);
            char datetimeBuffer[25];
            strftime(datetimeBuffer, sizeof(datetimeBuffer), "%Y-%m-%d %H:%M:%S", &timeinfo);
            String statusMsg = "Sync successful - " + String(datetimeBuffer);

            logNTPSyncEvent(ntpServers[i], true, statusMsg, i);
            saveNtpLogsToStorage();

            UnifiedLogger::info("NTP sync successful using %s. Current time: %s", ntpServers[i].c_str(), ctime(&now));

            syncInProgress = false;
            return true;
        }

        // Got old/cached time, not a real sync.
        UnifiedLogger::warning("NTP sync for %s returned old time (%lu), trying next server\n", ntpServers[i].c_str(), static_cast<unsigned long>(now));
    } else {
        UnifiedLogger::warning("NTP sync failed for server %s (SNTP did not complete within timeout), trying next server\n", ntpServers[i].c_str());
    }

    // Current server failed - log and move to next.
    time_t now = time(nullptr);
    if (!hadValidTimeBeforeSync && now <= 0) {
        // If we had no valid time before and still have none, keep deterministic timestamp.
        now = 0;
    }

    struct tm timeinfo;
    localtime_r(&now, &timeinfo);
    char datetimeBuffer[25];
    strftime(datetimeBuffer, sizeof(datetimeBuffer), "%Y-%m-%d %H:%M:%S", &timeinfo);
    String statusMsg = "Sync failed - " + String(datetimeBuffer);

    logNTPSyncEvent(ntpServers[i], false, statusMsg, i);
    saveNtpLogsToStorage();

    for (int next = i + 1; next < 3; next++) {
        if (ntpServers[next].length() == 0) {
            continue;
        }

        syncServerIndexInProgress = next;

        if (shouldLogNtpProgress(lastServerTestLogMs[next])) {
            UnifiedLogger::info("Testing NTP server %d: %s\n", next, ntpServers[next].c_str());
        }
        if (shouldLogNtpProgress(lastServerConfigLogMs[next])) {
            UnifiedLogger::info("Configuring NTP with server: %s\n", ntpServers[next].c_str());
        }

        sntp_stop();
        configTzTime(timezoneString.c_str(), ntpServers[next].c_str());
        syncServerStartMs = millis();
        return false;
    }

    // All servers failed.
    syncInProgress = false;
    lastSyncSuccessful = false;
    lastSyncDetailedStatus = "Sync failed - all servers unreachable";
    lastSyncStatus = "CONNECTING";

    now = time(nullptr);
    localtime_r(&now, &timeinfo);
    strftime(datetimeBuffer, sizeof(datetimeBuffer), "%Y-%m-%d %H:%M:%S", &timeinfo);
    statusMsg = "Sync failed - " + String(datetimeBuffer);

    logNTPSyncEvent("All servers", false, statusMsg, -1);
    saveNtpLogsToStorage();

    UnifiedLogger::error("NTP sync failed: all servers unreachable\n");
    return true;
}

bool NTPManager::isSyncInProgress() const {
    return syncInProgress;
}

time_t NTPManager::getCurrentTime() const {
    if (!isTimeSet()) {
        return 0;
    }
    return time(nullptr);
}

String NTPManager::getFormattedTime() const {
    if (!isTimeSet()) {
        return "Time not set";
    }

    time_t now = time(nullptr);
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);

    char buffer[9];  // HH:MM:SS + null
    strftime(buffer, sizeof(buffer), "%H:%M:%S", &timeinfo);
    return String(buffer);
}

String NTPManager::getFormattedDateTime() const {
    if (!isTimeSet()) {
        return "Time not set";
    }

    time_t now = time(nullptr);
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);

    char buffer[20];  // YYYY-MM-DD HH:MM:SS + null
    strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &timeinfo);
    return String(buffer);
}

bool NTPManager::isTimeSet() const {
    time_t now = time(nullptr);
    // Time is set if it's after 2020 (reasonable check for NTP sync)
    return now > 1577836800;  // 2020-01-01 00:00:00 UTC
}

String NTPManager::getLastSyncStatus() const {
    return lastSyncStatus;
}

time_t NTPManager::getLastSyncTime() const {
    return lastSyncTime;
}

// Enhanced status methods
String NTPManager::getLastSyncServer() const {
    return lastSyncServer;
}

String NTPManager::getLastSyncDetailedStatus() const {
    return lastSyncDetailedStatus;
}

int NTPManager::getLastSyncServerIndex() const {
    return lastSyncServerIndex;
}

bool NTPManager::wasLastSyncSuccessful() const {
    return lastSyncSuccessful;
}

void NTPManager::logNTPSyncEvent(const String& server, bool success, const String& status, int serverIndex) {
    // Add new log entry to circular buffer
    NTPSyncLog& logEntry = ntpLogBuffer[ntpLogBufferIndex];
    logEntry.timestamp = time(nullptr);
    logEntry.server = server;
    logEntry.success = success;
    logEntry.status_message = status;
    logEntry.server_index = serverIndex;
    
    // Update buffer index and count
    ntpLogBufferIndex = (ntpLogBufferIndex + 1) % NTP_LOG_BUFFER_SIZE;
    if (ntpLogCount < NTP_LOG_BUFFER_SIZE) {
        ntpLogCount++;
    }
}

String NTPManager::getNtpLogsJson() const {
    String json = "{\n  \"logs\": [\n";
    
    bool firstEntry = true;
    
    // Start from the oldest entry (after current index) and wrap around
    for (int i = 0; i < ntpLogCount; i++) {
        int index = (ntpLogBufferIndex - ntpLogCount + i + NTP_LOG_BUFFER_SIZE) % NTP_LOG_BUFFER_SIZE;
        const NTPSyncLog& logEntry = ntpLogBuffer[index];
        
        if (!firstEntry) {
            json += ",\n";
        }
        firstEntry = false;
        
        json += "    {\n";
        json += "      \"timestamp\": ";
        json += String(logEntry.timestamp);
        json += ",\n";
        
        json += "      \"server\": \"";
        json += logEntry.server;
        json += "\",\n";
        
        json += "      \"success\": ";
        json += logEntry.success ? "true" : "false";
        json += ",\n";
        
        json += "      \"status_message\": \"";
        json += logEntry.status_message;
        json += "\",\n";
        
        json += "      \"server_index\": ";
        json += String(logEntry.server_index);
        json += "\n";
        
        json += "    }";
    }
    
    json += "\n  ],\n";
    json += "  \"count\": ";
    json += String(ntpLogCount);
    json += ",\n";
    json += "  \"max_entries\": ";
    json += String(NTP_LOG_BUFFER_SIZE);
    json += "\n";
    json += "}\n";
    
    return json;
}

void NTPManager::printTimeInfo() const {
    if (!isTimeSet()) {
        UnifiedLogger::warning("Time not synchronized\n");
        return;
    }

    time_t now = time(nullptr);
    UnifiedLogger::info("Current time: %s", ctime(&now));
    UnifiedLogger::info("Last sync: %s", lastSyncStatus.c_str());
    if (lastSyncTime > 0) {
        UnifiedLogger::info("Last sync time: %s", ctime(&lastSyncTime));
    }
}

const char* NTPManager::getServer(int index) const {
    if (index >= 0 && index < 3 && ntpServers[index].length() > 0) {
        return ntpServers[index].c_str();
    }
    return nullptr;
}

bool NTPManager::validateTimezone(const String& tz) const {
    // Basic validation - should contain timezone info
    // More complex validation could be added
    return tz.length() > 0 && tz.length() <= 200;
}

// LittleFS Storage Implementation
bool NTPManager::initializeStorage() {
    if (!LittleFS.begin()) {
        UnifiedLogger::warning("LittleFS mount failed for NTP logs, attempting to format...\n");
        if (LittleFS.format()) {
            if (!LittleFS.begin()) {
                UnifiedLogger::error("Failed to mount LittleFS after format\n");
                return false;
            }
        } else {
            UnifiedLogger::error("Failed to format LittleFS\n");
            return false;
        }
    }
    return true;
}

bool NTPManager::saveNtpLogsToStorage() {
    unsigned long startTime = micros();

    if (!initializeStorage()) {
        UnifiedLogger::error("Storage initialization failed, cannot save NTP logs\n");
        return false;
    }

    // Serialize to backup file first
    String tempFilename = "/ntp_logs_backup.json";
    String mainFilename = "/ntp_logs.json";

    File file = LittleFS.open(tempFilename, "w");
    if (!file) {
        UnifiedLogger::error("Failed to open NTP logs backup file for writing\n");
        return false;
    }

    // Write JSON content
    String jsonContent = serializeLogsToJson();
    if (file.print(jsonContent) == 0) {
        UnifiedLogger::error("Failed to write NTP logs to backup file\n");
        file.close();
        return false;
    }
    file.close();

    // Atomic swap: remove old file, rename new file
    if (LittleFS.exists(mainFilename)) {
        LittleFS.remove(mainFilename);
    }
    if (!LittleFS.rename(tempFilename, mainFilename)) {
        UnifiedLogger::error("Failed to rename NTP logs file\n");
        return false;
    }

    UnifiedLogger::info("NTP logs saved to LittleFS successfully\n");


    return true;
}

bool NTPManager::loadNtpLogsFromStorage() {
    if (!initializeStorage()) {
        UnifiedLogger::error("Storage initialization failed, cannot load NTP logs\n");
        return false;
    }

    String mainFilename = "/ntp_logs.json";

    if (!LittleFS.exists(mainFilename)) {
        UnifiedLogger::info("No NTP logs file found, starting with empty logs\n");
        return true; // Not an error if file doesn't exist yet
    }

    File file = LittleFS.open(mainFilename, "r");
    if (!file) {
        UnifiedLogger::error("Failed to open NTP logs file for reading\n");
        return false;
    }

    // Read file content
    String jsonContent = "";
    while (file.available()) {
        jsonContent += char(file.read());
    }
    file.close();

    if (jsonContent.length() == 0) {
        UnifiedLogger::warning("NTP logs file is empty\n");
        return true;
    }

    // Deserialize and load into buffer
    if (!deserializeLogsFromJson(jsonContent)) {
        UnifiedLogger::error("Failed to deserialize NTP logs from JSON\n");
        return false;
    }

    UnifiedLogger::info("NTP logs loaded from LittleFS successfully\n");
    return true;
}

String NTPManager::serializeLogsToJson() const {
    // Create JSON document - ArduinoJson v7 handles memory allocation automatically
    PsramJsonAllocator jsonAllocator;
    JsonDocument doc(&jsonAllocator);

    // Create metadata
    JsonObject metadata = doc["metadata"].to<JsonObject>();
    metadata["version"] = 1;
    metadata["max_entries"] = NTP_LOG_BUFFER_SIZE;
    metadata["last_updated"] = time(nullptr);
    metadata["format"] = "NTP_LOG_V1";

    // Create logs array
    JsonArray logs = doc["logs"].to<JsonArray>();

    // Add logs from circular buffer (oldest to newest)
    for (int i = 0; i < ntpLogCount; i++) {
        int index = (ntpLogBufferIndex - ntpLogCount + i + NTP_LOG_BUFFER_SIZE) % NTP_LOG_BUFFER_SIZE;
        const NTPSyncLog& logEntry = ntpLogBuffer[index];

        if (logEntry.timestamp > 0) { // Only include valid entries
            JsonObject logObj = logs.add<JsonObject>();
            logObj["timestamp"] = logEntry.timestamp;
            
            // Format datetime string
            struct tm timeinfo;
            localtime_r(&logEntry.timestamp, &timeinfo);
            char datetimeBuffer[25];
            strftime(datetimeBuffer, sizeof(datetimeBuffer), "%Y-%m-%dT%H:%M:%SZ", &timeinfo);
            logObj["datetime"] = datetimeBuffer;
            
            logObj["server"] = logEntry.server;
            logObj["success"] = logEntry.success;
            logObj["status"] = logEntry.status_message;
            logObj["serverIndex"] = logEntry.server_index;
            logObj["syncDurationMs"] = 0; // Placeholder for future use
        }
    }

    // Serialize to string
    String jsonString;
    serializeJson(doc, jsonString);
    return jsonString;
}

bool NTPManager::deserializeLogsFromJson(const String& jsonContent) {
    // Create JSON document - ArduinoJson v7 handles memory allocation automatically
    PsramJsonAllocator jsonAllocator;
    JsonDocument doc(&jsonAllocator);

    // Parse JSON
    DeserializationError error = deserializeJson(doc, jsonContent);
    if (error) {
        UnifiedLogger::error("Failed to parse NTP logs JSON: %s\n", error.c_str());
        return false;
    }

    // Clear current buffer
    ntpLogBufferIndex = 0;
    ntpLogCount = 0;
    
    // Initialize circular buffer
    for (int i = 0; i < NTP_LOG_BUFFER_SIZE; i++) {
        ntpLogBuffer[i].timestamp = 0;
        ntpLogBuffer[i].server = "";
        ntpLogBuffer[i].success = false;
        ntpLogBuffer[i].status_message = "";
        ntpLogBuffer[i].server_index = -1;
    }

    // Load logs from JSON
    if (!doc["logs"].isNull() && doc["logs"].is<JsonArray>()) {
        JsonArray logs = doc["logs"];
        
        for (JsonObject logObj : logs) {
            if (ntpLogCount >= NTP_LOG_BUFFER_SIZE) {
                break; // Buffer full
            }

            NTPSyncLog& logEntry = ntpLogBuffer[ntpLogBufferIndex];
            
            // Parse log entry
            if (!logObj["timestamp"].isNull()) {
                logEntry.timestamp = logObj["timestamp"];
            }
            if (!logObj["server"].isNull()) {
                logEntry.server = logObj["server"].as<String>();
            }
            if (!logObj["success"].isNull()) {
                logEntry.success = logObj["success"];
            }
            if (!logObj["status"].isNull()) {
                logEntry.status_message = logObj["status"].as<String>();
            }
            if (!logObj["serverIndex"].isNull()) {
                logEntry.server_index = logObj["serverIndex"];
            }
            
            // Update buffer index and count
            ntpLogBufferIndex = (ntpLogBufferIndex + 1) % NTP_LOG_BUFFER_SIZE;
            ntpLogCount++;
        }
    }

    return true;
}
