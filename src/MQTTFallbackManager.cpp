#include "MQTTFallbackManager.h"
#include "UnifiedLogger.h"

MQTTFallbackManager::MQTTFallbackManager() :
    configManager(nullptr),
    mqttManager(nullptr),
    scheduleManager(nullptr),
    relayManager(nullptr),
    schedulerWasEnabled(false),
    fallbackActive(false),
    preFallbackRelayModeCaptured(false),
    preFallbackRelayMode(RELAY_OFF) {
}

MQTTFallbackManager::~MQTTFallbackManager() {
    // Cleanup if needed
}

void MQTTFallbackManager::begin(ConfigManager& config, MQTTManager& mqtt, ScheduleManager& schedule, RelayManager* relay) {
    configManager = &config;
    mqttManager = &mqtt;
    scheduleManager = &schedule;
    relayManager = relay;

    // Initialize state
    schedulerWasEnabled = false;
    fallbackActive = false;
    preFallbackRelayModeCaptured = false;
    preFallbackRelayMode = RELAY_OFF;

    UnifiedLogger::info("[MQTTFallback] MQTTFallbackManager initialized\n");
}

void MQTTFallbackManager::update() {
    if (!configManager || !mqttManager || !scheduleManager) {
        UnifiedLogger::error("[MQTTFallback] Managers not initialized\n");
        return;
    }

    Settings& settings = configManager->getSettings();

    // Check if fallback is enabled
    if (!settings.fallback.enabled) {
        if (fallbackActive) {
            // Fallback was active but now disabled - deactivate
            UnifiedLogger::info("[MQTTFallback] Fallback disabled - deactivating scheduler\n");
            settings.schedules.enabled = schedulerWasEnabled;
            fallbackActive = false;

            // Restore relay state only if scheduler was the last command source, and safety shutdown is not active.
            if (preFallbackRelayModeCaptured && relayManager && relayManager->isInitialized()) {
                if (!relayManager->isSafetyShutdownActive() && relayManager->getLastCommandSource() == "schedule") {
                    relayManager->setModeWithVerification(preFallbackRelayMode, "fallback_restore");
                    UnifiedLogger::info("[MQTTFallback] Relay state restored after fallback deactivation\n");
                } else if (relayManager->isSafetyShutdownActive()) {
                    UnifiedLogger::warning("[MQTTFallback] Relay restore skipped: safety shutdown active\n");
                } else {
                    UnifiedLogger::info("[MQTTFallback] Relay restore skipped: last command source was '%s'\n",
                                        relayManager->getLastCommandSource().c_str());
                }
                preFallbackRelayModeCaptured = false;
            }

            // Ensure relay safety settings match the new scheduler state immediately.
            scheduleManager->updateSafetySettings();
            UnifiedLogger::info("[MQTTFallback] Scheduler restored to: %s\n", schedulerWasEnabled ? "enabled" : "disabled");
        }
        return;
    }

    // Calculate timeout in milliseconds
    unsigned long timeoutMs = settings.fallback.timeout_minutes * 60UL * 1000UL;

    // If fallback is currently active, keep scheduler enabled until MQTT has been
    // connected continuously for the same timeout window (stability check).
    if (fallbackActive) {
        if (mqttManager->getConnectionDuration() >= timeoutMs) {
            UnifiedLogger::info("[MQTTFallback] MQTT stable for %lu minutes - deactivating scheduler fallback\n", settings.fallback.timeout_minutes);
            settings.schedules.enabled = schedulerWasEnabled;
            fallbackActive = false;

            // Restore relay state only if scheduler was the last command source, and safety shutdown is not active.
            // Perform this BEFORE re-enabling relay timeout via updateSafetySettings().
            if (preFallbackRelayModeCaptured && relayManager && relayManager->isInitialized()) {
                if (!relayManager->isSafetyShutdownActive() && relayManager->getLastCommandSource() == "schedule") {
                    relayManager->setModeWithVerification(preFallbackRelayMode, "fallback_restore");
                    UnifiedLogger::info("[MQTTFallback] Relay state restored after MQTT stability period\n");
                } else if (relayManager->isSafetyShutdownActive()) {
                    UnifiedLogger::warning("[MQTTFallback] Relay restore skipped after fallback: safety shutdown active\n");
                } else {
                    UnifiedLogger::info("[MQTTFallback] Relay restore skipped after fallback: last command source was '%s'\n",
                                        relayManager->getLastCommandSource().c_str());
                }
                preFallbackRelayModeCaptured = false;
            }

            scheduleManager->updateSafetySettings();
            UnifiedLogger::info("[MQTTFallback] Scheduler restored to: %s\n", schedulerWasEnabled ? "enabled" : "disabled");
        }
        return;
    }

    // If scheduler is already enabled (by configuration), fallback activation is irrelevant.
    if (settings.schedules.enabled) {
        return;
    }

    // Activate fallback when MQTT has been disconnected for the timeout period.
    if (mqttManager->hasBeenDisconnectedFor(timeoutMs)) {
        UnifiedLogger::warning("[MQTTFallback] MQTT disconnected for %lu minutes - activating scheduler\n", settings.fallback.timeout_minutes);

        // Capture relay state so we can restore it when fallback ends.
        if (relayManager && relayManager->isInitialized()) {
            const String preMode = relayManager->getModeString();
            preFallbackRelayMode = relayManager->getCurrentMode();
            preFallbackRelayModeCaptured = true;
            UnifiedLogger::info("[MQTTFallback] Captured pre-fallback relay mode: %s\n", preMode.c_str());
        } else {
            preFallbackRelayModeCaptured = false;
            UnifiedLogger::warning("[MQTTFallback] Relay mode capture skipped: relay manager unavailable\n");
        }

        schedulerWasEnabled = settings.schedules.enabled; // Remember original state (expected false here)
        settings.schedules.enabled = true;
        fallbackActive = true;

        scheduleManager->updateSafetySettings();
        UnifiedLogger::warning("[MQTTFallback] Scheduler activated for fallback mode\n");
    }
}
