#ifndef SCHEDULEMANAGER_H
#define SCHEDULEMANAGER_H

#include <Arduino.h>
#include <vector>
#include <time.h>
#include "ConfigManager.h"
#include "NTPManager.h"
#include "RelayManager.h"
#include "ErrorHandler.h"

// Forward declaration to avoid circular dependency
class MainController;

class ScheduleManager {
public:
    ScheduleManager();
    ~ScheduleManager();

    // Initialize schedule manager with dependencies
    void begin(ConfigManager& configManager, NTPManager& ntpManager, RelayManager& relayManager);

    // NTP-based update method - called regularly from main loop
    void update();

    // Get next scheduled event information
    String getNextScheduledEvent() const;

    // Get all schedules as JSON for API
    String getSchedulesJson() const;

    // Force reload schedules from config
    void reloadSchedules();

    // Check if schedule manager is initialized
    bool isInitialized() const { return initialized; }

    // Set MainController reference for performance monitoring
    void setMainController(MainController* controller);


    // Update safety settings based on current configuration
    void updateSafetySettings();

    // Get performance metrics as JSON for monitoring
    String getPerformanceMetricsJson() const;

    // Run comprehensive self-tests and return detailed results
    String runSelfTests();

private:
    ConfigManager* configManagerPtr;
    NTPManager* ntpManagerPtr;
    RelayManager* relayManagerPtr;
    bool initialized;

    // Fallback and recovery state
    RelayMode lastSuccessfulMode;
    unsigned long lastSuccessfulTime;
    bool ntpPreviouslySynced;
    static const unsigned long MAX_NTP_RETRY_PERIOD = 300000; // 5 minutes

    // NTP-based scheduling state
    time_t lastCheckedTime;
    RelayMode currentRelayMode;
    
    // Enhanced state tracking for Phase 2
    RelayMode lastAppliedRelayMode;      // Last successfully applied mode
    unsigned long lastStateChangeTime;   // Timestamp of last state change
    int consecutiveFailureCount;        // Track consecutive failures
    
    // Performance monitoring
    unsigned long lastUpdateDuration;    // Track update() method duration
    int updateCallCount;                // Count update calls
    unsigned long totalUpdateTime;      // Total time spent in updates
    
    // MainController reference for performance monitoring
    MainController* mainControllerPtr;

    // Cached configuration data for performance optimization
    mutable std::vector<Settings::Schedule::ScheduleEntry> cachedSchedules;
    mutable time_t lastCacheUpdateTime;
    mutable bool cacheValid;

    // Runtime logging control is handled globally by UnifiedLogger.
    static const unsigned long SCHEDULE_MODE_LOG_INTERVAL_MS = 30000UL;  // 30s
    static const unsigned long SCHEDULE_PERF_LOG_INTERVAL_MS = 60000UL;  // 60s
    unsigned long lastStateSyncLogMs;
    unsigned long lastRelayModeChangeLogMs;
    unsigned long lastRelayModeUpdateSuccessLogMs;
    unsigned long lastSlowUpdateLogMs;

    // Helper methods
    bool shouldEmitRateLimitedLog(unsigned long& lastLogMs, unsigned long intervalMs);
    RelayMode determineCurrentRelayMode(time_t currentTime);
    RelayMode determineCurrentRelayLevel(const std::vector<Settings::Schedule::ScheduleEntry>& activeRanges, time_t currentTime);
    bool isTimeInRange(const Settings::Schedule::ScheduleEntry& range, time_t currentTime);

    time_t parseTimeToMinutes(const String& timeStr) const;

    // Time parsing and comparison
    time_t parseScheduleTime(int day, const String& timeStr, time_t currentTime) const;
    int getCurrentDayOfWeek(time_t currentTime) const;
    bool isSameDay(time_t time1, time_t time2) const;
    time_t getNextOccurrence(const Settings::Schedule::ScheduleEntry& schedule, time_t currentTime, bool useStartTime = true) const;

    // Relay control
    RelayMode levelStringToMode(const String& level);
    String modeToString(RelayMode mode);

    // Schedule validation
    bool isValidScheduleEntry(const Settings::Schedule::ScheduleEntry& schedule);

    // Cache management
    void updateScheduleCache();
    const std::vector<Settings::Schedule::ScheduleEntry>& getCachedSchedules();
};

#endif // SCHEDULEMANAGER_H
