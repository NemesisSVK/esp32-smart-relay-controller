#include "MainController.h"
#include "RelayLogger.h"
#include "NTPManager.h"
#include <LittleFS.h>
#include <ArduinoJson.h>
#include "PsramJsonAllocator.h"
#include "UnifiedLogger.h"

// Static member initialization
NTPManager* RelayLogger::ntpManager = nullptr;
MainController* RelayLogger::mainController = nullptr;
std::vector<RelayEvent> RelayLogger::relayEvents;
unsigned long RelayLogger::nextEventId = 1;
bool RelayLogger::relayEventsDirty = false;

void RelayLogger::setNTPManager(NTPManager* ntpMgr) {
    ntpManager = ntpMgr;
}

void RelayLogger::setMainController(MainController* mc) {
    mainController = mc;
}

void RelayLogger::begin() {
    UnifiedLogger::info("[RelayLogger] Initializing...\n");

    // Load existing relay events from filesystem
    if (!loadRelayEvents()) {
        UnifiedLogger::warning("[RelayLogger] Could not load existing relay events, starting fresh\n");
    }
}

bool RelayLogger::logEvent(const String& previousState, const String& newState,
                          const String& requestedBy, bool success) {
    String result = success ? "success" : "fail";
    return addRelayEvent(previousState, newState, requestedBy, result);
}

String RelayLogger::getLogsJson() {
    PsramJsonAllocator jsonAllocator;
    JsonDocument doc(&jsonAllocator);

    // Add metadata
    JsonObject metadata = doc["metadata"].to<JsonObject>();
    metadata["version"] = 1;
    metadata["format"] = "RELAY_EVENTS_V1";

    // Set created_at to the oldest event or current time
    if (!relayEvents.empty()) {
        metadata["created_at"] = relayEvents.front().datetime_start;
    } else {
        if (ntpManager && ntpManager->isTimeSet()) {
            metadata["created_at"] = ntpManager->getFormattedDateTime();
        } else {
            metadata["created_at"] = "NTP not synchronized";
        }
    }

    // Set last_updated to current time
    if (ntpManager && ntpManager->isTimeSet()) {
        metadata["last_updated"] = ntpManager->getFormattedDateTime();
    } else {
        metadata["last_updated"] = "NTP not synchronized";
    }

    metadata["total_events"] = relayEvents.size();
    metadata["max_events"] = MAX_RELAY_EVENTS;
    metadata["next_event_id"] = nextEventId;
    metadata["uncommitted_changes"] = relayEventsDirty;

    // Add events array
    JsonArray events = doc["events"].to<JsonArray>();

    for (const auto& event : relayEvents) {
        JsonObject eventObj = events.add<JsonObject>();
        eventObj["event_id"] = event.event_id;
        eventObj["timestamp"] = event.timestamp;
        eventObj["datetime"] = event.datetime;
        eventObj["datetime_start"] = event.datetime_start;
        eventObj["previous_state"] = event.previous_state;
        eventObj["new_state"] = event.new_state;
        eventObj["requested_by"] = event.requested_by;
        eventObj["result"] = event.result;
        eventObj["counter"] = event.counter;

        if (!event.context.isEmpty()) {
            eventObj["context"] = event.context;
        }
    }

    String jsonString;
    serializeJson(doc, jsonString);
    return jsonString;
}

int RelayLogger::getLogCount() {
    return relayEvents.size();
}

bool RelayLogger::clearLogs() {
    // Clear in-memory data
    relayEvents.clear();
    nextEventId = 1;

    // Reset dirty flag to prevent confusion with periodic flush
    relayEventsDirty = false;

    // Remove existing files
    bool fileRemovalSuccess = true;
    if (LittleFS.exists(RELAY_EVENTS_FILE)) {
        if (!LittleFS.remove(RELAY_EVENTS_FILE)) {
            UnifiedLogger::warning("[RelayLogger] Failed to remove main relay events file\n");
            fileRemovalSuccess = false;
        }
    }
    if (LittleFS.exists(RELAY_EVENTS_BACKUP_FILE)) {
        if (!LittleFS.remove(RELAY_EVENTS_BACKUP_FILE)) {
            UnifiedLogger::warning("[RelayLogger] Failed to remove backup relay events file\n");
            fileRemovalSuccess = false;
        }
    }

    if (fileRemovalSuccess) {
        UnifiedLogger::info("[RelayLogger] All relay events cleared successfully\n");
    } else {
        UnifiedLogger::warning("[RelayLogger] Relay events cleared from memory, but some files may remain\n");
    }

    return fileRemovalSuccess;
}

bool RelayLogger::loadRelayEvents() {
    if (!LittleFS.exists(RELAY_EVENTS_FILE)) {
        UnifiedLogger::info("[RelayLogger] No existing relay events file found\n");
        return true; // Not an error, just no events yet
    }

    File file = LittleFS.open(RELAY_EVENTS_FILE, "r");
    if (!file) {
        UnifiedLogger::error("[RelayLogger] Failed to open relay events file for reading\n");
        return false;
    }

    PsramJsonAllocator jsonAllocator;
    JsonDocument doc(&jsonAllocator);
    DeserializationError error = deserializeJson(doc, file);
    file.close();

    if (error) {
        UnifiedLogger::error("[RelayLogger] Failed to parse relay events file: %s\n", error.c_str());
        return false;
    }

    // Clear existing events
    relayEvents.clear();

    // Load metadata
    if (!doc["metadata"]["next_event_id"].isNull()) {
        nextEventId = doc["metadata"]["next_event_id"];
    } else {
        nextEventId = 1; // Fallback
    }

    // Load events
    JsonArray events = doc["events"];
    for (JsonObject eventObj : events) {
        RelayEvent event;
        event.event_id = eventObj["event_id"];
        event.timestamp = eventObj["timestamp"];
        event.datetime = eventObj["datetime"].as<String>();
        event.datetime_start = eventObj["datetime_start"].as<String>();
        event.previous_state = eventObj["previous_state"].as<String>();
        event.new_state = eventObj["new_state"].as<String>();
        event.requested_by = eventObj["requested_by"].as<String>();
        event.result = eventObj["result"].as<String>();
        event.counter = eventObj["counter"];

        if (eventObj["context"].is<String>()) {
            event.context = eventObj["context"].as<String>();
        }

        relayEvents.push_back(event);
    }

    // Ensure we don't exceed limit after loading
    trimEventsToLimit();

    UnifiedLogger::info("[RelayLogger] Loaded %d relay events\n", relayEvents.size());

    return true;
}


