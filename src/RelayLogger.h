#ifndef RELAYLOGGER_H
#define RELAYLOGGER_H

#include <Arduino.h>
#include <vector>

// Forward declarations
class NTPManager;
class MainController;

// Relay event structure for aggregation
struct RelayEvent {
    unsigned long event_id;
    unsigned long timestamp;
    String datetime;
    String datetime_start;  // First occurrence
    String previous_state;
    String new_state;
    String requested_by;
    String result;          // "success" or aggregated failure types
    int counter;            // Occurrence count
    String context;         // Optional additional info
};

class RelayLogger {
public:
    // NTP Manager setup
    static void setNTPManager(NTPManager* ntpMgr);

    // MainController setup for performance monitoring
    static void setMainController(MainController* mc);

    // Initialize logger
    static void begin();

    // Log a relay state change
    static bool logEvent(const String& previousState, const String& newState, 
                        const String& requestedBy, bool success);

    // Get logs as JSON for API
    static String getLogsJson();

    // Get current log count
    static int getLogCount();

    // Clear all logs
    static bool clearLogs();

    // Flush relay events to filesystem if dirty
    static void flushRelayEventsIfDirty();

    // Force flush relay events to filesystem regardless of dirty flag state
    static void forceFlushRelayEvents();

    // Load relay events from filesystem

private:
    static NTPManager* ntpManager;
    static MainController* mainController;
    static std::vector<RelayEvent> relayEvents;  // Replace logBuffer
    static unsigned long nextEventId;
    static bool relayEventsDirty;                // Dirty flag for deferred saves
    static const int MAX_RELAY_EVENTS = 100;     // Reasonable limit for aggregated events

    // Helper methods
    static bool saveRelayEvents();
    static bool loadRelayEvents();
    static bool addRelayEvent(const String& previousState, const String& newState,
                             const String& requestedBy, const String& result,
                             const String& context = "");
    static void trimEventsToLimit();
};

// File paths for relay events
const char RELAY_EVENTS_FILE[] = "/relay_events.json";
const char RELAY_EVENTS_BACKUP_FILE[] = "/relay_events_backup.json";

#endif // RELAYLOGGER_H