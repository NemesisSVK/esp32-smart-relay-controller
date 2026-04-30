#ifndef MQTTMANAGER_H
#define MQTTMANAGER_H

#include "ConfigManager.h"
#include "AsyncMqttTransport.h"
#include <vector>

// Forward declarations to avoid circular dependencies
class DS18B20SensorManager;
class SHT4xSensorManager;
class MAX31855KSensorManager;
class ChipTempSensorManager;
class RelayManager;
class MainController;

class MQTTManager {
public:
    struct PublishStats {
        uint32_t attempts;
        uint32_t success;
        uint32_t failed;
        unsigned long lastAttemptMs;
    };

    struct DiagnosticsSnapshot {
        bool reconnectAttemptInProgress;
        bool discoveryInProgress;
        bool discoveryPublished;
        unsigned long reconnectAttemptAgeMs;
        unsigned long lastPublishAttemptAgeMs;
        uint32_t connectTransitions;
        uint32_t disconnectTransitions;
        int lastDisconnectReason;
        unsigned long lastDisconnectAgeMs;
        size_t bufferedEntryCount;
        size_t bufferedBytes;
        bool bufferedFlushPending;
        uint32_t bufferedDropCount;
        uint32_t bufferedEvictionCount;
    };

    struct MqttPublishOptions {
        uint8_t qos;
        bool retained;
    };

    // Publish policy constants (active with async transport):
    // - Birth/LWT: QoS1 retained
    // - Discovery config: QoS1 retained
    // - DS18 telemetry values: QoS1 non-retained (mission-critical)
    // - Telemetry values: QoS0 non-retained
    // - Status/diagnostic states: QoS1 non-retained
    static const MqttPublishOptions kPublishPolicyBirth;
    static const MqttPublishOptions kPublishPolicyDiscoveryConfig;
    static const MqttPublishOptions kPublishPolicyDs18Telemetry;
    static const MqttPublishOptions kPublishPolicyTelemetry;
    static const MqttPublishOptions kPublishPolicyStatus;

    MQTTManager();
    ~MQTTManager();

    // MQTT setup and connection
    void setupMQTT(const String& server, int port, const String& clientId, const String& user = "", const String& password = "");
    void reconnect();
    bool isConnected();
    void loop();

    // Update connection timing state without doing network work.
    // Call this even when WiFi is down so fallback logic can rely on disconnect timing.
    void updateConnectionState(bool networkAvailable);

    // Publishing methods
    bool publish(const String& topic, const String& payload, const MqttPublishOptions& options);
    bool publish(const String& topic, const String& payload, bool retained = false);
    void publishDiscovery(const Settings& settings, DS18B20SensorManager* ds18b20Manager, SHT4xSensorManager* sht4xManager, ChipTempSensorManager* chipTempManager, RelayManager* relayManager);
    void publishAllSensorData();

    // Discovery state
    void setDiscoveryPublished(bool published) { discoveryPublished = published; }
    bool hasDiscoveryBeenPublished() const { return discoveryPublished; }
    
    // Timing configuration
    void setPublishInterval(unsigned long interval) { publishInterval = interval; }
    unsigned long getPublishInterval() const { return publishInterval; }

    // Performance monitoring integration
    void setMainController(MainController* controller);

    // Manager and settings setters
    void setManagers(DS18B20SensorManager* ds18b20, SHT4xSensorManager* sht4x, MAX31855KSensorManager* max31855k, ChipTempSensorManager* chip, RelayManager* relay);
    void setSettings(const Settings* configSettings);

    // Connection status tracking
    unsigned long getDisconnectionDuration() const;
    unsigned long getConnectionDuration() const;
    bool hasBeenDisconnectedFor(unsigned long milliseconds) const;
    bool consumeConnectedEvent();
    PublishStats getPublishStatsSnapshot() const;
    DiagnosticsSnapshot getDiagnosticsSnapshot() const;

private:
    static constexpr int MAX_DS18_LOG_SLOTS = 32;
    static const unsigned long PUBLISH_FAILURE_LOG_INTERVAL_MS = 60000UL; // 60s

    AsyncMqttTransport asyncTransport;
    String mqtt_server;
    int mqtt_port;
    String client_id;
    String mqtt_user;
    String mqtt_password;

    // Discovery state tracking
    bool discoveryPublished;

    // Timing configuration
    unsigned long publishInterval; // Default 10 seconds

    // Performance monitoring
    MainController* mainController;

    // Reconnect throttling (prevents tight-loop hammering when broker/auth is down)
    unsigned long lastReconnectAttemptMs;
    static const unsigned long RECONNECT_THROTTLE_MS = 10000UL; // 10 seconds
    bool connectionAttemptInProgress;
    unsigned long connectAttemptStartedMs;
    static const unsigned long CONNECT_ATTEMPT_GUARD_MS = 10000UL; // 10 seconds

