#ifndef NTPMANAGER_H
#define NTPMANAGER_H

#include <Arduino.h>
#include <time.h>
#include <LittleFS.h>
#include <ArduinoJson.h>

// Forward declaration
class MainController;

#if defined(__GNUC__) || defined(__clang__)
#define NTP_DEPRECATED(msg) __attribute__((deprecated(msg)))
#else
#define NTP_DEPRECATED(msg)
#endif

// NTP Sync Log structure
struct NTPSyncLog {
    time_t timestamp;
    String server;
    bool success;
    String status_message;
    int server_index;
};

class NTPManager {
public:
    NTPManager();
    ~NTPManager();

    // Setup NTP with configuration (always enabled)
    bool setupNTP(const String& timezone, const String servers[3]);

    // Synchronize time (legacy blocking compatibility wrapper).
    // Do not call from loop() or HTTP handlers; use beginSync()/processSync().
    bool syncTime() NTP_DEPRECATED("NTPManager::syncTime() is blocking; use beginSync()/processSync()");
    void beginSync();
    bool processSync();
    bool isSyncInProgress() const;

    // Get current time information
    time_t getCurrentTime() const;
    String getFormattedTime() const;
    String getFormattedDateTime() const;
    bool isTimeSet() const;

    // Status information
    String getLastSyncStatus() const;
    time_t getLastSyncTime() const;

    // Enhanced status information
    String getLastSyncServer() const;
    String getLastSyncDetailedStatus() const;
    int getLastSyncServerIndex() const;
    bool wasLastSyncSuccessful() const;

    // Utility functions
    void printTimeInfo() const;

    // MainController setup for performance monitoring
    void setMainController(MainController* mc);

    // Public method to get logs for WebServerManager
    String getNtpLogsForApi() const { return getNtpLogsJson(); }

private:
    MainController* mainController;
    bool ntpEnabled;
    String timezoneString;
    String ntpServers[3];
    time_t lastSyncTime;
    String lastSyncStatus;

    // Enhanced tracking for detailed sync information
    String lastSyncServer;
    String lastSyncDetailedStatus;
    int lastSyncServerIndex;
    bool lastSyncSuccessful;

    // Non-blocking sync state
    bool syncInProgress;
    int syncServerIndexInProgress;
    unsigned long syncServerStartMs;
    bool hadValidTimeBeforeSync;
    static constexpr unsigned long NTP_SERVER_TIMEOUT_MS = 10000UL;
    static constexpr unsigned long NTP_PROGRESS_LOG_INTERVAL_MS = 60000UL; // 60s

    // Circular buffer for NTP sync logs (last 10 entries)
    static const int NTP_LOG_BUFFER_SIZE = 10;
    NTPSyncLog ntpLogBuffer[NTP_LOG_BUFFER_SIZE];
    int ntpLogBufferIndex;
    int ntpLogCount;
    unsigned long lastServerTestLogMs[3];
    unsigned long lastServerConfigLogMs[3];
    unsigned long lastSyncAttemptLogMs[3];
    unsigned long lastTimezoneLogMs;

    // Helper functions
    const char* getServer(int index) const;
    bool validateTimezone(const String& tz) const;

    // NTP Log methods
    void logNTPSyncEvent(const String& server, bool success, const String& status, int serverIndex);
    String getNtpLogsJson() const;

    // LittleFS Storage methods
    bool initializeStorage();
    bool saveNtpLogsToStorage();
    bool loadNtpLogsFromStorage();
    String serializeLogsToJson() const;
    bool deserializeLogsFromJson(const String& jsonContent);
    bool shouldLogNtpProgress(unsigned long& lastLogMs, unsigned long intervalMs = NTP_PROGRESS_LOG_INTERVAL_MS);
};

#endif // NTPMANAGER_H
