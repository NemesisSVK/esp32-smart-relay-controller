#include "MQTTManager.h"
#include "ErrorHandler.h"
#include <LittleFS.h>
#include "UnifiedLogger.h"
#include "DS18B20SensorManager.h"
#include "SHT4xSensorManager.h"
#include "MAX31855KSensorManager.h"
#include "ChipTempSensorManager.h"
#include "RelayManager.h"
#include "MainController.h"
#include "PsramAlloc.h"
#include "PsramLimits.h"
#include <cstring>

const MQTTManager::MqttPublishOptions MQTTManager::kPublishPolicyBirth = {1, true};
const MQTTManager::MqttPublishOptions MQTTManager::kPublishPolicyDiscoveryConfig = {1, true};
const MQTTManager::MqttPublishOptions MQTTManager::kPublishPolicyDs18Telemetry = {1, false};
const MQTTManager::MqttPublishOptions MQTTManager::kPublishPolicyTelemetry = {0, false};
const MQTTManager::MqttPublishOptions MQTTManager::kPublishPolicyStatus = {1, false};

MQTTManager::MQTTManager()
    : asyncTransport(),
         discoveryPublished(false),
         publishInterval(10000), // Default 10 seconds
          mainController(nullptr),
          ds18b20Manager(nullptr),
          sht4xManager(nullptr),
          max31855kManager(nullptr),
          chipTempManager(nullptr),
          relayManager(nullptr),
         settings(nullptr),
         lastPublishTime(0),
         lastReconnectAttemptMs(0),
         connectionAttemptInProgress(false),
         connectAttemptStartedMs(0),
          lastConnectionState(false),
          disconnectionTime(0),
          connectionTime(0),
          deferredFirstPublishForDsReadout(false),
          connectedEventPending(false),
          connectTransitions(0),
          disconnectTransitions(0),
          lastDisconnectReason(-1),
          lastDisconnectMs(0),
          publishAttempts(0),
          publishSuccess(0),
          publishFailed(0),
          lastPublishAttemptMs(0),
          ds18PublishFailureOverflowLogMs(0),
          maxPublishFailureLogMs(0),
          sht4xPublishFailureLogMs(0),
          sht4xFaultPublishFailureLogMs(0),
          cpuPublishFailureLogMs(0),
          memoryPublishFailureLogMs(0),
          littleFSMountFailureLogMs(0),
          littleFSPublishFailureLogMs(0),
          relayStatePublishFailureLogMs(0),
          relayCommandPublishFailureLogMs(0),
          bufferedPublishDropLogMs(0),
          bufferedPublishFlushFailureLogMs(0),
          bufferedPublishes(),
          bufferedPublishBytes(0),
          bufferedFlushPending(false),
          bufferedPublishDropCount(0),
          bufferedPublishEvictionCount(0),
          discoveryInProgress(false),
          discoveryPhase(DiscoveryPhase::DS18B20),
          discoveryDs18b20Index(0),
          lastDiscoveryStepMs(0) {
    // Set MQTT buffer size - increased to handle large discovery messages
    asyncTransport.setBufferSize(2048);

    for (int i = 0; i < MAX_DS18_LOG_SLOTS; i++) {
        ds18PublishFailureLogMs[i] = 0;
    }
}

MQTTManager::~MQTTManager() {
    clearBufferedPublishes();
}

void MQTTManager::setupMQTT(const String& server, int port, const String& clientId, const String& user, const String& password) {
    mqtt_server = server;
    mqtt_port = port;
    client_id = clientId;
    mqtt_user = user;
    mqtt_password = password;

    asyncTransport.setServer(mqtt_server, mqtt_port);
    // Use a more forgiving keepalive to reduce brief "offline/online" flaps in Home Assistant.
    asyncTransport.setKeepAlive(60);  // 60s Keep Alive -> ~90s offline detection
    asyncTransport.setSocketTimeout(60);
}

void MQTTManager::setMainController(MainController* controller) {
    mainController = controller;
}

void MQTTManager::setManagers(DS18B20SensorManager* ds18b20, SHT4xSensorManager* sht4x, MAX31855KSensorManager* max31855k, ChipTempSensorManager* chip, RelayManager* relay) {
    ds18b20Manager = ds18b20;
    sht4xManager = sht4x;
    max31855kManager = max31855k;
    chipTempManager = chip;
    relayManager = relay;
}

void MQTTManager::setSettings(const Settings* configSettings) {
    settings = configSettings;
}

void MQTTManager::reconnect() {
    if (asyncTransport.isConnected()) {
        connectionAttemptInProgress = false;
        return;
    }

    const unsigned long now = millis();
    if (connectionAttemptInProgress) {
        if ((now - connectAttemptStartedMs) < CONNECT_ATTEMPT_GUARD_MS) {
            return;
        }
        connectionAttemptInProgress = false;
    }

    if (lastReconnectAttemptMs != 0 && (now - lastReconnectAttemptMs) < RECONNECT_THROTTLE_MS) {
        return;
    }

    UnifiedLogger::info("[MQTT] Attempting MQTT connection...\n");

    // Configure LWT parameters
    String willTopic = "device/" + client_id + "/status";
    const String willMessage = "offline";
    const MqttPublishOptions birthPolicy = kPublishPolicyBirth;
    byte willQoS = birthPolicy.qos;
    boolean willRetain = birthPolicy.retained;

    if (asyncTransport.connect(client_id,
                               mqtt_user,
                               mqtt_password,
                               willTopic,
                               willQoS,
                               willRetain,
                               willMessage)) {
        // Connection can complete asynchronously; transition handling and birth publish
        // are processed in trackConnectionState()/processTransportEvents().
        connectionAttemptInProgress = true;
        connectAttemptStartedMs = now;
    } else {
        int state = asyncTransport.state();

        // Report auth failures explicitly when the backend provides that signal.
        const int malformedCreds = static_cast<int>(AsyncMqttClientDisconnectReason::MQTT_MALFORMED_CREDENTIALS);
        const int notAuthorized = static_cast<int>(AsyncMqttClientDisconnectReason::MQTT_NOT_AUTHORIZED);
        if (state == malformedCreds || state == notAuthorized) {
            ErrorHandler::reportError(ErrorCode::MQTT_INVALID_CREDENTIALS, ErrorSeverity::ERROR,
                                    "MQTT authentication failed - check credentials");
        } else {
            ErrorHandler::reportError(ErrorCode::MQTT_CONNECTION_FAILED, ErrorSeverity::WARNING,
                                    "MQTT connection failed");
       }

       UnifiedLogger::warning("[MQTT] Connect attempt failed immediately (state=%d)\n", state);
       connectionAttemptInProgress = false;
   }

    // Throttle is based on completion time to avoid rapid-fire retries after a blocking connect().
    lastReconnectAttemptMs = millis();
}

bool MQTTManager::isConnected() {
    return asyncTransport.isConnected();
}

unsigned long MQTTManager::getDisconnectionDuration() const {
    if (!lastConnectionState) {
        return millis() - disconnectionTime;
    }
    return 0;
}