    // Manager references
    DS18B20SensorManager* ds18b20Manager;
    SHT4xSensorManager* sht4xManager;
    MAX31855KSensorManager* max31855kManager;
    ChipTempSensorManager* chipTempManager;
    RelayManager* relayManager;
    const Settings* settings;
    unsigned long lastPublishTime;

    // Connection status tracking
    bool lastConnectionState;
    unsigned long disconnectionTime;
    unsigned long connectionTime;
    bool deferredFirstPublishForDsReadout;
    bool connectedEventPending;
    uint32_t connectTransitions;
    uint32_t disconnectTransitions;
    int lastDisconnectReason;
    unsigned long lastDisconnectMs;
    uint32_t publishAttempts;
    uint32_t publishSuccess;
    uint32_t publishFailed;
    unsigned long lastPublishAttemptMs;

    unsigned long ds18PublishFailureLogMs[MAX_DS18_LOG_SLOTS];
    unsigned long ds18PublishFailureOverflowLogMs;
    unsigned long maxPublishFailureLogMs;
    unsigned long sht4xPublishFailureLogMs;
    unsigned long sht4xFaultPublishFailureLogMs;
    unsigned long cpuPublishFailureLogMs;
    unsigned long memoryPublishFailureLogMs;
    unsigned long littleFSMountFailureLogMs;
    unsigned long littleFSPublishFailureLogMs;
    unsigned long relayStatePublishFailureLogMs;
    unsigned long relayCommandPublishFailureLogMs;
    unsigned long bufferedPublishDropLogMs;
    unsigned long bufferedPublishFlushFailureLogMs;

    struct BufferedPublishEntry {
        char* topic;
        size_t topicLen;
        char* payload;
        size_t payloadLen;
        MqttPublishOptions options;
        unsigned long updatedAtMs;
    };

    std::vector<BufferedPublishEntry> bufferedPublishes;
    size_t bufferedPublishBytes;
    bool bufferedFlushPending;
    uint32_t bufferedPublishDropCount;
    uint32_t bufferedPublishEvictionCount;

    void trackConnectionState(bool currentConnectionState);
    void processTransportEvents();
    bool shouldLogPublishFailure(unsigned long& lastLogMs);
    bool shouldBufferTopic(const String& topic) const;
    bool bufferPublishLatest(const String& topic, const String& payload, const MqttPublishOptions& options);
    bool flushBufferedPublishes(size_t maxPerCall);
    void clearBufferedPublishes();
    void freeBufferedPublishEntry(BufferedPublishEntry& entry);
    size_t getBufferedEntryBytes(const BufferedPublishEntry& entry) const;
    int findBufferedPublishIndexByTopic(const String& topic) const;
    bool evictOldestBufferedPublish();

    // Discovery publishing methods
    void publishDS18B20Discovery(const Settings& settings, int sensorIndex);
    void publishSHT4xDiscovery(const Settings& settings);
    void publishMAX31855KDiscovery(const Settings& settings);
    void publishChipTempDiscovery(const Settings& settings);
    void publishMemoryDiscovery(const Settings& settings);
    void publishLittleFSDiscovery(const Settings& settings);
    void publishRelayDiscovery(const Settings& settings);

    // Sensor data publishing methods
    void publishDS18B20Data(const Settings& settings, DS18B20SensorManager* ds18b20Manager);
    void publishSHT4xData(const Settings& settings, SHT4xSensorManager* sht4xManager);
    void publishMAX31855KData(const Settings& settings, MAX31855KSensorManager* max31855kManager);
    void publishChipTempData(const Settings& settings, ChipTempSensorManager* chipTempManager);
    void publishMemoryData(const Settings& settings);
    void publishLittleFSData(const Settings& settings);
    void publishRelayData(const Settings& settings, RelayManager* relayManager);

    enum class DiscoveryPhase : uint8_t {
        DS18B20 = 0,
        MEMORY,
        LITTLEFS,
        SHT4X,
        MAX31855K,
        CHIPTEMP,
        RELAY,
        COMPLETE
    };

    void processDiscoveryQueue();
    bool discoveryInProgress;
    DiscoveryPhase discoveryPhase;
    int discoveryDs18b20Index;
    unsigned long lastDiscoveryStepMs;
    static const unsigned long DISCOVERY_STEP_DELAY_MS = 100UL;
    static const size_t OFFLINE_FLUSH_BATCH_PER_LOOP = 8U;
};

#endif // MQTTMANAGER_H
