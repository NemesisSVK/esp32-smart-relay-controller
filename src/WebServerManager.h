#ifndef WEBSERVERMANAGER_H
#define WEBSERVERMANAGER_H

#include <LittleFS.h>
#include <ArduinoJson.h>
#include <functional>
#include <memory>
#if defined(ARDUINO_ARCH_ESP32)
#include <freertos/portmacro.h>
#endif
#include "ConfigManager.h"
#include "NTPManager.h"
#include "IPWhitelistManager.h"
#include "PerformanceStats.h"
namespace webserver {
class IRequest;
class IResponse;
enum class RequestMethod : uint8_t;
class IWebServerBackend;
}


// Forward declarations to avoid circular dependencies
class DS18B20SensorManager;
class SHT4xSensorManager;
class MAX31855KSensorManager;
class ChipTempSensorManager;
class MQTTManager;
class RelayManager;
class ScheduleManager;
class MainController;

// Basic authentication support

class WebServerManager {
public:
    WebServerManager();
    ~WebServerManager();

    // Web server management
    void setupWebServer();
    void handleClient();
    void registerEndpoints();


    // Set references to managers (called by MainController)
    void setManagers(ConfigManager* configMgr, DS18B20SensorManager* ds18b20Mgr, SHT4xSensorManager* sht4xMgr, MAX31855KSensorManager* max31855kMgr, ChipTempSensorManager* chipTempMgr, MQTTManager* mqttMgr, NTPManager* ntpMgr, RelayManager* relayMgr, ScheduleManager* scheduleMgr, MainController* mainCtrl);

    // Boot time setup
    void setBootDateTime(const String& bootTime);

    // Authentication support functions (public for HTTPS handlers)
    bool isAuthEnabled() const { return authEnabled; }
    ConfigManager* getConfigManager() const { return configManager; }

private:
    std::unique_ptr<webserver::IWebServerBackend> backend;
    ConfigManager* configManager;
    DS18B20SensorManager* ds18b20Manager;
    SHT4xSensorManager* sht4xManager;
    MAX31855KSensorManager* max31855kManager;
    ChipTempSensorManager* chipTempManager;
    MQTTManager* mqttManager;
    NTPManager* ntpManager;
    ScheduleManager* scheduleManager;
    IPWhitelistManager ipWhitelistManager;
    RelayManager* relayManager;
    MainController* mainController;

    // Boot time tracking
    String bootDateTime;

    // Authentication support
    bool authEnabled;
    bool restartPending;
    unsigned long restartAtMs;
    static constexpr unsigned long RESTART_GRACE_PERIOD_MS = 250;

    // DS18B20 scan guardrails
    unsigned long lastScanStartedMs;
    static constexpr unsigned long SCAN_COOLDOWN_MS = 5000;
    static constexpr unsigned long FULL_SCAN_COOLDOWN_MS = 30000;
    struct RelayStatusApiDiagnosticsSnapshot {
        uint32_t requestsTotal = 0;
        uint32_t status2xx = 0;
        uint32_t status4xx = 0;
        uint32_t status5xx = 0;
        uint32_t statusOther = 0;
        uint32_t avgLatencyMs = 0;
        uint32_t p95LatencyMs = 0;
        uint32_t maxLatencyMs = 0;
        int32_t lastNon200Status = 0;
        unsigned long lastNon200AgeMs = 0;
        unsigned long lastRequestAgeMs = 0;
    };
    struct RelayStatusApiDiagnosticsData {
        uint32_t requestsTotal = 0;
        uint32_t status2xx = 0;
        uint32_t status4xx = 0;
        uint32_t status5xx = 0;
        uint32_t statusOther = 0;
        uint64_t totalLatencyMs = 0;
        uint32_t maxLatencyMs = 0;
        uint16_t latencySamples[128] = {0};
        uint16_t latencySampleCount = 0;
        uint16_t latencySampleWriteIndex = 0;
        int32_t lastNon200Status = 0;
        unsigned long lastNon200Ms = 0;
        unsigned long lastRequestMs = 0;
    };
    RelayStatusApiDiagnosticsData relayStatusApiDiagnostics;
#if defined(ARDUINO_ARCH_ESP32)
    mutable portMUX_TYPE relayStatusApiDiagnosticsLock = portMUX_INITIALIZER_UNLOCKED;
#endif