unsigned long MQTTManager::getConnectionDuration() const {
    if (lastConnectionState) {
        return millis() - connectionTime;
    }
    return 0;
}

bool MQTTManager::hasBeenDisconnectedFor(unsigned long milliseconds) const {
    if (!lastConnectionState) {
        return (millis() - disconnectionTime) >= milliseconds;
    }
    return false;
}

bool MQTTManager::consumeConnectedEvent() {
    const bool hadEvent = connectedEventPending;
    connectedEventPending = false;
    return hadEvent;
}

MQTTManager::PublishStats MQTTManager::getPublishStatsSnapshot() const {
    return PublishStats{
        publishAttempts,
        publishSuccess,
        publishFailed,
        lastPublishAttemptMs
    };
}

MQTTManager::DiagnosticsSnapshot MQTTManager::getDiagnosticsSnapshot() const {
    const unsigned long now = millis();
    return DiagnosticsSnapshot{
        connectionAttemptInProgress,
        discoveryInProgress,
        discoveryPublished,
        lastReconnectAttemptMs == 0 ? 0 : (now - lastReconnectAttemptMs),
        lastPublishAttemptMs == 0 ? 0 : (now - lastPublishAttemptMs),
        connectTransitions,
        disconnectTransitions,
        lastDisconnectReason,
        lastDisconnectMs == 0 ? 0 : (now - lastDisconnectMs),
        bufferedPublishes.size(),
        bufferedPublishBytes,
        bufferedFlushPending,
        bufferedPublishDropCount,
        bufferedPublishEvictionCount
    };
}

bool MQTTManager::shouldLogPublishFailure(unsigned long& lastLogMs) {
    const unsigned long now = millis();
    if (lastLogMs == 0 || (now - lastLogMs) >= PUBLISH_FAILURE_LOG_INTERVAL_MS) {
        lastLogMs = now;
        return true;
    }
    return false;
}

bool MQTTManager::shouldBufferTopic(const String& topic) const {
    // Discovery payloads are retained and resent on connect; don't cache these.
    return !topic.endsWith("/config");
}

size_t MQTTManager::getBufferedEntryBytes(const BufferedPublishEntry& entry) const {
    return (entry.topicLen + 1U) + (entry.payloadLen + 1U);
}

void MQTTManager::freeBufferedPublishEntry(BufferedPublishEntry& entry) {
    if (entry.topic) {
        psram::deallocate(entry.topic);
        entry.topic = nullptr;
    }
    if (entry.payload) {
        psram::deallocate(entry.payload);
        entry.payload = nullptr;
    }
    entry.topicLen = 0;
    entry.payloadLen = 0;
}

void MQTTManager::clearBufferedPublishes() {
    for (size_t i = 0; i < bufferedPublishes.size(); i++) {
        freeBufferedPublishEntry(bufferedPublishes[i]);
    }
    bufferedPublishes.clear();
    bufferedPublishBytes = 0;
    bufferedFlushPending = false;
}