bool RelayLogger::saveRelayEvents() {
    unsigned long startTime = micros();

    // Pre-allocate JsonDocument with sufficient size for 100 events (~20KB)
    // Each event: ~200 bytes (metadata + strings), plus overhead
    PsramJsonAllocator jsonAllocator;
    JsonDocument doc(&jsonAllocator);

    // Add metadata
    JsonObject metadata = doc["metadata"].to<JsonObject>();
    metadata["version"] = 1;
    metadata["format"] = "RELAY_EVENTS_V1";
    metadata["next_event_id"] = nextEventId;

    // Add events array
    JsonArray events = doc["events"].to<JsonArray>();

    for (const auto& event : relayEvents) {
        JsonObject eventObj = events.add<JsonObject>();
        eventObj["event_id"] = event.event_id;
        eventObj["timestamp"] = event.timestamp;
        eventObj["datetime"] = event.datetime;
        eventObj["datetime_start"] = event.datetime_start;
        eventObj["previous_state"] = event.previous_state;
        eventObj["new_state"] = event.new_state;
        eventObj["requested_by"] = event.requested_by;
        eventObj["result"] = event.result;
        eventObj["counter"] = event.counter;

        if (!event.context.isEmpty()) {
            eventObj["context"] = event.context;
        }
    }

    // Atomic save process: write to backup, then rename
    File backupFile = LittleFS.open(RELAY_EVENTS_BACKUP_FILE, "w");
    if (!backupFile) {
        UnifiedLogger::error("[RelayLogger] Failed to open backup file for writing\n");
        unsigned long durationMs = (micros() - startTime) / 1000;
        if (mainController) {
            mainController->updateRelayLoggerStats(1, durationMs); // 1 = relay save
        }
        return false;
    }

    if (serializeJson(doc, backupFile) == 0) {
        UnifiedLogger::error("[RelayLogger] Failed to write to backup file\n");
        backupFile.close();
        unsigned long durationMs = (micros() - startTime) / 1000;
        if (mainController) {
            mainController->updateRelayLoggerStats(1, durationMs); // 1 = relay save
        }
        return false;
    }

    backupFile.close();

    // Delete old main file
    if (LittleFS.exists(RELAY_EVENTS_FILE)) {
        LittleFS.remove(RELAY_EVENTS_FILE);
    }

    // Rename backup to main
    if (!LittleFS.rename(RELAY_EVENTS_BACKUP_FILE, RELAY_EVENTS_FILE)) {
        UnifiedLogger::error("[RelayLogger] Failed to rename backup file\n");
        unsigned long durationMs = (micros() - startTime) / 1000;
        if (mainController) {
            mainController->updateRelayLoggerStats(1, durationMs); // 1 = relay save
        }
        return false;
    }

    unsigned long durationMs = (micros() - startTime) / 1000;
    if (mainController) {
        mainController->updateRelayLoggerStats(1, durationMs); // 1 = relay save
    }

    return true;
}

void RelayLogger::flushRelayEventsIfDirty() {
    if (relayEventsDirty) {
        if (saveRelayEvents()) {
            relayEventsDirty = false;
        }
    }
}

void RelayLogger::forceFlushRelayEvents() {
    saveRelayEvents();
}

bool RelayLogger::addRelayEvent(const String& previousState, const String& newState,
                                const String& requestedBy, const String& result,
                                const String& context) {
    // Check if identical event exists
    for (auto& event : relayEvents) {
        if (event.previous_state == previousState &&
            event.new_state == newState &&
            event.requested_by == requestedBy &&
            event.result == result) {
            // Update existing event: increment counter, update last datetime
            event.counter++;
            event.timestamp = millis();
            if (ntpManager && ntpManager->isTimeSet()) {
                event.datetime = ntpManager->getFormattedDateTime();
            } else {
                event.datetime = "NTP not synchronized";
            }
            event.context = context; // Update context with latest occurrence
            relayEventsDirty = true;
            return true;
        }
    }

    // Create new event
    RelayEvent event;
    event.event_id = nextEventId++;
    event.timestamp = millis();
    event.previous_state = previousState;
    event.new_state = newState;
    event.requested_by = requestedBy;
    event.result = result;
    event.counter = 1;
    event.context = context;

    // Set datetime fields
    if (ntpManager && ntpManager->isTimeSet()) {
        event.datetime = ntpManager->getFormattedDateTime();
        event.datetime_start = event.datetime;
    } else {
        event.datetime = "NTP not synchronized";
        event.datetime_start = "NTP not synchronized";
    }

    // Add to events and trim if needed
    relayEvents.push_back(event);
    trimEventsToLimit();
    relayEventsDirty = true;

    return true;
}

void RelayLogger::trimEventsToLimit() {
    while (relayEvents.size() > MAX_RELAY_EVENTS) {
        relayEvents.erase(relayEvents.begin()); // Remove oldest
    }
}