    // HTML page handlers
    void handleRoot(webserver::IRequest& request, webserver::IResponse& response);
    void handleSettings(webserver::IRequest& request, webserver::IResponse& response);
    void handleOta(webserver::IRequest& request, webserver::IResponse& response);
    void handleDetection(webserver::IRequest& request, webserver::IResponse& response);
    void handleMonitoring(webserver::IRequest& request, webserver::IResponse& response);
    void handlePerformance(webserver::IRequest& request, webserver::IResponse& response);
    void handleRelay(webserver::IRequest& request, webserver::IResponse& response);
    void handleDebug(webserver::IRequest& request, webserver::IResponse& response);
    void handleFaviconIco(webserver::IRequest& request, webserver::IResponse& response);
    void handleFaviconSvg(webserver::IRequest& request, webserver::IResponse& response);

    // API handlers
    void handleApiSettingsGet(webserver::IRequest& request, webserver::IResponse& response);
    void handleApiSettingsPost(webserver::IRequest& request, webserver::IResponse& response);
    void handleApiReboot(webserver::IRequest& request, webserver::IResponse& response);
    void handleApiConfigGet(webserver::IRequest& request, webserver::IResponse& response);
    void handleApiHomeSensors(webserver::IRequest& request, webserver::IResponse& response);
    void handleApiOtaStatus(webserver::IRequest& request, webserver::IResponse& response);
    void handleApiOtaArm(webserver::IRequest& request, webserver::IResponse& response);
    void handleApiErrorsGet(webserver::IRequest& request, webserver::IResponse& response);
    void handleApiErrorHistoryGet(webserver::IRequest& request, webserver::IResponse& response);
    void handleApiMonitoringCards(webserver::IRequest& request, webserver::IResponse& response);
    void handleApiPerformanceCards(webserver::IRequest& request, webserver::IResponse& response);
    void handleApiPerformanceReset();
    void handleScan(webserver::IRequest& request, webserver::IResponse& response);
    void handleApiNtpStatus(webserver::IRequest& request, webserver::IResponse& response);
    void handleApiNtpLogs(webserver::IRequest& request, webserver::IResponse& response);
    void handleApiRelayStatus(webserver::IRequest& request, webserver::IResponse& response);
    void handleApiRelaySet(webserver::IRequest& request, webserver::IResponse& response);
    void handleApiRelayLogs(webserver::IRequest& request, webserver::IResponse& response);
    void handleRelayControl(String mode, webserver::IRequest& request, webserver::IResponse& response);

    // Debug log viewer (UI-only)
    void handleApiDebugEnable(webserver::IRequest& request, webserver::IResponse& response);
    void handleApiDebugLogs(webserver::IRequest& request, webserver::IResponse& response);
    void handleApiDebugClear(webserver::IRequest& request, webserver::IResponse& response);

    // Schedule API handlers

    // Utility functions
    String getNavHtml(String current);
    void streamSettingsHtml(webserver::IResponse& response);
    void streamDetectionHtml(webserver::IResponse& response);
    void streamMonitoringHtml(webserver::IResponse& response);
    void streamPerformanceHtml(webserver::IResponse& response);
    String generateHomeSensorsHtml(const Settings& settings);
    String generateMonitoringCardsHtml(const Settings& settings);
    String generatePerformanceStatsHtml();
    String generateRelayLoggerPerformanceHtml();
    String generateRelayLoggerRow(const char* operation, const PerformanceStats& stats);
    String generateRootHtml();
    void streamRelayHtml(webserver::IResponse& response);
    String getLittleFSInfo();

    // Authentication support functions
    bool checkAuthentication(webserver::IRequest& request, webserver::IResponse& response);
    String generateAuthRealm();

    // IP whitelist support functions
    bool checkIPWhitelist(webserver::IRequest& request, webserver::IResponse& response);

    // WiFi connectivity check
    bool isWiFiConnected();

    // Deferred restart support (avoids blocking delay in HTTP handlers)
    void scheduleRestart(unsigned long delayMs = RESTART_GRACE_PERIOD_MS);
    void processPendingRestart();

    // Adapter route shim used by all registered handlers.
    using AdapterHandler = std::function<void(webserver::IRequest&, webserver::IResponse&)>;
    void registerTimedEndpointAdapter(const char* uri, webserver::RequestMethod method, AdapterHandler handler);
    void recordRelayStatusApiRequest(int statusCode, uint32_t durationMs);
    RelayStatusApiDiagnosticsSnapshot getRelayStatusApiDiagnosticsSnapshot() const;
    void resetRelayStatusApiDiagnostics();
    static uint32_t computeP95FromSamples(const uint16_t* samples, uint16_t count);
    void lockRelayStatusApiDiagnostics() const;
    void unlockRelayStatusApiDiagnostics() const;
};

#endif // WEBSERVERMANAGER_H