int MQTTManager::findBufferedPublishIndexByTopic(const String& topic) const {
    const size_t topicLen = static_cast<size_t>(topic.length());
    for (size_t i = 0; i < bufferedPublishes.size(); i++) {
        const BufferedPublishEntry& entry = bufferedPublishes[i];
        if (entry.topic && entry.topicLen == topicLen && strncmp(entry.topic, topic.c_str(), topicLen) == 0) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

bool MQTTManager::evictOldestBufferedPublish() {
    if (bufferedPublishes.empty()) {
        return false;
    }

    size_t oldestIndex = 0;
    unsigned long oldestMs = bufferedPublishes[0].updatedAtMs;
    for (size_t i = 1; i < bufferedPublishes.size(); i++) {
        if (bufferedPublishes[i].updatedAtMs < oldestMs) {
            oldestMs = bufferedPublishes[i].updatedAtMs;
            oldestIndex = i;
        }
    }

    BufferedPublishEntry& oldest = bufferedPublishes[oldestIndex];
    const size_t released = getBufferedEntryBytes(oldest);
    freeBufferedPublishEntry(oldest);
    bufferedPublishes.erase(bufferedPublishes.begin() + oldestIndex);
    bufferedPublishBytes = (bufferedPublishBytes >= released) ? (bufferedPublishBytes - released) : 0;
    bufferedPublishEvictionCount++;
    return true;
}

bool MQTTManager::bufferPublishLatest(const String& topic, const String& payload, const MqttPublishOptions& options) {
    if (!shouldBufferTopic(topic)) {
        return false;
    }

    const size_t topicLen = static_cast<size_t>(topic.length());
    const size_t payloadLen = static_cast<size_t>(payload.length());
    const size_t entryBytes = (topicLen + 1U) + (payloadLen + 1U);
    const size_t maxEntryBytes = psram_limits::kMqttOfflineCacheMaxEntryBytes;
    const size_t maxPoolBytes = psram_limits::kMqttOfflineCacheBytes;
    const size_t maxEntries = psram_limits::kMqttOfflineCacheMaxEntries;

    if (entryBytes > maxEntryBytes || topicLen == 0U) {
        if (shouldLogPublishFailure(bufferedPublishDropLogMs)) {
            UnifiedLogger::warning("[MQTT] Deferred publish drop: topic=%s bytes=%u exceeds entry cap=%u\n",
                                   topic.c_str(),
                                   static_cast<unsigned int>(entryBytes),
                                   static_cast<unsigned int>(maxEntryBytes));
        }
        bufferedPublishDropCount++;
        return false;
    }

    const int existingIndex = findBufferedPublishIndexByTopic(topic);
    if (existingIndex >= 0) {
        BufferedPublishEntry& existing = bufferedPublishes[static_cast<size_t>(existingIndex)];
        const size_t oldBytes = getBufferedEntryBytes(existing);

        psram::AllocationResult payloadAlloc = psram::allocate(payloadLen + 1U);
        if (!payloadAlloc.ok()) {
            if (shouldLogPublishFailure(bufferedPublishDropLogMs)) {
                UnifiedLogger::warning("[MQTT] Deferred publish update drop: payload alloc failed for topic=%s\n",
                                       topic.c_str());
            }
            bufferedPublishDropCount++;
            return false;
        }
        memcpy(payloadAlloc.ptr, payload.c_str(), payloadLen);
        static_cast<char*>(payloadAlloc.ptr)[payloadLen] = '\0';

        psram::deallocate(existing.payload);
        existing.payload = static_cast<char*>(payloadAlloc.ptr);
        existing.payloadLen = payloadLen;
        existing.options = options;
        existing.updatedAtMs = millis();

        const size_t newBytes = getBufferedEntryBytes(existing);
        if (newBytes >= oldBytes) {
            bufferedPublishBytes += (newBytes - oldBytes);
        } else {
            bufferedPublishBytes -= (oldBytes - newBytes);
        }
        bufferedFlushPending = true;
        return true;
    }

    while ((bufferedPublishBytes + entryBytes > maxPoolBytes) || (bufferedPublishes.size() >= maxEntries)) {
        if (!evictOldestBufferedPublish()) {
            break;
        }
    }

    if ((bufferedPublishBytes + entryBytes > maxPoolBytes) || (bufferedPublishes.size() >= maxEntries)) {
        if (shouldLogPublishFailure(bufferedPublishDropLogMs)) {
            UnifiedLogger::warning("[MQTT] Deferred publish drop: cache full (entries=%u bytes=%u)\n",
                                   static_cast<unsigned int>(bufferedPublishes.size()),
                                   static_cast<unsigned int>(bufferedPublishBytes));
        }
        bufferedPublishDropCount++;
        return false;
    }

    psram::AllocationResult topicAlloc = psram::allocate(topicLen + 1U);
    psram::AllocationResult payloadAlloc = psram::allocate(payloadLen + 1U);
    if (!topicAlloc.ok() || !payloadAlloc.ok()) {
        if (topicAlloc.ok()) {
            psram::deallocate(topicAlloc.ptr);
        }
        if (payloadAlloc.ok()) {
            psram::deallocate(payloadAlloc.ptr);
        }
        if (shouldLogPublishFailure(bufferedPublishDropLogMs)) {
            UnifiedLogger::warning("[MQTT] Deferred publish drop: alloc failed (topic=%s)\n", topic.c_str());
        }
        bufferedPublishDropCount++;
        return false;
    }

    memcpy(topicAlloc.ptr, topic.c_str(), topicLen);
    static_cast<char*>(topicAlloc.ptr)[topicLen] = '\0';
    memcpy(payloadAlloc.ptr, payload.c_str(), payloadLen);
    static_cast<char*>(payloadAlloc.ptr)[payloadLen] = '\0';

    BufferedPublishEntry entry{};
    entry.topic = static_cast<char*>(topicAlloc.ptr);
    entry.topicLen = topicLen;
    entry.payload = static_cast<char*>(payloadAlloc.ptr);
    entry.payloadLen = payloadLen;
    entry.options = options;
    entry.updatedAtMs = millis();
    bufferedPublishes.push_back(entry);
    bufferedPublishBytes += entryBytes;
    bufferedFlushPending = true;
    return true;
}

bool MQTTManager::flushBufferedPublishes(size_t maxPerCall) {
    if (!isConnected() || bufferedPublishes.empty()) {
        bufferedFlushPending = !bufferedPublishes.empty();
        return true;
    }

    size_t sentCount = 0;
    while (!bufferedPublishes.empty() && sentCount < maxPerCall) {
        BufferedPublishEntry& entry = bufferedPublishes.front();
        publishAttempts++;
        lastPublishAttemptMs = millis();
        const bool ok = asyncTransport.publish(String(entry.topic), String(entry.payload), entry.options.retained, entry.options.qos);
        if (!ok) {
            publishFailed++;
            bufferedFlushPending = true;
            if (shouldLogPublishFailure(bufferedPublishFlushFailureLogMs)) {
                UnifiedLogger::warning("[MQTT] Deferred publish flush paused (topic=%s)\n", entry.topic);
            }
            ErrorHandler::reportError(ErrorCode::MQTT_PUBLISH_FAILED, ErrorSeverity::WARNING,
                                      "MQTT deferred publish flush failed for topic: " + String(entry.topic));
            return false;
        }

        publishSuccess++;
        const size_t released = getBufferedEntryBytes(entry);
        freeBufferedPublishEntry(entry);
        bufferedPublishes.erase(bufferedPublishes.begin());
        bufferedPublishBytes = (bufferedPublishBytes >= released) ? (bufferedPublishBytes - released) : 0;
        sentCount++;
    }

    bufferedFlushPending = !bufferedPublishes.empty();
    return true;
}

void MQTTManager::trackConnectionState(bool currentConnectionState) {
    if (currentConnectionState != lastConnectionState) {
        if (currentConnectionState) {
            connectionTime = millis();
            connectTransitions++;
            connectionAttemptInProgress = false;
            connectedEventPending = true;
            bufferedFlushPending = !bufferedPublishes.empty();

            String birthTopic = "device/" + client_id + "/status";
            if (!publish(birthTopic, "online", kPublishPolicyBirth)) {
                UnifiedLogger::warning("[MQTT] Failed to publish online birth status\n");
            }

            // Re-publish current state snapshot immediately on reconnect so HA
            // gets fresh values without waiting for the periodic publish interval.
            if (settings) {
                publishAllSensorData();
            }
        } else {
            disconnectionTime = millis();
            lastDisconnectMs = disconnectionTime;
            disconnectTransitions++;
            lastDisconnectReason = asyncTransport.state();
            connectionAttemptInProgress = false;
        }
        lastConnectionState = currentConnectionState;

        UnifiedLogger::info("[MQTT] Connection state changed: %s (state=%d)\n",
                            currentConnectionState ? "CONNECTED" : "DISCONNECTED",
                            asyncTransport.state());
    }
}

void MQTTManager::updateConnectionState(bool networkAvailable) {
    const bool currentConnectionState = networkAvailable && asyncTransport.isConnected();
    trackConnectionState(currentConnectionState);
}

void MQTTManager::loop() {
    asyncTransport.loop();
    processTransportEvents();

    // Track connection state changes (non-blocking)
    updateConnectionState(true);

    // Flush deferred latest-state publishes right after reconnect so HA receives fresh values quickly.
    if (isConnected() && bufferedFlushPending) {
        flushBufferedPublishes(OFFLINE_FLUSH_BATCH_PER_LOOP);
    }

    // Process Home Assistant discovery in small non-blocking steps.
    // This avoids multi-second loop stalls when many DS18B20 sensors are configured.
    if (isConnected() && discoveryInProgress) {
        processDiscoveryQueue();
        return;
    }

    // Check for timed publishing.
    // Run this regardless of MQTT connection so publish() can stage latest
    // values into the deferred cache while disconnected.
    if (settings && millis() - lastPublishTime >= publishInterval) {
        // Ensure DS18B20 has produced at least one readout before we start publishing batches.
        // This prevents publishing uninitialized cache values (or the DS18B20 85C power-on default)
        // immediately after MQTT connects.
        if (ds18b20Manager && !ds18b20Manager->hasCompletedAtLeastOneReadout()) {
            if (!deferredFirstPublishForDsReadout) {
                UnifiedLogger::info("[MQTT] Deferring publish until first DS18B20 readout completes\n");
                deferredFirstPublishForDsReadout = true;
            }

            // Avoid tight-loop retries: schedule a quick re-check soon.
            const unsigned long now = millis();
            const unsigned long retryDelayMs = 1000UL;
            if (publishInterval > retryDelayMs) {
                lastPublishTime = now - publishInterval + retryDelayMs;
            } else {
                lastPublishTime = now;
            }
            return;
        }

        deferredFirstPublishForDsReadout = false;
        publishAllSensorData();
        lastPublishTime = millis();
    }
}

bool MQTTManager::publish(const String& topic, const String& payload, const MqttPublishOptions& options) {
    if (!isConnected()) {
        if (bufferPublishLatest(topic, payload, options)) {
            return true;
        }
        publishFailed++;
        ErrorHandler::reportError(ErrorCode::MQTT_PUBLISH_FAILED, ErrorSeverity::WARNING,
                                  "MQTT publish failed while disconnected for topic: " + topic);
        return false;
    }

    publishAttempts++;
    lastPublishAttemptMs = millis();
    bool success = asyncTransport.publish(topic, payload, options.retained, options.qos);

    if (success) {
        publishSuccess++;
        return true;
    }

    publishFailed++;
    if (bufferPublishLatest(topic, payload, options)) {
        return true;
    }

    ErrorHandler::reportError(ErrorCode::MQTT_PUBLISH_FAILED, ErrorSeverity::WARNING,
                              "MQTT publish failed for topic: " + topic);
    return false;
}

void MQTTManager::processTransportEvents() {
    AsyncMqttTransport::ConnectionEvents events;
    if (!asyncTransport.pollConnectionEvents(events)) {
        return;
    }

    // Transport callbacks can run outside the main loop on async backends.
    // Handle diagnostics here to keep logging/error handling in loop context only.
    if (events.connected) {
        UnifiedLogger::info("[MQTT] Transport event: connected\n");
    }
    if (events.disconnected) {
        connectionAttemptInProgress = false;
        lastDisconnectReason = events.disconnectState;
        lastDisconnectMs = millis();
        const int malformedCreds = static_cast<int>(AsyncMqttClientDisconnectReason::MQTT_MALFORMED_CREDENTIALS);
        const int notAuthorized = static_cast<int>(AsyncMqttClientDisconnectReason::MQTT_NOT_AUTHORIZED);
        if (events.disconnectState == malformedCreds || events.disconnectState == notAuthorized) {
            ErrorHandler::reportError(ErrorCode::MQTT_INVALID_CREDENTIALS, ErrorSeverity::ERROR,
                                      "MQTT authentication failed - check credentials");
        } else {
            ErrorHandler::reportError(ErrorCode::MQTT_CONNECTION_FAILED, ErrorSeverity::WARNING,
                                      "MQTT connection failed");
        }
        UnifiedLogger::warning("[MQTT] Transport event: disconnected (state=%d)\n",
                               events.disconnectState);
    }
}

bool MQTTManager::publish(const String& topic, const String& payload, bool retained) {
    return publish(topic, payload, MqttPublishOptions{0, retained});
}

void MQTTManager::publishDiscovery(const Settings& settings, DS18B20SensorManager* ds18b20Manager, SHT4xSensorManager* sht4xManager, ChipTempSensorManager* chipTempManager, RelayManager* relayManager) {
    if (discoveryPublished || discoveryInProgress) {
        return;
    }

    // Keep references current in case this is called before/after manager rewire.
    this->ds18b20Manager = ds18b20Manager;
    this->sht4xManager = sht4xManager;
    this->chipTempManager = chipTempManager;
    this->relayManager = relayManager;
    this->settings = &settings;

    discoveryInProgress = true;
    discoveryPhase = DiscoveryPhase::DS18B20;
    discoveryDs18b20Index = 0;
    lastDiscoveryStepMs = 0;

    UnifiedLogger::info("[MQTT] Starting non-blocking discovery publish\n");
}

void MQTTManager::processDiscoveryQueue() {
    if (!discoveryInProgress || !settings) {
        return;
    }

    switch (discoveryPhase) {
        case DiscoveryPhase::DS18B20: {
            if (ds18b20Manager && discoveryDs18b20Index < ds18b20Manager->getSensorCount()) {
                const unsigned long now = millis();
                if (lastDiscoveryStepMs != 0 && (now - lastDiscoveryStepMs) < DISCOVERY_STEP_DELAY_MS) {
                    return;
                }

                publishDS18B20Discovery(*settings, discoveryDs18b20Index);
                discoveryDs18b20Index++;
                lastDiscoveryStepMs = now;
                return;
            }
            discoveryPhase = DiscoveryPhase::MEMORY;
            return;
        }
        case DiscoveryPhase::MEMORY:
            publishMemoryDiscovery(*settings);
            discoveryPhase = DiscoveryPhase::LITTLEFS;
            return;
        case DiscoveryPhase::LITTLEFS:
            publishLittleFSDiscovery(*settings);
            discoveryPhase = DiscoveryPhase::SHT4X;
            return;
        case DiscoveryPhase::SHT4X:
            if (sht4xManager && sht4xManager->isInitialized()) {
                publishSHT4xDiscovery(*settings);
            }
            discoveryPhase = DiscoveryPhase::MAX31855K;
            return;
        case DiscoveryPhase::MAX31855K:
            if (max31855kManager) {
                publishMAX31855KDiscovery(*settings);
            }
            discoveryPhase = DiscoveryPhase::CHIPTEMP;
            return;
        case DiscoveryPhase::CHIPTEMP:
            if (chipTempManager && chipTempManager->isInitialized()) {
                publishChipTempDiscovery(*settings);
            }
            discoveryPhase = DiscoveryPhase::RELAY;
            return;
        case DiscoveryPhase::RELAY:
            if (relayManager) {
                publishRelayDiscovery(*settings);
            }
            discoveryPhase = DiscoveryPhase::COMPLETE;
            return;
        case DiscoveryPhase::COMPLETE:
            discoveryInProgress = false;
            discoveryPublished = true;
            UnifiedLogger::info("[MQTT] Discovery messages complete\n");
            return;
    }
}

void MQTTManager::publishAllSensorData() {
    // Time the entire batch operation
    unsigned long batchStartTime = millis();

    // Publish DS18B20 sensor data
    if (ds18b20Manager && settings) {
        publishDS18B20Data(*settings, ds18b20Manager);
    }

    // Publish SHT4x sensor data
    if (sht4xManager && settings) {
        publishSHT4xData(*settings, sht4xManager);
    }

    // Publish MAX31855K sensor data
    if (max31855kManager && settings) {
        publishMAX31855KData(*settings, max31855kManager);
    }

    // Publish chip temperature data
    if (chipTempManager && chipTempManager->isInitialized() && settings) {
        publishChipTempData(*settings, chipTempManager);
    }

    // Publish memory data
    if (settings) {
        publishMemoryData(*settings);
    }

    // Publish LittleFS data
    if (settings) {
        publishLittleFSData(*settings);
    }

    // Publish relay data
    if (relayManager && settings) {
        publishRelayData(*settings, relayManager);
    }

    // Record batch timing to performance stats
    unsigned long batchDurationMs = millis() - batchStartTime;
    if (mainController) {
        mainController->updateComponentStatsPublic(3, batchDurationMs); // MQTT_INDEX = 3
    }

}

void MQTTManager::publishMAX31855KDiscovery(const Settings& settings) {
    String availabilityTopic = "device/" + settings.mqtt.client_id + "/status";
    String baseTopic = "homeassistant/sensor/" + settings.mqtt.client_id + "_" + settings.mqtt.topics.max31855k;
    String deviceInfo = "\"identifiers\":[\"" + settings.mqtt.client_id + "\"],\"name\":\"" + settings.device_info.name + "\",\"manufacturer\":\"" + settings.device_info.manufacturer + "\",\"model\":\"" + settings.device_info.model + "\"";

    // Thermocouple temperature sensor discovery
    String tempConfigTopic = baseTopic + "_temp/config";
    String tempStateTopic = baseTopic + "_temp/state";
    String tempUniqueId = settings.mqtt.client_id + "_" + settings.mqtt.topics.max31855k + "_temp";

    String tempDiscovery = "{\"name\":\"" + settings.mqtt.topics.max31855k + " Temperature\",";
    tempDiscovery += "\"state_topic\":\"" + tempStateTopic + "\",";
    tempDiscovery += "\"availability_topic\":\"" + availabilityTopic + "\",";
    tempDiscovery += "\"value_template\":\"{{ value_json.value }}\",";
    tempDiscovery += "\"unique_id\":\"" + tempUniqueId + "\",";
    tempDiscovery += "\"expire_after\":300,";
    tempDiscovery += "\"unit_of_measurement\":\"\\u00B0C\",";
    tempDiscovery += "\"device_class\":\"temperature\",";
    tempDiscovery += "\"state_class\":\"measurement\",";
    tempDiscovery += "\"force_update\":true,";
    tempDiscovery += "\"device\":{" + deviceInfo + "}}";

    publish(tempConfigTopic, tempDiscovery, kPublishPolicyDiscoveryConfig);

    // Status sensor discovery
    String statusConfigTopic = baseTopic + "_status/config";
    String statusStateTopic = baseTopic + "_status/state";
    String statusUniqueId = settings.mqtt.client_id + "_" + settings.mqtt.topics.max31855k + "_status";

    String statusDiscovery = "{\"name\":\"" + settings.mqtt.topics.max31855k + " Status\",";
    statusDiscovery += "\"state_topic\":\"" + statusStateTopic + "\",";
    statusDiscovery += "\"availability_topic\":\"" + availabilityTopic + "\",";
    statusDiscovery += "\"value_template\":\"{{ value_json.value }}\",";
    statusDiscovery += "\"unique_id\":\"" + statusUniqueId + "\",";
    statusDiscovery += "\"expire_after\":300,";
    statusDiscovery += "\"force_update\":true,";
    statusDiscovery += "\"entity_category\":\"diagnostic\",";
    statusDiscovery += "\"icon\":\"mdi:message-text-outline\",";
    statusDiscovery += "\"device\":{" + deviceInfo + "}}";

    publish(statusConfigTopic, statusDiscovery, kPublishPolicyDiscoveryConfig);
}

void MQTTManager::publishMAX31855KData(const Settings& settings, MAX31855KSensorManager* max31855kManager) {
    if (!max31855kManager) return;

    String baseTopic = "homeassistant/sensor/" + settings.mqtt.client_id + "_" + settings.mqtt.topics.max31855k;

    // Publish temperature (thermocouple only)
    float tc = max31855kManager->getThermocoupleC();
    String tempPayload;
    if (isnan(tc)) {
        tempPayload = "{\"value\":null}";
    } else {
        tempPayload = "{\"value\":" + String(tc, 1) + "}";
    }
    bool tempSuccess = publish(baseTopic + "_temp/state", tempPayload, kPublishPolicyTelemetry);

    // Publish normalized operational status for parity with DS18/SHT4x.
    // Keep detailed MAX31855K-specific state in an extra field for diagnostics.
    const bool operational = max31855kManager->isSensorOperational();
    const char* detailedStatus = max31855kManager->getStatusString();
    String statusPayload = "{\"value\":\"" + String(operational ? "OK" : "NOT OPERATIONAL") +
                           "\",\"detail\":\"" + String(detailedStatus) + "\"}";
    bool statusSuccess = publish(baseTopic + "_status/state", statusPayload, kPublishPolicyStatus);

    if (!(tempSuccess && statusSuccess)) {
        if (shouldLogPublishFailure(maxPublishFailureLogMs)) {
            UnifiedLogger::warning("[MQTT] Failed to publish MAX31855K data\n");
        }
    }
}

void MQTTManager::publishDS18B20Discovery(const Settings& settings, int sensorIndex) {
    String sensorName = settings.mqtt.topics.temp[sensorIndex];
    String availabilityTopic = "device/" + settings.mqtt.client_id + "/status";

    // Temperature sensor discovery
    String tempStateTopic = "homeassistant/sensor/" + settings.mqtt.client_id + "_" + settings.mqtt.topics.temp[sensorIndex] + "/state";
    String tempUniqueId = settings.mqtt.client_id + "_" + settings.mqtt.topics.temp[sensorIndex];
    String tempConfigTopic = "homeassistant/sensor/" + settings.mqtt.client_id + "_" + settings.mqtt.topics.temp[sensorIndex] + "/config";

    String tempDiscovery = "{\"name\":\"" + sensorName + "\","
                          "\"state_topic\":\"" + tempStateTopic + "\","
                           "\"availability_topic\":\"" + availabilityTopic + "\","
                           "\"value_template\":\"{{ value_json.value }}\","
                           "\"unique_id\":\"" + tempUniqueId + "\","
                           "\"expire_after\":300,"
                           "\"unit_of_measurement\":\"\\u00B0C\","
                          "\"device_class\":\"temperature\","
                          "\"state_class\":\"measurement\","
                          "\"force_update\":true,"
                          "\"icon\":\"mdi:thermometer\","
                          "\"device\":{\"identifiers\":[\"" + settings.mqtt.client_id + "\"],\"name\":\"" + settings.device_info.name + "\",\"manufacturer\":\"" + settings.device_info.manufacturer + "\",\"model\":\"" + settings.device_info.model + "\"}}";

    publish(tempConfigTopic, tempDiscovery, kPublishPolicyDiscoveryConfig);

    // Status sensor discovery
    String statusSensorName = sensorName + " Status";
    String statusStateTopic = "homeassistant/sensor/" + settings.mqtt.client_id + "_" + settings.mqtt.topics.temp[sensorIndex] + "_status/state";
    String statusUniqueId = settings.mqtt.client_id + "_" + settings.mqtt.topics.temp[sensorIndex] + "_status";
    String statusConfigTopic = "homeassistant/sensor/" + settings.mqtt.client_id + "_" + settings.mqtt.topics.temp[sensorIndex] + "_status/config";

    String statusDiscovery = "{\"name\":\"" + statusSensorName + "\","
                              "\"state_topic\":\"" + statusStateTopic + "\","
                               "\"availability_topic\":\"" + availabilityTopic + "\","
                               "\"value_template\":\"{{ value_json.value }}\","
                               "\"unique_id\":\"" + statusUniqueId + "\","
                               "\"expire_after\":300,"
                               "\"force_update\":true,"
                              "\"entity_category\":\"diagnostic\","
                              "\"icon\":\"mdi:message-text-outline\","
                              "\"device\":{\"identifiers\":[\"" + settings.mqtt.client_id + "\"],\"name\":\"" + settings.device_info.name + "\",\"manufacturer\":\"" + settings.device_info.manufacturer + "\",\"model\":\"" + settings.device_info.model + "\"}}";

    publish(statusConfigTopic, statusDiscovery, kPublishPolicyDiscoveryConfig);
}

void MQTTManager::publishSHT4xDiscovery(const Settings& settings) {
    String baseTopic = "homeassistant/sensor/" + settings.mqtt.client_id + "_" + settings.mqtt.topics.sht4x;
    String availabilityTopic = "device/" + settings.mqtt.client_id + "/status";
    String deviceInfo = "\"identifiers\":[\"" + settings.mqtt.client_id + "\"],\"name\":\"" + settings.device_info.name + "\",\"manufacturer\":\"" + settings.device_info.manufacturer + "\",\"model\":\"" + settings.device_info.model + "\"";
    String sensorName = settings.mqtt.topics.sht4x;

    // Temperature sensor discovery
    String tempConfigTopic = baseTopic + "_temp/config";
    String tempStateTopic = baseTopic + "_temp/state";
    String tempUniqueId = settings.mqtt.client_id + "_" + settings.mqtt.topics.sht4x + "_temp";

    String tempDiscovery = "{\"name\":\"" + sensorName + " Temperature\","
                          "\"state_topic\":\"" + tempStateTopic + "\","
                           "\"availability_topic\":\"" + availabilityTopic + "\","
                           "\"value_template\":\"{{ value_json.temperature }}\","
                           "\"unique_id\":\"" + tempUniqueId + "\","
                           "\"expire_after\":300,"
                           "\"unit_of_measurement\":\"\\u00B0C\","
                          "\"device_class\":\"temperature\","
                          "\"state_class\":\"measurement\","
                          "\"force_update\":true,"
                          "\"device\":{" + deviceInfo + "}}";

    publish(tempConfigTopic, tempDiscovery, kPublishPolicyDiscoveryConfig);

    // Humidity sensor discovery
    String humidityConfigTopic = baseTopic + "_humidity/config";
    String humidityStateTopic = baseTopic + "_humidity/state";
    String humidityUniqueId = settings.mqtt.client_id + "_" + settings.mqtt.topics.sht4x + "_humidity";

    String humidityDiscovery = "{\"name\":\"" + sensorName + " Humidity\","
                              "\"state_topic\":\"" + humidityStateTopic + "\","
                               "\"availability_topic\":\"" + availabilityTopic + "\","
                               "\"value_template\":\"{{ value_json.humidity }}\","
                               "\"unique_id\":\"" + humidityUniqueId + "\","
                               "\"expire_after\":300,"
                               "\"unit_of_measurement\":\"%\","
                              "\"device_class\":\"humidity\","
                              "\"state_class\":\"measurement\","
                              "\"force_update\":true,"
                              "\"device\":{" + deviceInfo + "}}";

    publish(humidityConfigTopic, humidityDiscovery, kPublishPolicyDiscoveryConfig);

    // Status sensor discovery
    String statusConfigTopic = baseTopic + "_status/config";
    String statusStateTopic = baseTopic + "_status/state";
    String statusUniqueId = settings.mqtt.client_id + "_" + settings.mqtt.topics.sht4x + "_status";

    String statusDiscovery = "{\"name\":\"" + sensorName + " Status\","
                            "\"state_topic\":\"" + statusStateTopic + "\","
                            "\"availability_topic\":\"" + availabilityTopic + "\","
                            "\"value_template\":\"{{ value_json.status }}\","
                            "\"unique_id\":\"" + statusUniqueId + "\","
                            "\"expire_after\":300,"
                            "\"force_update\":true,"
                            "\"entity_category\":\"diagnostic\","
                            "\"device\":{" + deviceInfo + "}}";

    publish(statusConfigTopic, statusDiscovery, kPublishPolicyDiscoveryConfig);

}

void MQTTManager::publishMemoryDiscovery(const Settings& settings) {
    String availabilityTopic = "device/" + settings.mqtt.client_id + "/status";

    // Memory sensor discovery
    String memorySensorName = "Memory Free Heap";
    String memoryStateTopic = "homeassistant/sensor/" + settings.mqtt.client_id + "_memory_free_heap/state";
    String memoryUniqueId = settings.mqtt.client_id + "_memory_free_heap";
    String memoryConfigTopic = "homeassistant/sensor/" + settings.mqtt.client_id + "_memory_free_heap/config";

    String memoryDiscovery = "{\"name\":\"" + memorySensorName + "\","
                            "\"state_topic\":\"" + memoryStateTopic + "\","
                            "\"availability_topic\":\"" + availabilityTopic + "\","
                            "\"value_template\":\"{{ value_json.value }}\","
                            "\"unique_id\":\"" + memoryUniqueId + "\","
                            "\"force_update\":true,"
                            "\"unit_of_measurement\":\"B\","
                            "\"entity_category\":\"diagnostic\","
                            "\"device\":{\"identifiers\":[\"" + settings.mqtt.client_id + "\"],\"name\":\"" + settings.device_info.name + "\",\"manufacturer\":\"" + settings.device_info.manufacturer + "\",\"model\":\"" + settings.device_info.model + "\"}}";

    publish(memoryConfigTopic, memoryDiscovery, kPublishPolicyDiscoveryConfig);
}

void MQTTManager::publishLittleFSDiscovery(const Settings& settings) {
    String availabilityTopic = "device/" + settings.mqtt.client_id + "/status";

    // LittleFS sensor discovery
    String littleFSSensorName = "LittleFS Free Space";
    String littleFSStateTopic = "homeassistant/sensor/" + settings.mqtt.client_id + "_littlefs_free_percent/state";
    String littleFSUniqueId = settings.mqtt.client_id + "_littlefs_free_percent";
    String littleFSConfigTopic = "homeassistant/sensor/" + settings.mqtt.client_id + "_littlefs_free_percent/config";

    String littleFSDiscovery = "{\"name\":\"" + littleFSSensorName + "\","
                              "\"state_topic\":\"" + littleFSStateTopic + "\","
                              "\"availability_topic\":\"" + availabilityTopic + "\","
                              "\"value_template\":\"{{ value_json.value }}\","
                              "\"unique_id\":\"" + littleFSUniqueId + "\","
                              "\"force_update\":true,"
                              "\"unit_of_measurement\":\"%\","
                              "\"entity_category\":\"diagnostic\","
                              "\"device\":{\"identifiers\":[\"" + settings.mqtt.client_id + "\"],\"name\":\"" + settings.device_info.name + "\",\"manufacturer\":\"" + settings.device_info.manufacturer + "\",\"model\":\"" + settings.device_info.model + "\"}}";

    publish(littleFSConfigTopic, littleFSDiscovery, kPublishPolicyDiscoveryConfig);
}

void MQTTManager::publishDS18B20Data(const Settings& settings, DS18B20SensorManager* ds18b20Manager) {
    const bool hasReadout = ds18b20Manager->hasCompletedAtLeastOneReadout();
    for (int i = 0; i < ds18b20Manager->getSensorCount(); i++) {
        float temp = ds18b20Manager->getTemperature(i);
        const bool suppress85 = hasReadout && ds18b20Manager->isLikelyPowerOnDefault85C(i);
        const bool publishAsNa = (!hasReadout) || temp == -127.0f || suppress85;
        const bool operational = ds18b20Manager->isSensorOperational(i);

        // Publish to MQTT - Temperature
        String tempPayload;
        if (publishAsNa) {
            // Publish null for invalid readings so HA can move state away from stale numeric value.
            tempPayload = "{\"value\":null}";
        } else {
            tempPayload = "{\"value\":" + String(temp, 1) + "}";
        }
        String tempStateTopic = "homeassistant/sensor/" + settings.mqtt.client_id + "_" + settings.mqtt.topics.temp[i] + "/state";
        bool tempSuccess = publish(tempStateTopic, tempPayload, kPublishPolicyDs18Telemetry);

        // Publish to MQTT - Status
        String status;
        if (!hasReadout) {
            status = "WARMING_UP";
        } else if (suppress85 && operational) {
            status = "WARMING_UP";
        } else if (publishAsNa || !operational) {
            // Reflect disconnected/invalid sensor state explicitly as NOT OPERATIONAL.
            status = "NOT OPERATIONAL";
        } else {
            status = operational ? "OK" : "NOT OPERATIONAL";
        }

        String statusPayload = "{\"value\":\"" + status + "\"}";
        String statusStateTopic = "homeassistant/sensor/" + settings.mqtt.client_id + "_" + settings.mqtt.topics.temp[i] + "_status/state";
        bool statusSuccess = publish(statusStateTopic, statusPayload, kPublishPolicyStatus);

        bool success = tempSuccess && statusSuccess;
        if (!success) {
            if (i < MAX_DS18_LOG_SLOTS) {
                if (shouldLogPublishFailure(ds18PublishFailureLogMs[i])) {
                    UnifiedLogger::warning("[MQTT] Failed to publish DS18B20 sensor %d data\n", i + 1);
                }
            } else if (shouldLogPublishFailure(ds18PublishFailureOverflowLogMs)) {
                UnifiedLogger::warning("[MQTT] Failed to publish DS18B20 sensor %d data\n", i + 1);
            }
        }
    }
}

void MQTTManager::publishSHT4xData(const Settings& settings, SHT4xSensorManager* sht4xManager) {
    // Check if we have a new valid measurement to publish
    bool hasNewMeasurement = sht4xManager->hasNewMeasurement();
    const bool operational = sht4xManager->isSensorOperational();

    if (hasNewMeasurement) {
        float temp = sht4xManager->getTemperature();
        float humidity = sht4xManager->getHumidity();
        String status = operational ? "OK" : "NOT OPERATIONAL";

        String baseTopic = "homeassistant/sensor/" + settings.mqtt.client_id + "_" + settings.mqtt.topics.sht4x;

        // Publish temperature
        String tempPayload;
        if (isnan(temp)) {
            tempPayload = "{\"temperature\":\"NA\"}";
        } else {
            tempPayload = "{\"temperature\":" + String(temp, 1) + "}";
        }
        String tempStateTopic = baseTopic + "_temp/state";
        bool tempSuccess = publish(tempStateTopic, tempPayload, kPublishPolicyTelemetry);

        // Publish humidity
        String humidityPayload;
        if (isnan(humidity)) {
            humidityPayload = "{\"humidity\":\"NA\"}";
        } else {
            humidityPayload = "{\"humidity\":" + String(humidity, 1) + "}";
        }
        String humidityStateTopic = baseTopic + "_humidity/state";
        bool humiditySuccess = publish(humidityStateTopic, humidityPayload, kPublishPolicyTelemetry);

        // Publish status
        String statusPayload = "{\"status\":\"" + status + "\"}";
        String statusStateTopic = baseTopic + "_status/state";
        bool statusSuccess = publish(statusStateTopic, statusPayload, kPublishPolicyStatus);

        bool sht4xSuccess = tempSuccess && humiditySuccess && statusSuccess;
        if (!sht4xSuccess) {
            if (shouldLogPublishFailure(sht4xPublishFailureLogMs)) {
                UnifiedLogger::warning("[MQTT] Failed to publish SHT4x sensor data\n");
            }
        }

        // Clear the new measurement flag after successful publication
        sht4xManager->clearNewMeasurementFlag();

    } else if (!operational) {
        // When sensor reads fail, publish explicit NA + NOT OPERATIONAL so HA does
        // not continue displaying stale "OK" values.
        String baseTopic = "homeassistant/sensor/" + settings.mqtt.client_id + "_" + settings.mqtt.topics.sht4x;

        String tempPayload = "{\"temperature\":null}";
        String humidityPayload = "{\"humidity\":null}";
        String statusPayload = "{\"status\":\"NOT OPERATIONAL\"}";

        String tempStateTopic = baseTopic + "_temp/state";
        String humidityStateTopic = baseTopic + "_humidity/state";
        String statusStateTopic = baseTopic + "_status/state";

        bool tempSuccess = publish(tempStateTopic, tempPayload, kPublishPolicyTelemetry);
        bool humiditySuccess = publish(humidityStateTopic, humidityPayload, kPublishPolicyTelemetry);
        bool statusSuccess = publish(statusStateTopic, statusPayload, kPublishPolicyStatus);

        if (!(tempSuccess && humiditySuccess && statusSuccess)) {
            if (shouldLogPublishFailure(sht4xFaultPublishFailureLogMs)) {
                UnifiedLogger::warning("[MQTT] Failed to publish SHT4x fault state\n");
            }
        }
    }
}

void MQTTManager::publishChipTempDiscovery(const Settings& settings) {
    String availabilityTopic = "device/" + settings.mqtt.client_id + "/status";

    // CPU temperature sensor discovery (similar to Memory Free Heap)
    String sensorName = "CPU Temp";
    String stateTopic = "homeassistant/sensor/" + settings.mqtt.client_id + "_cpu_temp/state";
    String uniqueId = settings.mqtt.client_id + "_cpu_temp";
    String configTopic = "homeassistant/sensor/" + settings.mqtt.client_id + "_cpu_temp/config";

    String discovery = "{\"name\":\"" + sensorName + "\","
                      "\"state_topic\":\"" + stateTopic + "\","
                      "\"availability_topic\":\"" + availabilityTopic + "\","
                      "\"value_template\":\"{{ value_json.value }}\","
                      "\"unique_id\":\"" + uniqueId + "\","
                      "\"force_update\":true,"
                      "\"unit_of_measurement\":\"\\u00B0C\","
                      "\"entity_category\":\"diagnostic\","
                      "\"device\":{\"identifiers\":[\"" + settings.mqtt.client_id + "\"],\"name\":\"" + settings.device_info.name + "\",\"manufacturer\":\"" + settings.device_info.manufacturer + "\",\"model\":\"" + settings.device_info.model + "\"}}";

    publish(configTopic, discovery, kPublishPolicyDiscoveryConfig);
}

void MQTTManager::publishChipTempData(const Settings& settings, ChipTempSensorManager* chipTempManager) {
    float temp = chipTempManager->getTemperature();

    // Publish to MQTT
    String payload;
    if (isnan(temp)) {
        payload = "{\"value\":null}";
    } else {
        payload = "{\"value\":" + String(temp, 1) + "}";
    }
    String stateTopic = "homeassistant/sensor/" + settings.mqtt.client_id + "_cpu_temp/state";
    bool success = publish(stateTopic, payload, kPublishPolicyTelemetry);

    if (!success) {
        if (shouldLogPublishFailure(cpuPublishFailureLogMs)) {
            UnifiedLogger::warning("[MQTT] Failed to publish CPU temperature\n");
        }
    }
}

void MQTTManager::publishMemoryData(const Settings& settings) {
    String memoryPayload = "{\"value\":" + String(ESP.getFreeHeap()) + "}";
    String memoryStateTopic = "homeassistant/sensor/" + settings.mqtt.client_id + "_memory_free_heap/state";
    bool memorySuccess = publish(memoryStateTopic, memoryPayload, kPublishPolicyTelemetry);

    if (!memorySuccess) {
        if (shouldLogPublishFailure(memoryPublishFailureLogMs)) {
            UnifiedLogger::warning("[MQTT] Failed to publish memory info\n");
        }
    }
}

void MQTTManager::publishLittleFSData(const Settings& settings) {
    // Publish LittleFS free space percentage
    if (!LittleFS.begin()) {
        if (shouldLogPublishFailure(littleFSMountFailureLogMs)) {
            UnifiedLogger::warning("[MQTT] LittleFS mount failed for MQTT publishing\n");
        }
        return;
    }

    size_t totalBytes = LittleFS.totalBytes();
    size_t usedBytes = LittleFS.usedBytes();
    size_t freeBytes = totalBytes - usedBytes;

    float freePercent = 0.0;
    if (totalBytes > 0) {
        freePercent = (float)freeBytes / (float)totalBytes * 100.0;
    }

    String littleFSPayload = "{\"value\":" + String(freePercent, 1) + "}";
    String littleFSStateTopic = "homeassistant/sensor/" + settings.mqtt.client_id + "_littlefs_free_percent/state";
    bool littleFSSuccess = publish(littleFSStateTopic, littleFSPayload, kPublishPolicyTelemetry);

    if (!littleFSSuccess) {
        if (shouldLogPublishFailure(littleFSPublishFailureLogMs)) {
            UnifiedLogger::warning("[MQTT] Failed to publish LittleFS info\n");
        }
    }
}

void MQTTManager::publishRelayDiscovery(const Settings& settings) {
    String availabilityTopic = "device/" + settings.mqtt.client_id + "/status";
    String deviceInfo = "\"identifiers\":[\"" + settings.mqtt.client_id + "\"],\"name\":\"" + settings.device_info.name + "\",\"manufacturer\":\"" + settings.device_info.manufacturer + "\",\"model\":\"" + settings.device_info.model + "\"";

    // Relay state sensor discovery
    String relayStateSensorName = "Relay State";
    String relayStateStateTopic = "homeassistant/sensor/" + settings.mqtt.client_id + "_relay_state/state";
    String relayStateUniqueId = settings.mqtt.client_id + "_relay_state";
    String relayStateConfigTopic = "homeassistant/sensor/" + settings.mqtt.client_id + "_relay_state/config";

    String relayStateDiscovery = "{\"name\":\"" + relayStateSensorName + "\","
                              "\"state_topic\":\"" + relayStateStateTopic + "\","
                              "\"availability_topic\":\"" + availabilityTopic + "\","
                              "\"value_template\":\"{{ value_json.state }}\","
                              "\"unique_id\":\"" + relayStateUniqueId + "\","
                              "\"force_update\":true,"
                              "\"entity_category\":\"diagnostic\","
                              "\"device\":{" + deviceInfo + "}}";

    publish(relayStateConfigTopic, relayStateDiscovery, kPublishPolicyDiscoveryConfig);

    // Relay command topic for Home Assistant
    String relayCommandSensorName = "Relay Command";
    String relayCommandStateTopic = "homeassistant/sensor/" + settings.mqtt.client_id + "_relay_command/state";
    String relayCommandUniqueId = settings.mqtt.client_id + "_relay_command";
    String relayCommandConfigTopic = "homeassistant/sensor/" + settings.mqtt.client_id + "_relay_command/config";

    String relayCommandDiscovery = "{\"name\":\"" + relayCommandSensorName + "\","
                                "\"state_topic\":\"" + relayCommandStateTopic + "\","
                                "\"availability_topic\":\"" + availabilityTopic + "\","
                                "\"value_template\":\"{{ value_json.command }}\","
                                "\"unique_id\":\"" + relayCommandUniqueId + "\","
                                "\"force_update\":true,"
                                "\"entity_category\":\"diagnostic\","
                                "\"device\":{" + deviceInfo + "}}";

    publish(relayCommandConfigTopic, relayCommandDiscovery, kPublishPolicyDiscoveryConfig);
}

void MQTTManager::publishRelayData(const Settings& settings, RelayManager* relayManager) {
    // Publish relay state
    String relayState = relayManager->getModeString();
    String relayStatePayload = "{\"state\":\"" + relayState + "\"}";
    String relayStateTopic = "homeassistant/sensor/" + settings.mqtt.client_id + "_relay_state/state";
    bool relayStateSuccess = publish(relayStateTopic, relayStatePayload, kPublishPolicyStatus);

    if (!relayStateSuccess) {
        if (shouldLogPublishFailure(relayStatePublishFailureLogMs)) {
            UnifiedLogger::warning("[MQTT] Failed to publish relay state\n");
        }
    }

    // Publish relay command source
    String commandSource = relayManager->getLastCommandSource();
    // If safety shutdown is active, override with safety status
    if (relayManager->isSafetyShutdownActive()) {
        commandSource = "safety_active";
    }
    String relayCommandPayload = "{\"command\":\"" + commandSource + "\"}";
    String relayCommandTopic = "homeassistant/sensor/" + settings.mqtt.client_id + "_relay_command/state";
    bool relayCommandSuccess = publish(relayCommandTopic, relayCommandPayload, kPublishPolicyStatus);

    if (!relayCommandSuccess) {
        if (shouldLogPublishFailure(relayCommandPublishFailureLogMs)) {
            UnifiedLogger::warning("[MQTT] Failed to publish relay command\n");
        }
    }
}
