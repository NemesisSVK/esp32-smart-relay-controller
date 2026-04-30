#ifndef MQTTFALLBACKMANAGER_H
#define MQTTFALLBACKMANAGER_H

#include "ConfigManager.h"
#include "MQTTManager.h"
#include "ScheduleManager.h"
#include "RelayManager.h"

class MQTTFallbackManager {
public:
    MQTTFallbackManager();
    ~MQTTFallbackManager();

    // Initialize with dependencies
    void begin(ConfigManager& config, MQTTManager& mqtt, ScheduleManager& schedule, RelayManager* relay);

    // Main update method called in loop
    void update();

private:
    ConfigManager* configManager;
    MQTTManager* mqttManager;
    ScheduleManager* scheduleManager;
    RelayManager* relayManager;

    // State tracking
    bool schedulerWasEnabled;  // Remember original scheduler state
    bool fallbackActive;       // Whether fallback is currently active

    // Relay mode restoration after fallback ends
    bool preFallbackRelayModeCaptured;
    RelayMode preFallbackRelayMode;
};

#endif // MQTTFALLBACKMANAGER_H
