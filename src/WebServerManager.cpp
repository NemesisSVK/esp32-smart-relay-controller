#include "WebServerManager.h"
#include "ErrorHandler.h"
#include "RelayManager.h"
#include "RelayLogger.h"
#include "MainController.h"
#include <WiFi.h>
#include <vector>
#include <algorithm>
#include "DS18B20SensorManager.h"
#include "SHT4xSensorManager.h"
#include "MAX31855KSensorManager.h"
#include "ChipTempSensorManager.h"
#include "MQTTManager.h"
#include "ScheduleManager.h"
#include "UnifiedLogger.h"
#include <cstring>
#include "CodeDate.h"
#include "DebugLogBuffer.h"
#include "WebRequestAdapter.h"
#include "PsramJsonAllocator.h"

static constexpr const char* kContentTypeJsonUtf8 = "application/json; charset=UTF-8";
static constexpr const char* kContentTypeHtmlUtf8 = "text/html; charset=UTF-8";
static constexpr const char* kContentTypePlainUtf8 = "text/plain; charset=UTF-8";

namespace {
constexpr unsigned long kRelayControlDispatchTimeoutMs = 3000UL;
constexpr unsigned long kOtaControlDispatchTimeoutMs = 3000UL;
constexpr unsigned long kControlDispatchPollIntervalMs = 5UL;

constexpr int32_t kRelayErrorWifiDisconnected = 5300;
constexpr int32_t kRelayErrorQueueBusy = 5301;
constexpr int32_t kRelayErrorWaitTimeout = 5302;
constexpr int32_t kRelayErrorQueueTimeout = 5303;
constexpr int32_t kRelayErrorSafetyTimeout = 5500;
constexpr int32_t kRelayErrorSafetyShutdown = 5501;

String buildJsonErrorPayload(const char* message, int32_t errorCode = 0) {
    String payload = "{\"error\":\"";
    payload += message ? message : "Unknown error";
    payload += "\"";
    if (errorCode != 0) {
        payload += ",\"code\":";
        payload += String(errorCode);
    }
    payload += "}";
    return payload;
}

void sendJsonError(webserver::IResponse& response, int statusCode, const char* message, int32_t errorCode = 0) {
    response.send(statusCode, kContentTypeJsonUtf8, buildJsonErrorPayload(message, errorCode));
}

String htmlEscape(const String& input) {
    String out;
    out.reserve(input.length());
    for (size_t i = 0; i < static_cast<size_t>(input.length()); i++) {
        const char c = input.charAt(i);
        switch (c) {
            case '&': out += F("&amp;"); break;
            case '<': out += F("&lt;"); break;
            case '>': out += F("&gt;"); break;
            case '"': out += F("&quot;"); break;
            case '\'': out += F("&#39;"); break;
            default: out += c; break;
        }
    }
    return out;
}

void copyCommandTextArg(MainController::ControlCommand& command, const char* value) {
    if (!value) {
        command.textArg[0] = '\0';
        return;
    }
    strncpy(command.textArg, value, sizeof(command.textArg) - 1);
    command.textArg[sizeof(command.textArg) - 1] = '\0';
}
} // namespace

WebServerManager::WebServerManager() : backend(nullptr), configManager(nullptr), ds18b20Manager(nullptr), sht4xManager(nullptr), max31855kManager(nullptr), chipTempManager(nullptr), mqttManager(nullptr), ntpManager(nullptr),
                                               scheduleManager(nullptr), relayManager(nullptr), mainController(nullptr), authEnabled(false), restartPending(false), restartAtMs(0), lastScanStartedMs(0) {
}

WebServerManager::~WebServerManager() {
}

void WebServerManager::setManagers(ConfigManager* configMgr, DS18B20SensorManager* ds18b20Mgr, SHT4xSensorManager* sht4xMgr, MAX31855KSensorManager* max31855kMgr, ChipTempSensorManager* chipTempMgr, MQTTManager* mqttMgr, NTPManager* ntpMgr, RelayManager* relayMgr, ScheduleManager* scheduleMgr, MainController* mainCtrl) {
    configManager = configMgr;
    ds18b20Manager = ds18b20Mgr;
    sht4xManager = sht4xMgr;
    max31855kManager = max31855kMgr;
    chipTempManager = chipTempMgr;
    mqttManager = mqttMgr;
    ntpManager = ntpMgr;
    relayManager = relayMgr;
    scheduleManager = scheduleMgr;
    mainController = mainCtrl;
}

void WebServerManager::setBootDateTime(const String& bootTime) {
    bootDateTime = bootTime;
}

void WebServerManager::setupWebServer() {
    if (!backend) {
        backend = webserver::createWebServerBackend(webserver::WebServerBackendKind::Async, 80);
    }

    if (!backend) {
        UnifiedLogger::error("[WEB] Failed to initialize async HTTP backend\n");
        return;
    }

    registerEndpoints();

    // Check if authentication is enabled in configuration
    if (configManager) {
        const Settings settings = configManager->getSettingsSnapshot();
        authEnabled = settings.security.auth_enabled;
        
        // Initialize IP whitelist
        ipWhitelistManager.setEnabled(settings.security.ip_whitelist_enabled);
        for (const auto& range : settings.security.ip_ranges) {
            ipWhitelistManager.addIPRange(range);
        }
    }

    // Start HTTP server
    backend->begin();
    UnifiedLogger::info("[WEB] HTTP server (%s backend) started on port 80\n",
                         webserver::webServerBackendKindName(backend->kind()));
    if (authEnabled) {
        UnifiedLogger::info("[WEB] HTTP Basic Authentication enabled\n");
    }

}

void WebServerManager::handleClient() {
    if (backend) {
        backend->poll();
    }
    processPendingRestart();
}


void WebServerManager::scheduleRestart(unsigned long delayMs) {
    restartPending = true;
    restartAtMs = millis() + delayMs;
    UnifiedLogger::info("[WEB] Restart scheduled in %lu ms\n", delayMs);
}

void WebServerManager::processPendingRestart() {
    if (!restartPending) {
        return;
    }

    if (static_cast<long>(millis() - restartAtMs) >= 0) {
        restartPending = false;
        UnifiedLogger::info("[WEB] Executing deferred restart\n");
        ESP.restart();
    }
}

void WebServerManager::lockRelayStatusApiDiagnostics() const {
#if defined(ARDUINO_ARCH_ESP32)
    portENTER_CRITICAL(&relayStatusApiDiagnosticsLock);
#endif
}

void WebServerManager::unlockRelayStatusApiDiagnostics() const {
#if defined(ARDUINO_ARCH_ESP32)
    portEXIT_CRITICAL(&relayStatusApiDiagnosticsLock);
#endif
}

uint32_t WebServerManager::computeP95FromSamples(const uint16_t* samples, uint16_t count) {
    if (!samples || count == 0) {
        return 0;
    }

    uint16_t sorted[128] = {0};
    const uint16_t boundedCount = static_cast<uint16_t>(std::min<size_t>(count, sizeof(sorted) / sizeof(sorted[0])));
    memcpy(sorted, samples, static_cast<size_t>(boundedCount) * sizeof(uint16_t));
    std::sort(sorted, sorted + boundedCount);

    size_t rank = ((static_cast<size_t>(boundedCount) * 95U) + 99U) / 100U;
    if (rank == 0) {
        rank = 1;
    }
    if (rank > boundedCount) {
        rank = boundedCount;
    }
    return static_cast<uint32_t>(sorted[rank - 1U]);
}

void WebServerManager::recordRelayStatusApiRequest(int statusCode, uint32_t durationMs) {
    lockRelayStatusApiDiagnostics();

    relayStatusApiDiagnostics.requestsTotal++;
    if (statusCode >= 200 && statusCode < 300) {
        relayStatusApiDiagnostics.status2xx++;
    } else if (statusCode >= 400 && statusCode < 500) {
        relayStatusApiDiagnostics.status4xx++;
    } else if (statusCode >= 500 && statusCode < 600) {
        relayStatusApiDiagnostics.status5xx++;
    } else {
        relayStatusApiDiagnostics.statusOther++;
    }

    relayStatusApiDiagnostics.totalLatencyMs += durationMs;
    if (durationMs > relayStatusApiDiagnostics.maxLatencyMs) {
        relayStatusApiDiagnostics.maxLatencyMs = durationMs;
    }

    const size_t sampleCapacity = sizeof(relayStatusApiDiagnostics.latencySamples) / sizeof(relayStatusApiDiagnostics.latencySamples[0]);
    const uint16_t sample = static_cast<uint16_t>(std::min<uint32_t>(durationMs, 65535U));
    relayStatusApiDiagnostics.latencySamples[relayStatusApiDiagnostics.latencySampleWriteIndex] = sample;
    relayStatusApiDiagnostics.latencySampleWriteIndex =
        static_cast<uint16_t>((relayStatusApiDiagnostics.latencySampleWriteIndex + 1U) % sampleCapacity);
    if (relayStatusApiDiagnostics.latencySampleCount < sampleCapacity) {
        relayStatusApiDiagnostics.latencySampleCount++;
    }

    const unsigned long nowMs = millis();
    relayStatusApiDiagnostics.lastRequestMs = nowMs;
    if (statusCode != 200) {
        relayStatusApiDiagnostics.lastNon200Status = statusCode;
        relayStatusApiDiagnostics.lastNon200Ms = nowMs;
    }

    unlockRelayStatusApiDiagnostics();
}

void WebServerManager::resetRelayStatusApiDiagnostics() {
    lockRelayStatusApiDiagnostics();
    relayStatusApiDiagnostics = RelayStatusApiDiagnosticsData();
    unlockRelayStatusApiDiagnostics();
}

WebServerManager::RelayStatusApiDiagnosticsSnapshot WebServerManager::getRelayStatusApiDiagnosticsSnapshot() const {
    RelayStatusApiDiagnosticsSnapshot snapshot;
    uint16_t latencySamples[128] = {0};
    uint16_t latencySampleCount = 0;

    lockRelayStatusApiDiagnostics();
    snapshot.requestsTotal = relayStatusApiDiagnostics.requestsTotal;
    snapshot.status2xx = relayStatusApiDiagnostics.status2xx;
    snapshot.status4xx = relayStatusApiDiagnostics.status4xx;
    snapshot.status5xx = relayStatusApiDiagnostics.status5xx;
    snapshot.statusOther = relayStatusApiDiagnostics.statusOther;
    snapshot.maxLatencyMs = relayStatusApiDiagnostics.maxLatencyMs;
    snapshot.lastNon200Status = relayStatusApiDiagnostics.lastNon200Status;

    if (relayStatusApiDiagnostics.requestsTotal > 0) {
        snapshot.avgLatencyMs = static_cast<uint32_t>(relayStatusApiDiagnostics.totalLatencyMs / relayStatusApiDiagnostics.requestsTotal);
    }

    latencySampleCount = relayStatusApiDiagnostics.latencySampleCount;
    if (latencySampleCount > 0) {
        memcpy(latencySamples,
               relayStatusApiDiagnostics.latencySamples,
               static_cast<size_t>(latencySampleCount) * sizeof(uint16_t));
    }

    const unsigned long nowMs = millis();
    if (relayStatusApiDiagnostics.lastRequestMs > 0 && nowMs >= relayStatusApiDiagnostics.lastRequestMs) {
        snapshot.lastRequestAgeMs = nowMs - relayStatusApiDiagnostics.lastRequestMs;
    }
    if (relayStatusApiDiagnostics.lastNon200Ms > 0 && nowMs >= relayStatusApiDiagnostics.lastNon200Ms) {
        snapshot.lastNon200AgeMs = nowMs - relayStatusApiDiagnostics.lastNon200Ms;
    }
    unlockRelayStatusApiDiagnostics();

    snapshot.p95LatencyMs = computeP95FromSamples(latencySamples, latencySampleCount);
    return snapshot;
}

void WebServerManager::registerEndpoints() {
    registerTimedEndpointAdapter("/", webserver::RequestMethod::Get, [this](webserver::IRequest& request, webserver::IResponse& response) { handleRoot(request, response); });
    registerTimedEndpointAdapter("/settings", webserver::RequestMethod::Get, [this](webserver::IRequest& request, webserver::IResponse& response) { handleSettings(request, response); });
    registerTimedEndpointAdapter("/ota", webserver::RequestMethod::Get, [this](webserver::IRequest& request, webserver::IResponse& response) { handleOta(request, response); });
    registerTimedEndpointAdapter("/detection", webserver::RequestMethod::Get, [this](webserver::IRequest& request, webserver::IResponse& response) { handleDetection(request, response); });
    registerTimedEndpointAdapter("/monitoring", webserver::RequestMethod::Get, [this](webserver::IRequest& request, webserver::IResponse& response) { handleMonitoring(request, response); });
    registerTimedEndpointAdapter("/monitoring", webserver::RequestMethod::Post, [this](webserver::IRequest& request, webserver::IResponse& response) { handleMonitoring(request, response); });
    registerTimedEndpointAdapter("/performance", webserver::RequestMethod::Get, [this](webserver::IRequest& request, webserver::IResponse& response) { handlePerformance(request, response); });
    registerTimedEndpointAdapter("/performance", webserver::RequestMethod::Post, [this](webserver::IRequest& request, webserver::IResponse& response) { handlePerformance(request, response); });
    registerTimedEndpointAdapter("/relay", webserver::RequestMethod::Get, [this](webserver::IRequest& request, webserver::IResponse& response) { handleRelay(request, response); });
    registerTimedEndpointAdapter("/debug", webserver::RequestMethod::Get, [this](webserver::IRequest& request, webserver::IResponse& response) { handleDebug(request, response); });

    // Favicons (browsers commonly request these automatically)
    registerTimedEndpointAdapter("/favicon.ico", webserver::RequestMethod::Get, [this](webserver::IRequest& request, webserver::IResponse& response) { handleFaviconIco(request, response); });
    registerTimedEndpointAdapter("/favicon.svg", webserver::RequestMethod::Get, [this](webserver::IRequest& request, webserver::IResponse& response) { handleFaviconSvg(request, response); });

    registerTimedEndpointAdapter("/api/settings", webserver::RequestMethod::Get, [this](webserver::IRequest& request, webserver::IResponse& response) { handleApiSettingsGet(request, response); });
    registerTimedEndpointAdapter("/api/settings", webserver::RequestMethod::Post, [this](webserver::IRequest& request, webserver::IResponse& response) { handleApiSettingsPost(request, response); });
    registerTimedEndpointAdapter("/api/reboot", webserver::RequestMethod::Post, [this](webserver::IRequest& request, webserver::IResponse& response) { handleApiReboot(request, response); });
    registerTimedEndpointAdapter("/api/config", webserver::RequestMethod::Get, [this](webserver::IRequest& request, webserver::IResponse& response) { handleApiConfigGet(request, response); });
    registerTimedEndpointAdapter("/api/home/sensors", webserver::RequestMethod::Get, [this](webserver::IRequest& request, webserver::IResponse& response) { handleApiHomeSensors(request, response); });
    registerTimedEndpointAdapter("/api/ota/status", webserver::RequestMethod::Get, [this](webserver::IRequest& request, webserver::IResponse& response) { handleApiOtaStatus(request, response); });
    registerTimedEndpointAdapter("/api/ota/arm", webserver::RequestMethod::Post, [this](webserver::IRequest& request, webserver::IResponse& response) { handleApiOtaArm(request, response); });
    registerTimedEndpointAdapter("/api/monitoring/cards", webserver::RequestMethod::Get, [this](webserver::IRequest& request, webserver::IResponse& response) { handleApiMonitoringCards(request, response); });
    registerTimedEndpointAdapter("/api/errors", webserver::RequestMethod::Get, [this](webserver::IRequest& request, webserver::IResponse& response) { handleApiErrorsGet(request, response); });
    registerTimedEndpointAdapter("/api/errors/logs", webserver::RequestMethod::Get, [this](webserver::IRequest& request, webserver::IResponse& response) { handleApiErrorHistoryGet(request, response); });
    registerTimedEndpointAdapter("/api/performance/cards", webserver::RequestMethod::Get, [this](webserver::IRequest& request, webserver::IResponse& response) { handleApiPerformanceCards(request, response); });
    registerTimedEndpointAdapter("/scan", webserver::RequestMethod::Get, [this](webserver::IRequest& request, webserver::IResponse& response) { handleScan(request, response); });

    // NTP API endpoints
    registerTimedEndpointAdapter("/api/ntp/status", webserver::RequestMethod::Get, [this](webserver::IRequest& request, webserver::IResponse& response) { handleApiNtpStatus(request, response); });
    registerTimedEndpointAdapter("/api/ntp/logs", webserver::RequestMethod::Get, [this](webserver::IRequest& request, webserver::IResponse& response) { handleApiNtpLogs(request, response); });

    // Relay API endpoints
    registerTimedEndpointAdapter("/api/relay/status", webserver::RequestMethod::Get, [this](webserver::IRequest& request, webserver::IResponse& response) { handleApiRelayStatus(request, response); });
    registerTimedEndpointAdapter("/api/relay/set", webserver::RequestMethod::Post, [this](webserver::IRequest& request, webserver::IResponse& response) { handleApiRelaySet(request, response); });
    registerTimedEndpointAdapter("/api/relay/logs", webserver::RequestMethod::Get, [this](webserver::IRequest& request, webserver::IResponse& response) { handleApiRelayLogs(request, response); });

    // Debug log viewer (UI-only)
    registerTimedEndpointAdapter("/api/debug/enable", webserver::RequestMethod::Post, [this](webserver::IRequest& request, webserver::IResponse& response) { handleApiDebugEnable(request, response); });
    registerTimedEndpointAdapter("/api/debug/logs", webserver::RequestMethod::Get, [this](webserver::IRequest& request, webserver::IResponse& response) { handleApiDebugLogs(request, response); });
    registerTimedEndpointAdapter("/api/debug/clear", webserver::RequestMethod::Post, [this](webserver::IRequest& request, webserver::IResponse& response) { handleApiDebugClear(request, response); });

    // Direct relay control endpoints (NEW)
    registerTimedEndpointAdapter("/relay/off", webserver::RequestMethod::Get, [this](webserver::IRequest& request, webserver::IResponse& response) { handleRelayControl("off", request, response); });
    registerTimedEndpointAdapter("/relay/low", webserver::RequestMethod::Get, [this](webserver::IRequest& request, webserver::IResponse& response) { handleRelayControl("low", request, response); });
    registerTimedEndpointAdapter("/relay/mid", webserver::RequestMethod::Get, [this](webserver::IRequest& request, webserver::IResponse& response) { handleRelayControl("mid", request, response); });
    registerTimedEndpointAdapter("/relay/high", webserver::RequestMethod::Get, [this](webserver::IRequest& request, webserver::IResponse& response) { handleRelayControl("high", request, response); });


}

void WebServerManager::registerTimedEndpointAdapter(const char* uri, webserver::RequestMethod method, AdapterHandler handler) {
    if (!backend) {
        UnifiedLogger::warning("[WEB] Skipping route registration before backend init: %s\n", uri ? uri : "<null>");
        return;
    }

    backend->registerRoute(uri, method, [this, handler](webserver::IRequest& request, webserver::IResponse& response) {
        unsigned long start = micros();
        handler(request, response);
        unsigned long duration = (micros() - start) / 1000;
        if (mainController) mainController->updateComponentStatsPublic(WEB_SERVER_INDEX, duration);
    });
}

String WebServerManager::getNavHtml(String current) {
    // 1. CSS optimized to match your relay-button design exactly
    String html = "<style>";
    html += ".nav-bar { background: #2d2d2d; padding: 5px; border-bottom: 1px solid #424242; position: fixed; top: 0; left: 0; right: 0; z-index: 1000; width: 100%; box-sizing: border-box; display: flex; gap: 10px; }";
    html += ".nav-btn { padding: 10px 18px; font-size: 1.0em; font-weight: bold; text-decoration: none; text-align: center; border-radius: 4px; transition: all 0.3s; color: white; ";
    html += "background: linear-gradient(135deg, #666, #555); box-shadow: 0 4px 8px rgba(0,0,0,0.3), inset 0 1px 0 rgba(255,255,255,0.2); }";
    html += ".nav-btn:hover { transform: translateY(-2px); box-shadow: 0 6px 12px rgba(0,0,0,0.4), inset 0 1px 0 rgba(255,255,255,0.3); background: linear-gradient(135deg, #777, #666); }";
    // Active state uses the same Blue Glow as your relay 'active' logic
    html += ".nav-btn.active { background: linear-gradient(135deg, #2196f3, #1976d2); box-shadow: 0 0 15px rgba(33, 150, 243, 0.8), inset 0 1px 0 rgba(255,255,255,0.4); transform: scale(1.05); border: 1px solid #64b5f6; }";
    html += "</style>";

    // 2. The Navigation Bar
    html += "<div class='nav-bar'>";
    
    // 3. The Buttons using unified classes
    html += String("<a href='/' class='nav-btn ") + (current == "home" ? "active" : "") + "'>Home</a>";
    html += String("<a href='/settings' class='nav-btn ") + (current == "settings" ? "active" : "") + "'>Settings</a>";
    html += String("<a href='/detection' class='nav-btn ") + (current == "detection" ? "active" : "") + "'>Detection</a>";
    html += String("<a href='/monitoring' class='nav-btn ") + (current == "monitoring" ? "active" : "") + "'>Monitoring</a>";
    html += String("<a href='/performance' class='nav-btn ") + (current == "performance" ? "active" : "") + "'>Performance</a>";
    html += String("<a href='/relay' class='nav-btn ") + (current == "relay" ? "active" : "") + "'>Relay</a>";
    html += String("<a href='/debug' class='nav-btn ") + (current == "debug" ? "active" : "") + "'>Debug</a>";
    html += String("<a href='/ota' class='nav-btn ") + (current == "ota" ? "active" : "") + "'>OTA</a>";
    
    html += "</div>";
    return html;
}


void WebServerManager::handleRoot(webserver::IRequest& request, webserver::IResponse& response) {
    if (!checkAuthentication(request, response)) {
        return; // Authentication failed, response already sent
    }

    if (!isWiFiConnected()) {
        response.send(503, kContentTypeHtmlUtf8, "<html><body><h1>Service Unavailable</h1><p>WiFi connection lost. Please check network connectivity.</p></body></html>");
        return;
    }

    if (!configManager) {
        response.send(500, kContentTypeHtmlUtf8, "Configuration manager not set");
        return;
    }

    const Settings settings = configManager->getSettingsSnapshot();
    const unsigned long baseIntervalMs = max(1000UL, static_cast<unsigned long>(settings.sensors.ds18b20_measurement_interval * 1000.0f));
    const unsigned long initialReadouts = ds18b20Manager ? ds18b20Manager->getCompletedReadoutCount() : 0;

    String html = String("<html><head><title>Home</title>") +
                  "<style>body { background-color: #1e1e1e; color: #ffffff; font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif; margin: 0; padding: 60px 20px 20px 20px; } " +
                  "h1 { color: #2196f3; } p { color: #b0b0b0; }</style></head><body>" + getNavHtml("home");
    html += "<div id='homeSensorsRoot'>";
    html += generateHomeSensorsHtml(settings);
    html += "</div>";
    html += "<script>";
    html += "const HOME_BASE_INTERVAL_MS=" + String(baseIntervalMs) + ";";
    html += "const HOME_RETRY_INTERVAL_MS=Math.min(2000,Math.max(500,Math.floor(HOME_BASE_INTERVAL_MS/10)));";
    html += "let homeLastReadouts=" + String(initialReadouts) + ";";
    html += "let homePollTimer=null;";
    html += "let homePollInFlight=false;";
    html += "function scheduleHomePoll(delayMs){if(homePollTimer){clearTimeout(homePollTimer);}homePollTimer=setTimeout(()=>{refreshHomeSensors().catch(()=>{});},delayMs);}";
    html += "async function refreshHomeSensors(force){if(homePollInFlight){return;}if(!force&&document.visibilityState!=='visible'){scheduleHomePoll(HOME_RETRY_INTERVAL_MS);return;}homePollInFlight=true;try{const response=await fetch('/api/home/sensors?t='+Date.now(),{credentials:'same-origin',cache:'no-store'});if(!response.ok){throw new Error('HTTP '+response.status);}const readoutsHeader=response.headers.get('X-DS18-Readouts');const readouts=parseInt(readoutsHeader||'-1',10);const sensorsHtml=await response.text();const hasNewReadout=!Number.isNaN(readouts)&&readouts>homeLastReadouts;if(hasNewReadout||homeLastReadouts===0){const root=document.getElementById('homeSensorsRoot');if(root){root.innerHTML=sensorsHtml;}if(!Number.isNaN(readouts)&&readouts>=0){homeLastReadouts=readouts;}scheduleHomePoll(HOME_BASE_INTERVAL_MS);}else{scheduleHomePoll(HOME_RETRY_INTERVAL_MS);}}catch(error){console.log('Home refresh failed:',error);scheduleHomePoll(HOME_RETRY_INTERVAL_MS);}finally{homePollInFlight=false;}}";
    html += "document.addEventListener('visibilitychange',()=>{if(document.visibilityState==='visible'){refreshHomeSensors(true).catch(()=>{});}});";
    html += "scheduleHomePoll(HOME_BASE_INTERVAL_MS);";
    html += "</script>";
    html += "</body></html>";
    response.send(200, kContentTypeHtmlUtf8, html);
}

String WebServerManager::generateHomeSensorsHtml(const Settings& settings) {
    String html;

    if (ds18b20Manager) {
        html += "<h1>DS18B20 Temperatures</h1>";
        for (int i = 0; i < ds18b20Manager->getSensorCount(); i++) {
            float temp = ds18b20Manager->getTemperature(i);
            bool operational = ds18b20Manager->isSensorOperational(i);
            bool suppress85 = ds18b20Manager->isLikelyPowerOnDefault85C(i);
            bool parasitic = ds18b20Manager->getParasiticPowerStatus(i);

            String status = (suppress85 && operational) ? "WARMING_UP" : (operational ? "OK" : "NOT OPERATIONAL");
            String powerMode = parasitic ? "PARASITIC" : "EXTERNAL";

            String topicLabel = "";
            if (i >= 0 && i < 32 && settings.mqtt.topics.temp[i].length() > 0) {
                topicLabel = " [" + htmlEscape(settings.mqtt.topics.temp[i]) + "]";
            }

            html += "<p>Sensor" + String(i + 1) + topicLabel + " ";
            if (!operational || suppress85 || temp == -127.0f) {
                html += "NA";
            } else {
                html += String(temp, 1) + "&deg;C";
            }
            html += " [" + status + ", " + powerMode + "]</p>";
        }
    }

    if (sht4xManager) {
        html += "<h1>SHT4x Temperatures</h1>";
        float temp = sht4xManager->getTemperature();
        float humidity = sht4xManager->getHumidity();
        bool operational = sht4xManager->isSensorOperational();
        String status = operational ? "OK" : "NOT OPERATIONAL";

        html += "<p>Temperature: ";
        html += isnan(temp) ? "NA" : (String(temp, 1) + "&deg;C");
        html += " [" + status + "]</p>";

        html += "<p>Humidity: ";
        html += isnan(humidity) ? "NA" : (String(humidity, 1) + "%");
        html += " [" + status + "]</p>";
    }

    if (max31855kManager) {
        html += "<h1>MAX31855K Thermocouple</h1>";
        float tc = max31855kManager->getThermocoupleC();
        float internal = max31855kManager->getInternalJunctionC();
        const char* status = max31855kManager->getStatusString();

        html += "<p>Thermocouple: ";
        html += isnan(tc) ? "NA" : (String(tc, 1) + "&deg;C");
        html += " [" + String(status) + "]</p>";

        html += "<p>Internal Junction: ";
        html += isnan(internal) ? "NA" : (String(internal, 1) + "&deg;C");
        html += " [" + String(status) + "]</p>";
    }

    return html;
}

void WebServerManager::handleApiHomeSensors(webserver::IRequest& request, webserver::IResponse& response) {
    if (!checkAuthentication(request, response)) {
        return;
    }

    if (!isWiFiConnected()) {
        response.send(503, kContentTypeHtmlUtf8, "<p>Service unavailable: WiFi disconnected.</p>");
        return;
    }

    if (!configManager) {
        response.send(500, kContentTypeHtmlUtf8, "<p>Configuration manager not set.</p>");
        return;
    }

    const Settings settings = configManager->getSettingsSnapshot();
    const unsigned long readoutCount = ds18b20Manager ? ds18b20Manager->getCompletedReadoutCount() : 0;
    response.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate");
    response.sendHeader("X-DS18-Readouts", String(readoutCount));
    response.send(200, kContentTypeHtmlUtf8, generateHomeSensorsHtml(settings));
}

void WebServerManager::handleSettings(webserver::IRequest& request, webserver::IResponse& response) {
    if (!checkAuthentication(request, response)) {
        return; // Authentication failed, response already sent
    }

    if (!isWiFiConnected()) {
        response.send(503, kContentTypeHtmlUtf8, "<html><body><h1>Service Unavailable</h1><p>WiFi connection lost. Please check network connectivity.</p></body></html>");
        return;
    }

    if (!configManager) {
        response.send(500, kContentTypeHtmlUtf8, "Configuration manager not set");
        return;
    }

    response.setContentLength(webserver::kContentLengthUnknown);
    response.send(200, kContentTypeHtmlUtf8, "");
    streamSettingsHtml(response);
    response.sendContent("");
}

void WebServerManager::handleOta(webserver::IRequest& request, webserver::IResponse& response) {
    if (!checkAuthentication(request, response)) {
        return; // Authentication failed, response already sent
    }

    if (!isWiFiConnected()) {
        response.send(503, kContentTypeHtmlUtf8,
                    "<html><body><h1>Service Unavailable</h1><p>WiFi connection lost. Please check network connectivity.</p></body></html>");
        return;
    }

    if (!mainController) {
        response.send(500, kContentTypeHtmlUtf8, "MainController not set");
        return;
    }

    String html = "<html><head><title>OTA</title>";
    html += "<style>body{background-color:#1e1e1e;color:#ffffff;font-family:'Segoe UI',Tahoma,Geneva,Verdana,sans-serif;margin:0;padding:60px 20px 20px 20px;}";
    html += "h1{color:#2196f3;} .card{background:#2d2d2d;border:1px solid #424242;border-radius:6px;padding:14px;margin:10px 0;}";
    html += ".muted{color:#b0b0b0;} .ok{color:#64b5f6;} .err{color:#ff6b6b;} .warn{color:#ffb74d;}";
    html += "pre{background:#111;border:1px solid #333;border-radius:6px;padding:10px;white-space:pre-wrap;word-break:break-word;}";
    html += ".btn{display:inline-block;padding:10px 18px;font-size:1.0em;font-weight:bold;text-align:center;border-radius:4px;border:none;cursor:pointer;";
    html += "color:white;background:linear-gradient(135deg,#666,#555);box-shadow:0 4px 8px rgba(0,0,0,0.3),inset 0 1px 0 rgba(255,255,255,0.2);}";
    html += ".btn:hover{transform:translateY(-2px);box-shadow:0 6px 12px rgba(0,0,0,0.4),inset 0 1px 0 rgba(255,255,255,0.3);}";
    html += ".status-item{display:flex;justify-content:flex-start;align-items:baseline;gap:6px;margin:4px 0;}";
    html += ".status-item .label{color:#b0b0b0;}";
    html += ".status-item .value{color:#ffffff !important;font-weight:600;}";
    html += ".ota-main-row{display:flex;gap:10px;align-items:stretch;}";
    html += ".ota-status-card{flex:0 0 25%;max-width:25%;min-width:260px;}";
    html += ".ota-platform-card{flex:1 1 auto;}";
    html += "@media (max-width:1100px){.ota-main-row{flex-direction:column;}.ota-status-card,.ota-platform-card{max-width:none;min-width:0;}}";
    html += "</style></head><body>";

    html += getNavHtml("ota");
    html += "<h1>OTA Updates</h1>";

    html += "<div class='card'>";
    html += "<button id='otaArmBtn' class='btn' type='button'>Arm OTA</button>";
    html += "<div id='otaActionStatus' class='muted' style='margin-top:10px;'>Status: idle</div>";
    html += "</div>";

    html += "<div class='ota-main-row'>";
    html += "<div class='card ota-status-card'>";
    html += "<b id='otaConfigHeadline' class='muted'>Loading OTA status...</b><br>";
    html += "<div id='otaConfigError' class='err' style='display:none; margin-top:6px;'></div>";
    html += "<div class='status-item'><span class='label'>Port:</span><span id='otaPort' class='value'>-</span></div>";
    html += "<div class='status-item'><span class='label'>Window:</span><span id='otaWindow' class='value'>-</span></div>";
    html += "<div class='status-item'><span class='label'>Armed:</span><span id='otaArmed' class='value'>-</span></div>";
    html += "<div class='status-item'><span class='label'>Remaining:</span><span id='otaRemaining' class='value'>-</span></div>";
    html += "<div class='status-item'><span class='label'>In progress:</span><span id='otaInProgress' class='value'>-</span></div>";
    html += "<div class='status-item'><span class='label'>Type:</span><span id='otaType' class='value'>-</span></div>";
    html += "<div class='status-item'><span class='label'>Progress:</span><span id='otaProgress' class='value'>-</span></div>";
    html += "<div class='status-item'><span class='label'>Last error:</span><span id='otaLastError' class='value'>-</span></div>";
    html += "<div id='otaPollHealth' class='muted' style='margin-top:10px;'>Live status: connecting...</div>";
    html += "</div>";

    html += "<div class='card ota-platform-card'>";
    html += "<b>PlatformIO</b><br>";
    html += "<span class='muted'>Preferred target:</span><br>";
    html += "<pre id='otaCmdPreferred'>loading...</pre>";
    html += "<span class='muted'>Deterministic IP fallback:</span><br>";
    html += "<pre id='otaCmdIp'>loading...</pre>";
    html += "<span class='muted'>USB (unchanged):</span><br>";
    html += "<pre>pio run -t erase; pio run -t clean; pio run -t upload</pre>";
    html += "</div>";
    html += "</div>";

    html += "<div class='card'><b>Device Log</b><pre id='otaLogText'>Loading...</pre></div>";

    html += "<script>";
    html += "const OTA_POLL_MS=3000;";
    html += "let otaPollInFlight=false;";
    html += "let otaPollFailCount=0;";
    html += "const actionStatusEl=document.getElementById('otaActionStatus');";
    html += "const pollHealthEl=document.getElementById('otaPollHealth');";
    html += "function setActionStatus(text,cls){actionStatusEl.textContent='Status: '+text;actionStatusEl.className=cls||'muted';}";
    html += "function setPollHealth(text,cls){if(!pollHealthEl){return;}pollHealthEl.textContent='Live status: '+text;pollHealthEl.className=cls||'muted';}";
    html += "function setText(id,text){const el=document.getElementById(id);if(el){el.textContent=text;}}";
    html += "function setCmd(id,text){const el=document.getElementById(id);if(el){el.textContent=text;}}";
    html += "function normText(v,f){if(v===null||v===undefined){return f;}const s=String(v).trim();return s.length?s:f;}";
    html += "function normInt(v,f){const n=Number(v);return Number.isFinite(n)?String(Math.trunc(n)):f;}";
    html += "function renderOtaStatus(data){";
    html += "  const valid=!!data.config_valid;";
    html += "  const headline=document.getElementById('otaConfigHeadline');";
    html += "  const errEl=document.getElementById('otaConfigError');";
    html += "  if(headline){headline.textContent=valid?'ota.json OK':'ota.json invalid';headline.className=valid?'ok':'err';}";
    html += "  if(errEl){if(valid){errEl.style.display='none';errEl.textContent='';}else{errEl.style.display='block';errEl.textContent=(data.config_error||'Unknown config error');}}";
    html += "  const port=normInt(data.port,'-');";
    html += "  const windowSeconds=normInt(data.window_seconds,'-');";
    html += "  const remainingSeconds=normInt(data.remaining_seconds,'-');";
    html += "  const progressPercent=normInt(data.progress_percent,'-');";
    html += "  const armed=(data.armed===true)?'true':((data.armed===false)?'false':'-');";
    html += "  const inProgress=(data.in_progress===true)?'true':((data.in_progress===false)?'false':'-');";
    html += "  setText('otaPort',port);";
    html += "  setText('otaWindow',windowSeconds==='-'?'-':(windowSeconds+'s'));";
    html += "  setText('otaArmed',armed);";
    html += "  setText('otaRemaining',remainingSeconds==='-'?'-':(remainingSeconds+'s'));";
    html += "  setText('otaInProgress',inProgress);";
    html += "  setText('otaType',normText(data.command,'-'));";
    html += "  setText('otaProgress',progressPercent==='-'?'-':(progressPercent+'%'));";
    html += "  setText('otaLastError',normText(data.last_error,'-'));";
    html += "  const host=data.host_target||'ip_or_hostname';";
    html += "  const ip=data.ip_target||'0.0.0.0';";
    html += "  setCmd('otaCmdPreferred','pio run -e esp32-s3-devkitc-1-ota -t upload --upload-port '+host);";
    html += "  setCmd('otaCmdIp','pio run -e esp32-s3-devkitc-1-ota -t upload --upload-port '+ip);";
    html += "  setText('otaLogText',data.log_text||'');";
    html += "  otaPollFailCount=0;";
    html += "  setPollHealth('connected','ok');";
    html += "}";
    html += "async function fetchOtaStatus(force){if(otaPollInFlight){return;}if(!force&&document.visibilityState!=='visible'){return;}otaPollInFlight=true;try{const r=await fetch('/api/ota/status?t='+Date.now(),{credentials:'same-origin',cache:'no-store'});const j=await r.json().catch(()=>({}));if(!r.ok){throw new Error(j.error||('HTTP '+r.status));}renderOtaStatus(j);}catch(e){otaPollFailCount++;if(otaPollFailCount<3){setPollHealth('retrying... ('+otaPollFailCount+')','warn');}else{setPollHealth('connection unstable: '+e.message,'err');}}finally{otaPollInFlight=false;}}";
    html += "function isIpv4Host(host){return /^(?:\\d{1,3}\\.){3}\\d{1,3}$/.test(host||'');}";
    html += "function maybeRedirectToIp(ip){if(!isIpv4Host(ip)){return false;}const currentHost=window.location.hostname||'';if(currentHost===ip){return false;}const targetUrl=window.location.protocol+'//'+ip+window.location.pathname+window.location.search+window.location.hash;setActionStatus('OTA armed, switching to '+ip+' for stable upload target...','warn');window.location.replace(targetUrl);return true;}";
    html += "async function postOtaAction(url,successPrefix,opts){try{const r=await fetch(url,{method:'POST',credentials:'same-origin'});const j=await r.json().catch(()=>({}));if(!r.ok||!j.ok){throw new Error((j&&j.error)?j.error:('HTTP '+r.status));}setActionStatus((j&&j.message)?j.message:successPrefix,'ok');if(opts&&opts.redirectToIpOnSuccess&&j&&j.ip_target){if(maybeRedirectToIp(j.ip_target)){return;}}await fetchOtaStatus(true);}catch(e){setActionStatus(e.message,'err');}}";
    html += "document.getElementById('otaArmBtn').addEventListener('click',()=>{postOtaAction('/api/ota/arm','OTA armed',{redirectToIpOnSuccess:true});});";
    html += "setInterval(()=>{fetchOtaStatus(false).catch(()=>{});},OTA_POLL_MS);";
    html += "document.addEventListener('visibilitychange',()=>{if(document.visibilityState==='visible'){fetchOtaStatus(true).catch(()=>{});}});";
    html += "fetchOtaStatus(true).catch(()=>{});";
    html += "</script>";

    html += "</body></html>";

    response.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate");
    response.send(200, kContentTypeHtmlUtf8, html);
}

void WebServerManager::handleApiOtaStatus(webserver::IRequest& request, webserver::IResponse& response) {
    if (!checkAuthentication(request, response)) {
        return;
    }

    if (!isWiFiConnected()) {
        response.send(503, kContentTypeJsonUtf8, "{\"ok\":false,\"error\":\"WiFi connection lost\"}");
        return;
    }

    if (!mainController) {
        response.send(500, kContentTypeJsonUtf8, "{\"ok\":false,\"error\":\"MainController not set\"}");
        return;
    }

    OtaStatusSnapshot status = mainController->getOtaStatus();
    const String logText = mainController->getOtaLogText();
    String otaHostTarget = "ip_or_hostname";
    if (configManager) {
        const Settings settings = configManager->getSettingsSnapshot();
        if (settings.network.mdns_enabled && settings.network.hostname.length() > 0) {
            otaHostTarget = settings.network.hostname + ".local";
        }
    }

    PsramJsonAllocator jsonAllocator;
    JsonDocument doc(&jsonAllocator);
    doc["ok"] = true;
    doc["config_valid"] = status.configValid;
    doc["config_error"] = status.configError;
    doc["port"] = status.port;
    doc["window_seconds"] = status.windowSeconds;
    doc["armed"] = status.armed;
    doc["remaining_seconds"] = status.remainingSeconds;
    doc["in_progress"] = status.inProgress;
    doc["progress_percent"] = status.progressPercent;
    doc["command"] = status.command;
    doc["last_error"] = status.lastError;
    doc["host_target"] = otaHostTarget;
    doc["ip_target"] = WiFi.localIP().toString();
    doc["log_text"] = logText;

    String json;
    serializeJson(doc, json);
    response.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate");
    response.send(200, kContentTypeJsonUtf8, json);
}

void WebServerManager::handleApiOtaArm(webserver::IRequest& request, webserver::IResponse& response) {
    if (!checkAuthentication(request, response)) {
        return;
    }

    if (!isWiFiConnected()) {
        response.send(503, kContentTypeJsonUtf8, "{\"ok\":false,\"error\":\"WiFi connection lost\"}");
        return;
    }

    if (!mainController) {
        response.send(500, kContentTypeJsonUtf8, "{\"ok\":false,\"error\":\"MainController not set\"}");
        return;
    }

    MainController::ControlCommand armCommand;
    armCommand.type = MainController::ControlCommandType::OTA_ARM_WINDOW;
    const webserver::ControlDispatchWaitResult armResult =
        webserver::dispatchControlCommandAndWait(
            mainController,
            armCommand,
            kOtaControlDispatchTimeoutMs,
            kControlDispatchPollIntervalMs);

    if (armResult.status == webserver::ControlDispatchWaitStatus::EnqueueRejected) {
        response.send(429, kContentTypeJsonUtf8, "{\"ok\":false,\"error\":\"OTA not armed: control command queue busy\"}");
        return;
    }
    if (armResult.status == webserver::ControlDispatchWaitStatus::TimedOut) {
        response.send(504, kContentTypeJsonUtf8, "{\"ok\":false,\"error\":\"OTA not armed: request wait timeout\"}");
        return;
    }
    if (armResult.status != webserver::ControlDispatchWaitStatus::Completed) {
        response.send(500, kContentTypeJsonUtf8, "{\"ok\":false,\"error\":\"OTA not armed: internal command dispatch failed\"}");
        return;
    }
    if (armResult.commandResult.status == MainController::ControlCommandStatus::TIMED_OUT) {
        response.send(503, kContentTypeJsonUtf8, "{\"ok\":false,\"error\":\"OTA not armed: queue timeout\"}");
        return;
    }
    if (armResult.commandResult.status != MainController::ControlCommandStatus::SUCCEEDED) {
        String json = "{\"ok\":false,\"error\":\"OTA not armed: " + String(armResult.commandResult.message) + "\"}";
        response.send(400, kContentTypeJsonUtf8, json);
        return;
    }

    OtaStatusSnapshot status = mainController->getOtaStatus();
    const String ipTarget = WiFi.localIP().toString();
    String json = "{\"ok\":true,\"message\":\"OTA armed for " + String(status.windowSeconds) + " seconds.\",\"ip_target\":\"" + ipTarget + "\"}";
    response.send(200, kContentTypeJsonUtf8, json);
}

void WebServerManager::handleFaviconIco(webserver::IRequest& request, webserver::IResponse& response) {
    if (!checkAuthentication(request, response)) {
        return; // Authentication failed, response already sent
    }

    // Prefer a real ICO from LittleFS if present (best browser compatibility).
    File icoMeta = LittleFS.open("/favicon.ico", "r");
    if (icoMeta && icoMeta.size() > 0) {
        const size_t icoSize = icoMeta.size();
        icoMeta.close();

        // Guard against accidentally huge assets.
        if (icoSize <= 64 * 1024) {
            response.sendHeader("Cache-Control", "public, max-age=86400");
            if (response.sendFile(LittleFS, "/favicon.ico", "image/x-icon") > 0) {
                return;
            }
        }
    }

    // Fallback: redirect to SVG favicon (small, easy to maintain).
    response.sendHeader("Location", "/favicon.svg");
    response.sendHeader("Cache-Control", "public, max-age=86400");
    response.send(302, kContentTypePlainUtf8, "");
}

void WebServerManager::handleFaviconSvg(webserver::IRequest& request, webserver::IResponse& response) {
    if (!checkAuthentication(request, response)) {
        return; // Authentication failed, response already sent
    }

    // Serve custom SVG favicon from LittleFS if present.
    File svgMeta = LittleFS.open("/favicon.svg", "r");
    if (svgMeta && svgMeta.size() > 0) {
        const size_t svgSize = svgMeta.size();
        svgMeta.close();

        if (svgSize <= 64 * 1024) {
            response.sendHeader("Cache-Control", "public, max-age=86400");
            if (response.sendFile(LittleFS, "/favicon.svg", "image/svg+xml") > 0) {
                return;
            }
        }
    }

    // Simple thermometer icon in brand color (no external assets).
    static const char* kSvg =
        "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 64 64\">"
        "<rect width=\"64\" height=\"64\" rx=\"12\" fill=\"#2196f3\"/>"
        "<rect x=\"28\" y=\"12\" width=\"8\" height=\"30\" rx=\"4\" fill=\"#ffffff\"/>"
        "<circle cx=\"32\" cy=\"46\" r=\"10\" fill=\"#ffffff\"/>"
        "<circle cx=\"32\" cy=\"46\" r=\"6\" fill=\"#2196f3\"/>"
        "</svg>";

    response.sendHeader("Cache-Control", "public, max-age=86400");
    response.send(200, "image/svg+xml; charset=UTF-8", kSvg);
}

namespace {
class HtmlStreamWriter {
public:
    explicit HtmlStreamWriter(webserver::IResponse& response, size_t reserveBytes = 4096, size_t flushThreshold = 3072)
        : response(response), flushThreshold(flushThreshold), totalBytes(0) {
        buffer.reserve(reserveBytes);
    }

    HtmlStreamWriter& operator+=(const char* s) {
        if (!s) return *this;
        buffer += s;
        totalBytes += strlen(s);
        flushIfNeeded();
        return *this;
    }

    HtmlStreamWriter& operator+=(const String& s) {
        buffer += s;
        totalBytes += s.length();
        flushIfNeeded();
        return *this;
    }

    HtmlStreamWriter& operator+=(char c) {
        buffer += c;
        totalBytes += 1;
        flushIfNeeded();
        return *this;
    }

    size_t length() const { return totalBytes; }

    void flush() {
        if (buffer.length() == 0) return;
        response.sendContent(buffer);
        buffer = "";
    }

    void finish() { flush(); }

private:
    void flushIfNeeded() {
        if (buffer.length() >= flushThreshold) {
            flush();
        }
    }

    webserver::IResponse& response;
    String buffer;
    size_t flushThreshold;
    size_t totalBytes;
};
} // namespace

void WebServerManager::streamSettingsHtml(webserver::IResponse& response) {
    const Settings settings = configManager->getSettingsSnapshot();

    HtmlStreamWriter html(response);

    html += "<html><head><title>Settings</title><style>";
    html += "body { background-color: #1e1e1e; color: #ffffff; font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif; margin: 0; padding: 60px 20px 20px 20px; }";
    html += "h1 { color: #2196f3; margin-bottom: 30px; }";
    html += "h2 { color: #2196f3; font-size: 1.4em; margin-top: 15px; margin-bottom: 10px; border-bottom: 2px solid #2196f3; padding-bottom: 5px; }";
    html += "h3 { color: #3989ca; font-size: 1.1em; margin-top: 12px; margin-bottom: 8px; }";
    html += ".settings-container { display: grid; grid-template-columns: repeat(auto-fit, minmax(250px, 1fr)); gap: 15px; margin-bottom: 30px; }";
    html += ".settings-card { background: #2d2d2d; border: 1px solid #424242; border-radius: 8px; padding: 15px; }";
    html += ".settings-card h2 { margin-top: 0; }";
    html += ".settings-card h3 { margin-top: 0; }";
    html += ".settings-card { padding-top: 12px; }";
    html += ".form-row { display: flex; flex-direction: column; margin-bottom: 10px; }";
    html += ".form-row label { font-weight: bold; margin-bottom: 5px; font-size: 0.9em; color: #b0b0b0; }";
    html += ".form-row input, .form-row select { background-color: #333333; color: #ffffff; border: 1px solid #424242; padding: 8px 12px; border-radius: 4px; font-size: 0.9em; }";
    html += ".form-row input:focus, .form-row select:focus { outline: none; border-color: #2196f3; }";
    html += ".sensor-fields { display: none; }";
    html += ".topic-fields { display: none; }";
    html += ".form-help { font-size: 0.8em; color: #888888; margin-top: 3px; }";
    html += ".button-container { text-align: center; margin-top: 20px; border-top: none; grid-column: 1 / -1; }";
    html += "button { background-color: #2196f3; color: white; border: none; padding: 12px 24px; border-radius: 4px; cursor: pointer; font-size: 1em; margin: 0 10px; }";
    html += "button:hover { background-color: #1976d2; }";
    html += "button.danger { background-color: #f44336; }";
    html += "button.danger:hover { background-color: #d32f2f; }";
    html += "@media (max-width: 1200px) { .settings-container { grid-template-columns: repeat(auto-fit, minmax(250px, 1fr)); } }";
    html += "@media (max-width: 768px) { .settings-container { grid-template-columns: 1fr; } }";
    html += "</style></head><body>" + getNavHtml("settings");

    html += "<form id='settingsForm'>";
    html += "<div class='settings-container'>";

    // Column 1: DS18B20 Settings
    html += "<div class='settings-card'>";
    html += "<h2>DS18B20 Sensors</h2>";

    html += "<div class='form-row'>";
    html += "<select name='sensors_ds18b20_enabled'>";
    html += "<option value='true'" + String(settings.sensors.ds18b20_enabled ? " selected" : "") + ">Enabled</option>";
    html += "<option value='false'" + String(!settings.sensors.ds18b20_enabled ? " selected" : "") + ">Disabled</option>";
    html += "</select>";
    html += "<div class='form-help'>Master switch</div>";
    html += "</div>";

    html += "<div class='form-row'>";
    html += "<label>Sensor Count:</label>";
    html += "<select name='sensors_sensor_count' id='sensorCount'>";
    for (int i = 1; i <= 32; i++) {
        html += "<option value='" + String(i) + "'" + (i == settings.sensors.sensor_count ? " selected" : "") + ">" + String(i) + "</option>";
    }
    html += "</select>";
    html += "</div>";

    html += "<div class='form-row'>";
    html += "<label>One Wire Bus Count:</label>";
    html += "<select name='sensors_one_wire_bus_count' id='busCount'>";
    for (int i = 1; i <= 4; i++) {
        html += "<option value='" + String(i) + "'" + (i == settings.sensors.one_wire_bus_count ? " selected" : "") + ">" + String(i) + "</option>";
    }
    html += "</select>";
    html += "</div>";

    for (int p = 0; p < 4; p++) {
        String display = (p < settings.sensors.one_wire_bus_count) ? "block" : "none";
        html += "<div class='bus-fields' id='bus" + String(p + 1) + "' style='display:" + display + "'>";
        html += "<div class='form-row'>";
        html += "<label>One Wire Bus " + String(p + 1) + " Pin:</label>";
        html += "<input type='number' name='sensors_one_wire_pins" + String(p) + "' value='" + String(settings.sensors.one_wire_pins[p]) + "'>";
        html += "</div></div>";
    }

    html += "<div class='form-row'>";
    html += "<label>Viable Scan Pins:</label>";
    html += "<input type='text' name='sensors_viable_scan_pins' value='" + settings.sensors.viable_scan_pins + "' placeholder='4,5,6,7,12,13,14,15,16,17,18'>";
    html += "<div class='form-help'>Pin list for full detection</div>";
    html += "</div>";

    for (int s = 0; s < 32; s++) {
        html += "<div class='sensor-fields' id='sensor" + String(s + 1) + "'>";
        html += "<div class='form-row'>";
        html += "<label>Sensor " + String(s + 1) + " Address:</label>";
        String hexAddr = "";
        if (s < settings.sensors.sensor_count) {
            hexAddr = settings.sensors.sensor_addresses[s];
        }
        html += "<input type='text' name='sensor" + String(s + 1) + "_addr' value='" + hexAddr + "' placeholder='28FF641F4995F2AF'>";
        html += "</div></div>";
    }

    html += "<div class='form-row'>";
    html += "<div style='height: 20px;'></div>";
    html += "<h2>Global Sensor Measurement Interval (s)</h2>";
    html += "<input type='number' name='sensors_ds18b20_measurement_interval' value='" + String(settings.sensors.ds18b20_measurement_interval, 1) + "' step='0.1' min='10'>";
    html += "<div class='form-help'>Minimum interval is 10 seconds</div>";
    html += "</div>";

    html += "</div>"; // End DS18B20 column

    // Column 2: SHT4x Settings
    html += "<div class='settings-card'>";
    html += "<h2>SHT4x Sensor</h2>";

    html += "<div class='form-row'>";
    html += "<select name='sensors_sht4x_enabled'>";
    html += "<option value='true'" + String(settings.sensors.sht4x_enabled ? " selected" : "") + ">Enabled</option>";
    html += "<option value='false'" + String(!settings.sensors.sht4x_enabled ? " selected" : "") + ">Disabled</option>";
    html += "</select>";
    html += "<div class='form-help'>Master switch</div>";
    html += "</div>";

    html += "<div class='form-row'>";
    html += "<label>SDA Pin:</label>";
    html += "<input type='number' name='sensors_sht4x_sda_pin' value='" + String(settings.sensors.sht4x_sda_pin) + "'>";
    html += "</div>";

    html += "<div class='form-row'>";
    html += "<label>SCL Pin:</label>";
    html += "<input type='number' name='sensors_sht4x_scl_pin' value='" + String(settings.sensors.sht4x_scl_pin) + "'>";
    html += "</div>";

    html += "<div class='form-row'>";
    html += "<label>I2C Address:</label>";
    html += "<input type='text' name='sensors_sht4x_i2c_address' value='" + String(settings.sensors.sht4x_i2c_address, HEX) + "' placeholder='44'>";
    html += "</div>";

    html += "<div class='form-row'>";
    html += "<label>Precision:</label>";
    html += "<select name='sensors_sht4x_precision'>";
    html += "<option value='0'" + String(settings.sensors.sht4x_precision == 0 ? " selected" : "") + ">Low</option>";
    html += "<option value='1'" + String(settings.sensors.sht4x_precision == 1 ? " selected" : "") + ">Medium</option>";
    html += "<option value='2'" + String(settings.sensors.sht4x_precision == 2 ? " selected" : "") + ">High</option>";
    html += "</select>";
    html += "</div>";

    html += "<div class='form-row'>";
    html += "<label>Heater Power:</label>";
    html += "<select name='sensors_sht4x_heater_power'>";
    html += "<option value='0'" + String(settings.sensors.sht4x_heater_power == 0 ? " selected" : "") + ">None</option>";
    html += "<option value='1'" + String(settings.sensors.sht4x_heater_power == 1 ? " selected" : "") + ">Low (20mW)</option>";
    html += "<option value='2'" + String(settings.sensors.sht4x_heater_power == 2 ? " selected" : "") + ">Medium (110mW)</option>";
    html += "<option value='3'" + String(settings.sensors.sht4x_heater_power == 3 ? " selected" : "") + ">High (200mW)</option>";
    html += "</select>";
    html += "</div>";

    html += "<div class='form-row'>";
    html += "<label>Heater Duration:</label>";
    html += "<select name='sensors_sht4x_heater_duration'>";
    html += "<option value='0'" + String(settings.sensors.sht4x_heater_duration == 0 ? " selected" : "") + ">Short (0.1s)</option>";
    html += "<option value='1'" + String(settings.sensors.sht4x_heater_duration == 1 ? " selected" : "") + ">Long (1s)</option>";
    html += "</select>";
    html += "</div>";

    html += "<div class='form-row'>";
    html += "<label>Heating Strategy:</label>";
    html += "<select name='sensors_sht4x_heating_strategy'>";
    html += "<option value='0'" + String(settings.sensors.sht4x_heating_strategy == 0 ? " selected" : "") + ">None</option>";
    html += "<option value='1'" + String(settings.sensors.sht4x_heating_strategy == 1 ? " selected" : "") + ">Alternating</option>";
    html += "</select>";
    html += "<div class='form-help'>Alternate Heating/Reading</div>";
    html += "</div>";

    html += "<div class='form-row'>";
    html += "<label>Alternating Interval (s):</label>";
    html += "<input type='number' name='sensors_sht4x_measurement_interval' value='" + String(settings.sensors.sht4x_measurement_interval, 1) + "' step='0.1' min='10'>";
    html += "<div class='form-help'>Every second interval is readout</div>";
    html += "</div>";

    html += "</div>"; // End SHT4x column

    // Column 3: MAX31855K Thermocouple
    html += "<div class='settings-card'>";
    html += "<h2>MAX31855 K-Probe</h2>";

    html += "<div class='form-row'>";
    html += "<select name='sensors_max31855k_enabled'>";
    html += "<option value='true'" + String(settings.sensors.max31855k_enabled ? " selected" : "") + ">Enabled</option>";
    html += "<option value='false'" + String(!settings.sensors.max31855k_enabled ? " selected" : "") + ">Disabled</option>";
    html += "</select>";
    html += "<div class='form-help'>Master switch</div>";
    html += "</div>";

    html += "<div class='form-row'>";
    html += "<label>CLK pin:</label>";
    html += "<input type='number' name='sensors_max31855k_clk_pin' value='" + String(settings.sensors.max31855k_clk_pin) + "' placeholder='12'>";
    html += "</div>";

    html += "<div class='form-row'>";
    html += "<label>CS pin:</label>";
    html += "<input type='number' name='sensors_max31855k_cs_pin' value='" + String(settings.sensors.max31855k_cs_pin) + "' placeholder='11'>";
    html += "</div>";

    html += "<div class='form-row'>";
    html += "<label>DO pin:</label>";
    html += "<input type='number' name='sensors_max31855k_do_pin' value='" + String(settings.sensors.max31855k_do_pin) + "' placeholder='13'>";
    html += "</div>";

    html += "</div>"; // End MAX31855K column

    // Column 4: Relay Configuration
    html += "<div class='settings-card'>";
    html += "<h2>Relay Settings</h2>";

    html += "<div class='form-row'>";
    html += "<select name='relay_enabled'>";
    html += "<option value='true'" + String(settings.relay.enabled ? " selected" : "") + ">Enabled</option>";
    html += "<option value='false'" + String(!settings.relay.enabled ? " selected" : "") + ">Disabled</option>";
    html += "</select>";
    html += "<div class='form-help'>Master switch</div>";
    html += "</div>";

    html += "<div class='form-row'>";
    html += "<label>Garage Door Mode:</label>";
    html += "<select name='relay_garage_relay_mode_enabled'>";
    html += "<option value='true'" + String(settings.relay.garage_relay_mode_enabled ? " selected" : "") + ">Enabled</option>";
    html += "<option value='false'" + String(!settings.relay.garage_relay_mode_enabled ? " selected" : "") + ">Disabled</option>";
    html += "</select>";
    html += "<div class='form-help'>Pulse relay for 500ms</div>";
    html += "</div>";    

    html += "<div class='form-row'>";
    html += "<label>LOW State Pins:</label>";
    html += "<input type='text' name='relay_pins_low' value='";
    for (size_t i = 0; i < settings.relay.pins_low.size(); i++) {
        if (i > 0) html += ",";
        html += String(settings.relay.pins_low[i]);
    }
    html += "'></div>";

    html += "<div class='form-row'>";
    html += "<label>MID State Pins:</label>";
    html += "<input type='text' name='relay_pins_mid' value='";
    for (size_t i = 0; i < settings.relay.pins_mid.size(); i++) {
        if (i > 0) html += ",";
        html += String(settings.relay.pins_mid[i]);
    }
    html += "'></div>";

    html += "<div class='form-row'>";
    html += "<label>HIGH State Pins:</label>";
    html += "<input type='text' name='relay_pins_high' value='";
    for (size_t i = 0; i < settings.relay.pins_high.size(); i++) {
        if (i > 0) html += ",";
        html += String(settings.relay.pins_high[i]);
    }
    html += "'></div>";

    html += "<div class='form-row'>";
    html += "<label>Safety Timeout (ms):</label>";
    html += "<input type='number' name='relay_timeout_ms' value='" + String(settings.relay.timeout_ms) + "' min='1000' max='30000' step='100'>";
    html += "<div class='form-help'>Relay cooldown (1000-30000ms)</div>";
    html += "</div>";

    html += "<h3>Temp Safety Shutdown</h3>";
    html += "<div class='form-help'>DS18B20 based shutdown</div>";

    html += "<div class='form-row'>";
    html += "<select name='relay_safety_enabled'>";
    html += "<option value='true'" + String(settings.relay.safety_enabled ? " selected" : "") + ">Enabled</option>";
    html += "<option value='false'" + String(!settings.relay.safety_enabled ? " selected" : "") + ">Disabled</option>";
    html += "</select>";
    html += "</div>";

    html += "<div class='form-row'>";
    html += "<label>DS18B20 Sensor Selection:</label>";
    html += "<select name='relay_safety_sensor_number'>";
    // Dynamically populate with available DS18B20 sensors
    int sensorCount = ds18b20Manager ? ds18b20Manager->getSensorCount() : 0;
    for (int i = 1; i <= sensorCount; i++) {
        html += "<option value='" + String(i) + "'" + (i == settings.relay.safety_sensor_number ? " selected" : "") + ">Sensor " + String(i) + "</option>";
    }
    if (sensorCount == 0) {
        html += "<option value='1' selected>No sensors available</option>";
    }
    html += "</select>";
    html += "</div>";

    html += "<div class='form-row'>";
    html += "<label>Trigger Temp (&deg;C):</label>";
    html += "<input type='number' name='relay_safety_temperature_threshold' value='" + String(settings.relay.safety_temperature_threshold, 1) + "' min='0' max='100' step='0.1'>";
    html += "<div class='form-help'>(0-100&deg;C)</div>";
    html += "</div>";

    html += "<div class='form-row'>";
    html += "<label>Duration (s):</label>";
    html += "<input type='number' name='relay_safety_duration_seconds' value='" + String(settings.relay.safety_duration_seconds) + "' min='0' max='1800' step='1'>";
    html += "<div class='form-help'>Temp above needed for this long (0-1800s)</div>";
    html += "</div>";

    html += "<h2>Scheduler Activator</h2>";
    html += "<div class='form-row'>";
    html += "<select name='fallback_enabled'>";
    html += "<option value='true'" + String(settings.fallback.enabled ? " selected" : "") + ">Enabled</option>";
    html += "<option value='false'" + String(!settings.fallback.enabled ? " selected" : "") + ">Disabled</option>";
    html += "</select>";
    html += "<div class='form-help'>Enable automatic scheduler activation when MQTT connection is lost</div>";
    html += "</div>";

    html += "<div class='form-row'>";
    html += "<label>Timeout (m):</label>";
    html += "<input type='number' name='fallback_timeout_minutes' value='" + String(settings.fallback.timeout_minutes) + "' min='1' max='1440'>";
    html += "<div class='form-help'>Time to wait before triggering scheduler (1-1440 minutes)</div>";
    html += "</div>";    

    html += "</div>"; // End Relay column

    // Column 4: MQTT Settings
    html += "<div class='settings-card'>";
    html += "<h2>MQTT Settings</h2>";

    html += "<div class='form-row'>";
    html += "<label>Server:</label>";
    html += "<input type='text' name='mqtt_server' value='" + settings.mqtt.server + "'>";
    html += "</div>";

    html += "<div class='form-row'>";
    html += "<label>Port:</label>";
    html += "<input type='number' name='mqtt_port' value='" + String(settings.mqtt.port) + "'>";
    html += "</div>";

    html += "<div class='form-row'>";
    html += "<label>Username:</label>";
    html += "<input type='text' name='mqtt_user' value='" + settings.mqtt.user + "'>";
    html += "</div>";

    html += "<div class='form-row'>";
    html += "<label>Password:</label>";
    html += "<input type='password' name='mqtt_password' value='' placeholder='Enter MQTT password'>";
    html += "<div class='form-help'>Leave empty to keep current password</div>";
    html += "</div>";

    html += "<div class='form-row'>";
    html += "<label>Client ID:</label>";
    html += "<input type='text' name='mqtt_client_id' value='" + settings.mqtt.client_id + "'>";
    html += "</div>";

    html += "<div class='form-row'>";
    html += "<label>Publish Interval (s):</label>";
    html += "<input type='number' name='mqtt_publish_interval' value='" + String(settings.mqtt.publish_interval) + "' min='1' max='3600'>";
    html += "</div>";

    html += "<h3>MQTT Topics</h3>";
    for (int i = 0; i < 32; i++) {
        html += "<div class='topic-fields' id='topic" + String(i + 1) + "'>";
        html += "<div class='form-row'>";
        html += "<label>DS18B20 Sensor " + String(i + 1) + ":</label>";
        html += "<input type='text' name='mqtt_topics_temp" + String(i + 1) + "' value='" + (i < settings.sensors.sensor_count ? settings.mqtt.topics.temp[i] : "temperature_sensor_" + String(i + 1)) + "'>";
        html += "</div></div>";
    }

    html += "<div class='form-row'>";
    html += "<label>SHT4x Sensor:</label>";
    html += "<input type='text' name='mqtt_topics_sht4x' value='" + settings.mqtt.topics.sht4x + "' placeholder='sht4x_sensor'>";
    html += "</div>";

    html += "<div class='form-row'>";
    html += "<label>MAX31855 K-Probe:</label>";
    html += "<input type='text' name='mqtt_topics_max31855k' value='" + settings.mqtt.topics.max31855k + "' placeholder='max31855k_sensor'>";
    html += "</div>";

    html += "<h3>Device Information</h3>";
    html += "<div class='form-row'>";
    html += "<label>Device Name:</label>";
    html += "<input type='text' name='device_info_name' value='" + settings.device_info.name + "'>";
    html += "</div>";

    html += "<div class='form-row'>";
    html += "<label>Manufacturer:</label>";
    html += "<input type='text' name='device_info_manufacturer' value='" + settings.device_info.manufacturer + "'>";
    html += "</div>";

    html += "<div class='form-row'>";
    html += "<label>Model:</label>";
    html += "<input type='text' name='device_info_model' value='" + settings.device_info.model + "'>";
    html += "</div>";

    html += "</div>"; // End WiFi/MQTT column

    // Column 4: Network Settings
    html += "<div class='settings-card'>";
    html += "<h2>Network Settings</h2>";

    html += "<h3>WiFi Configuration</h3>";
    html += "<div class='form-row'>";
    html += "<label>SSID:</label>";
    html += "<input type='text' name='wifi_ssid' value='" + settings.wifi.ssid + "'>";
    html += "</div>";

    html += "<div class='form-row'>";
    html += "<label>Password:</label>";
    html += "<input type='password' name='wifi_password' value='' placeholder='Enter WiFi password'>";
    html += "<div class='form-help'>Leave empty to keep current password</div>";
    html += "</div>";

    html += "<h3>mDNS Configuration</h3>";
    html += "<div class='form-row'>";
    html += "<select name='network_mdns_enabled'>";
    html += "<option value='true'" + String(settings.network.mdns_enabled ? " selected" : "") + ">Enabled</option>";
    html += "<option value='false'" + String(!settings.network.mdns_enabled ? " selected" : "") + ">Disabled</option>";
    html += "</select>";
    html += "<div class='form-help'>Enable .local hostname discovery</div>";
    html += "</div>";

    html += "<div class='form-row'>";
    html += "<label>Hostname:</label>";
    html += "<input type='text' name='network_hostname' value='" + settings.network.hostname + "' placeholder='esp32-sensor'>";
    html += "</div>";

    html += "<h3>Security Settings</h3>";
    html += "<div class='form-row'>";
    html += "<label>Basic Auth Enabled:</label>";
    html += "<select name='security_auth_enabled'>";
    html += "<option value='true'" + String(settings.security.auth_enabled ? " selected" : "") + ">Enabled</option>";
    html += "<option value='false'" + String(!settings.security.auth_enabled ? " selected" : "") + ">Disabled</option>";
    html += "</select>";
    html += "</div>";

    html += "<div class='form-row'>";
    html += "<label>Username:</label>";
    html += "<input type='text' name='security_username' value='" + settings.security.username + "' placeholder='admin'>";
    html += "</div>";

    html += "<div class='form-row'>";
    html += "<label>Password:</label>";
    html += "<input type='password' name='security_password_hash' value='' placeholder='Enter authentication password'>";
    html += "<div class='form-help'>Leave empty to keep current password</div>";
    html += "</div>";

    html += "<div class='form-row'>";
    html += "<label>IP Whitelist Enabled:</label>";
    html += "<select name='security_ip_whitelist_enabled'>";
    html += "<option value='true'" + String(settings.security.ip_whitelist_enabled ? " selected" : "") + ">Enabled</option>";
    html += "<option value='false'" + String(!settings.security.ip_whitelist_enabled ? " selected" : "") + ">Disabled</option>";
    html += "</select>";
    html += "</div>";

    html += "<div class='form-row'>";
    html += "<label>IP Ranges (comma-separated):</label>";
    html += "<input type='text' name='security_ip_ranges' value='";
    for (size_t i = 0; i < settings.security.ip_ranges.size(); i++) {
        if (i > 0) html += ",";
        html += settings.security.ip_ranges[i];
    }
    html += "' placeholder='192.168.1.99, 192.168.1.100-30'>";
    html += "<div class='form-help'>192.168.1.100-30 (30 IPs)</div>";
    html += "</div>";

    html += "<div class='form-row'>";
    html += "<h2>Serial Logging</h2>";
    html += "<select name='network_serial_logging_enabled'>";
    html += "<option value='true'" + String(settings.debug.logging_enabled ? " selected" : "") + ">Enabled</option>";
    html += "<option value='false'" + String(!settings.debug.logging_enabled ? " selected" : "") + ">Disabled</option>";
    html += "</select>";
    html += "<div class='form-help'>Master switch for serial output</div>";
    html += "</div>";

    html += "</div>"; // End Network column

    // Column 5: NTP Settings
    html += "<div class='settings-card'>";
    html += "<h2>NTP Settings</h2>";

    // NTP Servers (3 servers)
    for (int i = 0; i < 3; i++) {
        html += "<div class='form-row'>";
        html += "<label>NTP Server " + String(i + 1) + ":</label>";
        html += "<input type='text' name='ntp_servers" + String(i) + "' value='" + settings.ntp.servers[i] + "' placeholder='pool.ntp.org'>";
        html += "</div>";
    }

    // Timezone
    html += "<div class='form-row'>";
    html += "<label>Timezone:</label>";
    html += "<input type='text' name='ntp_timezone' value='" + settings.ntp.timezone + "' placeholder='CET-1CEST,M3.5.0,M10.5.0/3'>";
    html += "<div class='form-help'>POSIX timezone format</div>";
    html += "</div>";

    // DS3231 RTC Settings (NEW)
    html += "<h3>DS3231 RTC Support</h3>";

    html += "<div class='form-row'>";
    html += "<select name='ntp_rtc_enabled'>";
    html += "<option value='true'" + String(settings.ntp.rtc.enabled ? " selected" : "") + ">Enabled</option>";
    html += "<option value='false'" + String(!settings.ntp.rtc.enabled ? " selected" : "") + ">Disabled</option>";
    html += "</select>";
    html += "</div>";

    html += "<div class='form-row'>";
    html += "<label>SDA Pin:</label>";
    html += "<input type='number' name='ntp_rtc_sda_pin' value='" + String(settings.ntp.rtc.sda_pin) + "'>";
    html += "</div>";

    html += "<div class='form-row'>";
    html += "<label>SCL Pin:</label>";
    html += "<input type='number' name='ntp_rtc_scl_pin' value='" + String(settings.ntp.rtc.scl_pin) + "'>";
    html += "</div>";

    html += "<div class='form-row'>";
    html += "<label>I2C Address:</label>";
    html += "<input type='text' name='ntp_rtc_i2c_address' value='0x" + String(settings.ntp.rtc.i2c_address, HEX) + "' placeholder='0x68'>";
    html += "</div>";

    html += "<div class='form-row'>";
    html += "<label>RTC DateTime Format:</label>";
    html += "<select name='ntp_rtc_store_utc'>";
    html += "<option value='true'" + String(settings.ntp.rtc.store_utc ? " selected" : "") + ">UTC</option>";
    html += "<option value='false'" + String(!settings.ntp.rtc.store_utc ? " selected" : "") + ">Local time</option>";
    html += "</select>";
    html += "<div class='form-help'>Local time uses ntp.timezone</div>";
    html += "</div>";

    html += "<div class='form-row'>";
    html += "<label>Use RTC on Boot:</label>";
    html += "<select name='ntp_rtc_set_system_time_on_boot'>";
    html += "<option value='true'" + String(settings.ntp.rtc.set_system_time_on_boot ? " selected" : "") + ">Enabled</option>";
    html += "<option value='false'" + String(!settings.ntp.rtc.set_system_time_on_boot ? " selected" : "") + ">Disabled</option>";
    html += "</select>";
    html += "<div class='form-help'>Set system time on boot from RTC</div>";
    html += "</div>";

    html += "<div class='form-row'>";
    html += "<label>Sync RTC from NTP:</label>";
    html += "<select name='ntp_rtc_sync_rtc_from_ntp'>";
    html += "<option value='true'" + String(settings.ntp.rtc.sync_rtc_from_ntp ? " selected" : "") + ">Enabled</option>";
    html += "<option value='false'" + String(!settings.ntp.rtc.sync_rtc_from_ntp ? " selected" : "") + ">Disabled</option>";
    html += "</select>";
    html += "</div>";

    // NOTE:
    // - RTC calibration interval is intentionally not configurable (fixed at 60 minutes to match NTP cadence).
    // - RTC max step remains configurable via config.json only (not exposed on the web UI).

    html += "</div>"; // End NTP column

    html += "</div>"; // End settings-container

    html += "<div class='button-container' style='grid-column: 1 / -1;'>";
    html += "<button type='button' id='saveBtn'>Save Settings</button>";
    html += "<button type='button' class='danger' id='rebootBtn'>Reboot ESP32</button>";
    html += "</div>";

    // Relay Scheduling Section (Below Main Grid)
    html += "<div style='margin-top: 30px;'>";
    html += "<div class='settings-card' style='max-width: none;'>";
    html += "<h2>Relay Scheduler</h2>";

    html += "<div class='form-row'>";
    html += "<select name='schedules_enabled'>";
    html += "<option value='true'" + String(settings.schedules.enabled ? " selected" : "") + ">Enabled</option>";
    html += "<option value='false'" + String(!settings.schedules.enabled ? " selected" : "") + ">Disabled</option>";
    html += "</select>";
    html += "</div>";

    // Days of the week
    const char* dayNames[] = {"monday", "tuesday", "wednesday", "thursday", "friday", "saturday", "sunday"};
    const char* dayDisplayNames[] = {"Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday"};

    for (int day = 0; day < 7; day++) {
        html += "<div class='day-section' style='margin-bottom: 20px; border: 1px solid #424242; padding: 10px; border-radius: 4px;'>";
        html += "<div style='display: flex; justify-content: space-between; align-items: center; margin-bottom: 10px;'>";
        html += "<span style='color: #2196f3; font-weight: bold; font-size: 1.2em;'>" + String(dayDisplayNames[day]) + "</span>";
        html += "<button type='button' class='add-range-btn' data-day='" + String(dayNames[day]) + "' style='background: #2196f3; color: white; border: none; padding: 5px 10px; border-radius: 4px; cursor: pointer;'>+ Add Range</button>";
        html += "</div>";
        html += "<div class='ranges-container' id='" + String(dayNames[day]) + "-ranges'>";

        // Find schedules for this day
        bool hasSchedules = false;
        for (const auto& entry : settings.schedules.entries) {
            if (entry.day == day) {
                hasSchedules = true;
                break;
            }
        }

        if (!hasSchedules) {
            html += "<div class='no-schedules' style='color: #888; font-style: italic; text-align: center; padding: 10px;'>No schedules configured for " + String(dayDisplayNames[day]) + "</div>";
        } else {
            // Display each schedule entry as a range
            int rangeIndex = 0;
            for (const auto& entry : settings.schedules.entries) {
                if (entry.day == day) {
                    // Kept container compact (gap 5px, padding 4px)
                    html += "<div class='time-range' style='display: inline-flex; align-items: center; margin-right: 10px; margin-bottom: 5px; gap: 5px; background: #2d2d2d; padding: 4px; border-radius: 4px;'>";
                    
                    // Font size restored to 16px, padding adjusted back to 3px 5px
                    html += "<input type='time' class='time-input' name='schedule_" + String(dayNames[day]) + "_" + String(rangeIndex) + "_start_time' value='" + entry.start_time + "' style='background: #333; color: #fff; border: 1px solid #555; padding: 3px 5px; border-radius: 4px; width: 90px; font-size: 16px;'>";
                    
                    // Font size restored to 16px, padding adjusted back to 3px 5px
                    html += "<select class='level-select' name='schedule_" + String(dayNames[day]) + "_" + String(rangeIndex) + "_start_level' style='background: #333; color: #fff; border: 1px solid #555; padding: 3px 5px; border-radius: 4px; font-size: 16px; width: 75px;'>";
                    html += "<option value='off'" + String(entry.start_level == "off" ? " selected" : "") + ">OFF</option>";
                    html += "<option value='low'" + String(entry.start_level == "low" ? " selected" : "") + ">LOW</option>";
                    html += "<option value='mid'" + String(entry.start_level == "mid" ? " selected" : "") + ">MID</option>";
                    html += "<option value='high'" + String(entry.start_level == "high" ? " selected" : "") + ">HIGH</option>";
                    html += "</select>";
                    
                    html += "<span class='range-separator' style='color: #888; font-weight: bold;'>&rarr;</span>";
                    
                    // Font size restored to 16px, padding adjusted back to 3px 5px
                    html += "<input type='time' class='time-input' name='schedule_" + String(dayNames[day]) + "_" + String(rangeIndex) + "_end_time' value='" + entry.end_time + "' style='background: #333; color: #fff; border: 1px solid #555; padding: 3px 5px; border-radius: 4px; width: 90px; font-size: 16px;'>";
                    
                    // *** Font size restored to 16px, 'width' removed to fit content exactly ***
                    html += "<span style='background: #333; color: #fff; border: 1px solid #555; padding: 3px 5px; border-radius: 4px; font-size: 16px; display: inline-block; text-align: center;'>OFF</span>";
                    html += "<input type='hidden' name='schedule_" + String(dayNames[day]) + "_" + String(rangeIndex) + "_end_level' value='off'>";
                    
                    // Button padding adjusted
                    html += "<button type='button' class='remove-range-btn' style='background: #f44336; color: white; border: none; padding: 5px 10px; border-radius: 4px; cursor: pointer;'>-</button>";
                    html += "</div>";
                    rangeIndex++;
                }
            }
        }



        html += "</div>";
        html += "</div>";
    }

    html += "</div>";
    html += "</div>";
    html += "</form>";

    html += "<script>\n";
    html += "function updateSensorFields() {\n";
    html += "    const count = parseInt(document.getElementById('sensorCount').value);\n";
    html += "    for (let i = 1; i <= 32; i++) {\n";
    html += "        const display = i <= count ? 'block' : 'none';\n";
    html += "        document.getElementById('sensor' + i).style.display = display;\n";
    html += "        const topicDiv = document.getElementById('topic' + i);\n";
    html += "        topicDiv.style.display = display;\n";
    html += "        if (display === 'block') {\n";
    html += "            const topicInput = topicDiv.querySelector('input');\n";
    html += "            if (!topicInput.value) {\n";
    html += "                topicInput.value = 'temperature_sensor_' + i;\n";
    html += "            }\n";
    html += "        }\n";
    html += "    }\n";
    html += "}\n";
    html += "function updateBusFields() {\n";
    html += "    const count = parseInt(document.getElementById('busCount').value);\n";
    html += "    for (let i = 1; i <= 4; i++) {\n";
    html += "        const display = i <= count ? 'block' : 'none';\n";
    html += "        document.getElementById('bus' + i).style.display = display;\n";
    html += "    }\n";
    html += "}\n";
    html += "document.getElementById('sensorCount').addEventListener('change', updateSensorFields);\n";
    html += "document.getElementById('busCount').addEventListener('change', updateBusFields);\n";
    html += "updateSensorFields();\n";  // Initial call
    html += "updateBusFields();\n";  // Initial call
    html += "document.getElementById('saveBtn').addEventListener('click', function() {\n";
    html += "    const formData = new FormData(document.getElementById('settingsForm'));\n";
    html += "    \n";
    html += "    // Client-side validation for safety parameters\n";
    html += "    const safetyEnabled = formData.get('relay_safety_enabled') === 'true';\n";
    html += "    if (safetyEnabled) {\n";
    html += "        const sensorNumber = parseInt(formData.get('relay_safety_sensor_number'));\n";
    html += "        const tempThreshold = parseFloat(formData.get('relay_safety_temperature_threshold'));\n";
    html += "        const durationSeconds = parseInt(formData.get('relay_safety_duration_seconds'));\n";
    html += "        \n";
    html += "        if (isNaN(sensorNumber) || sensorNumber < 1) {\n";
    html += "            alert('Safety sensor number must be a positive integer');\n";
    html += "            return;\n";
    html += "        }\n";
    html += "        \n";
    html += "        if (isNaN(tempThreshold) || tempThreshold < 0 || tempThreshold > 100) {\n";
    html += "            alert('Temperature threshold must be between 0 and 100&deg;C');\n";
    html += "            return;\n";
    html += "        }\n";
    html += "        \n";
    html += "        if (isNaN(durationSeconds) || durationSeconds < 0 || durationSeconds > 1800) {\n";
    html += "            alert('Duration must be between 0 and 1800 seconds');\n";
    html += "            return;\n";
    html += "        }\n";
    html += "    }\n";
    html += "    \n";
    html += "    console.log('DEBUG: Building settings data object');\n";
    html += "    const data = {\n";
    html += "        wifi: { ssid: formData.get('wifi_ssid'), password: formData.get('wifi_password') },\n";
    html += "    mqtt: {\n";
    html += "        server: formData.get('mqtt_server'),\n";
    html += "        port: parseInt(formData.get('mqtt_port')),\n";
    html += "        user: formData.get('mqtt_user'),\n";
    html += "        password: formData.get('mqtt_password'),\n";
    html += "        client_id: formData.get('mqtt_client_id'),\n";
    html += "        publish_interval: parseInt(formData.get('mqtt_publish_interval')),\n";
    html += "        topics: {\n";
    html += "            temp: [],\n";
    html += "        }\n";
    html += "    },\n";
    html += "        sensors: {\n";
    html += "            ds18b20_enabled: formData.get('sensors_ds18b20_enabled') === 'true',\n";
    html += "            sensor_count: parseInt(formData.get('sensors_sensor_count')),\n";
    html += "            one_wire_bus_count: parseInt(formData.get('sensors_one_wire_bus_count')),\n";
    html += "            one_wire_pins: [],\n";
    html += "            sensor_addresses: {},\n";
    html += "            viable_scan_pins: formData.get('sensors_viable_scan_pins'),\n";
    html += "            sht4x_enabled: formData.get('sensors_sht4x_enabled') === 'true',\n";
    html += "            sht4x_sda_pin: parseInt(formData.get('sensors_sht4x_sda_pin')),\n";
    html += "            sht4x_scl_pin: parseInt(formData.get('sensors_sht4x_scl_pin')),\n";
    html += "            sht4x_i2c_address: formData.get('sensors_sht4x_i2c_address'),\n";
    html += "            sht4x_precision: parseInt(formData.get('sensors_sht4x_precision')),\n";
    html += "            sht4x_heater_power: parseInt(formData.get('sensors_sht4x_heater_power')),\n";
    html += "            sht4x_heater_duration: parseInt(formData.get('sensors_sht4x_heater_duration')),\n";
    html += "            sht4x_heating_strategy: parseInt(formData.get('sensors_sht4x_heating_strategy')),\n";
    html += "            sht4x_measurement_interval: parseFloat(formData.get('sensors_sht4x_measurement_interval')),";
    html += "            max31855k_enabled: formData.get('sensors_max31855k_enabled') === 'true',\n";
    html += "            max31855k_do_pin: parseInt(formData.get('sensors_max31855k_do_pin')),\n";
    html += "            max31855k_clk_pin: parseInt(formData.get('sensors_max31855k_clk_pin')),\n";
    html += "            max31855k_cs_pin: parseInt(formData.get('sensors_max31855k_cs_pin')),\n";
    html += "            ds18b20_measurement_interval: parseFloat(formData.get('sensors_ds18b20_measurement_interval'))\n";
    html += "        },\n";
    html += "        relay: {\n";
    html += "            enabled: formData.get('relay_enabled') === 'true',\n";
    html += "            pins_low: formData.get('relay_pins_low').split(',').map(pin => parseInt(pin.trim())).filter(pin => !isNaN(pin)),\n";
    html += "            pins_mid: formData.get('relay_pins_mid').split(',').map(pin => parseInt(pin.trim())).filter(pin => !isNaN(pin)),\n";
    html += "            pins_high: formData.get('relay_pins_high').split(',').map(pin => parseInt(pin.trim())).filter(pin => !isNaN(pin)),\n";
    html += "            timeout_ms: parseInt(formData.get('relay_timeout_ms')),\n";
    html += "            safety_enabled: formData.get('relay_safety_enabled') === 'true',\n";
    html += "            safety_sensor_number: parseInt(formData.get('relay_safety_sensor_number')),\n";
    html += "            safety_temperature_threshold: parseFloat(formData.get('relay_safety_temperature_threshold')),\n";
    html += "            safety_duration_seconds: parseInt(formData.get('relay_safety_duration_seconds')),\n";
    html += "            garage_relay_mode_enabled: formData.get('relay_garage_relay_mode_enabled') === 'true'\n";
    html += "        },\n";
    html += "        device_info: {\n";
    html += "            name: formData.get('device_info_name'),\n";
    html += "            manufacturer: formData.get('device_info_manufacturer'),\n";
    html += "            model: formData.get('device_info_model'),\n";
    html += "        },\n";
    html += "        network: {\n";
    html += "            hostname: formData.get('network_hostname'),\n";
    html += "            mdns_enabled: formData.get('network_mdns_enabled') === 'true',\n";
    html += "            serial_logging_enabled: formData.get('network_serial_logging_enabled') === 'true'\n";
    html += "        },\n";
    html += "        security: {\n";
    html += "            auth_enabled: formData.get('security_auth_enabled') === 'true',\n";
    html += "            username: formData.get('security_username'),\n";
    html += "            password_hash: formData.get('security_password_hash'),\n";
    html += "            ip_whitelist_enabled: formData.get('security_ip_whitelist_enabled') === 'true',\n";
    html += "            ip_ranges: []\n";
    html += "        },\n";
    html += "        ntp: {\n";
    html += "            servers: [],\n";
    html += "            timezone: formData.get('ntp_timezone'),\n";
    html += "            rtc: {\n";
    html += "                enabled: formData.get('ntp_rtc_enabled') === 'true',\n";
    html += "                sda_pin: parseInt(formData.get('ntp_rtc_sda_pin')),\n";
    html += "                scl_pin: parseInt(formData.get('ntp_rtc_scl_pin')),\n";
    html += "                i2c_address: formData.get('ntp_rtc_i2c_address'),\n";
    html += "                store_utc: formData.get('ntp_rtc_store_utc') === 'true',\n";
    html += "                set_system_time_on_boot: formData.get('ntp_rtc_set_system_time_on_boot') === 'true',\n";
    html += "                sync_rtc_from_ntp: formData.get('ntp_rtc_sync_rtc_from_ntp') === 'true'\n";
    html += "            }\n";
    html += "        },\n";
    html += "        schedules: {\n";
    html += "            enabled: formData.get('schedules_enabled') === 'true',\n";
    html += "            entries: []\n";
    html += "        },\n";
    html += "        fallback: {\n";
    html += "            enabled: formData.get('fallback_enabled') === 'true',\n";
    html += "            timeout_minutes: parseInt(formData.get('fallback_timeout_minutes'))\n";
    html += "        }\n";
    html += "    };\n";
    html += "    const count = parseInt(formData.get('sensors_sensor_count'));\n";
    html += "    const busCount = parseInt(formData.get('sensors_one_wire_bus_count'));\n";
    html += "    for (let i = 0; i < busCount; i++) {\n";
    html += "        data.sensors.one_wire_pins.push(parseInt(formData.get('sensors_one_wire_pins' + i)));\n";
    html += "    }\n";
    html += "    for (let i = 0; i < count; i++) {\n";
    html += "        data.mqtt.topics['temp' + (i + 1)] = formData.get('mqtt_topics_temp' + (i + 1));\n";
    html += "        let hexAddr = formData.get('sensor' + (i + 1) + '_addr');\n";
    html += "        data.sensors.sensor_addresses['sensor' + (i + 1) + '_address'] = hexAddr;\n";
    html += "    }\n";
    html += "    data.mqtt.topics.sht4x = formData.get('mqtt_topics_sht4x');\n";
    html += "    data.mqtt.topics.max31855k = formData.get('mqtt_topics_max31855k');\n";
    html += "    // Parse NTP servers\n";
    html += "    for (let i = 0; i < 3; i++) {\n";
    html += "        data.ntp.servers.push(formData.get('ntp_servers' + i) || '');\n";
    html += "    }\n";
    html += "    // Parse IP ranges\n";
    html += "    const ipRangesStr = formData.get('security_ip_ranges');\n";
    html += "    if (ipRangesStr) {\n";
    html += "        const ranges = ipRangesStr.split(',').map(range => range.trim()).filter(range => range.length > 0);\n";
    html += "        data.security.ip_ranges = ranges;\n";
    html += "    }\n";

    // Parse schedule entries
    html += "    // Parse schedule entries\n";
    html += "    const dayNames = ['monday', 'tuesday', 'wednesday', 'thursday', 'friday', 'saturday', 'sunday'];\n";
    html += "    dayNames.forEach((dayName, dayIndex) => {\n";
    html += "        let rangeIndex = 0;\n";
    html += "        while (true) {\n";
    html += "            const startTimeKey = `schedule_${dayName}_${rangeIndex}_start_time`;\n";
    html += "            const startLevelKey = `schedule_${dayName}_${rangeIndex}_start_level`;\n";
    html += "            const endTimeKey = `schedule_${dayName}_${rangeIndex}_end_time`;\n";
    html += "            \n";
    html += "            const startTime = formData.get(startTimeKey);\n";
    html += "            const startLevel = formData.get(startLevelKey);\n";
    html += "            const endTime = formData.get(endTimeKey);\n";
    html += "            \n";
    html += "            if (!startTime || !startLevel || !endTime) {\n";
    html += "                break; // No more ranges for this day\n";
    html += "            }\n";
    html += "            \n";
    html += "            // Add range entry (end_level always 'off' by design)\n";
    html += "            data.schedules.entries.push({\n";
    html += "                day: dayIndex,\n";
    html += "                start_time: startTime,\n";
    html += "                start_level: startLevel,\n";
    html += "                end_time: endTime\n";
    html += "            });\n";
    html += "            \n";
    html += "            rangeIndex++;\n";
    html += "        }\n";
    html += "    });\n";
    html += "    console.log('DEBUG: Settings data object built:', data);\n";
    html += "    console.log('DEBUG: JSON payload:', JSON.stringify(data));\n";
    html += "    fetch('/api/settings', {\n";
    html += "        method: 'POST',\n";
    html += "        headers: {'Content-Type': 'application/json'},\n";
    html += "        body: JSON.stringify(data)\n";
    html += "    }).then(response => {\n";
    html += "        console.log('Response status:', response.status);\n";
    html += "        console.log('Response ok:', response.ok);\n";
    html += "        if (!response.ok) {\n";
    html += "            // Try to parse error response even when HTTP status is not OK\n";
    html += "            return response.text().then(text => {\n";
    html += "                console.log('Error response text:', text);\n";
    html += "                try {\n";
    html += "                    const errorData = JSON.parse(text);\n";
    html += "                    console.log('Parsed error data:', errorData);\n";
    html += "                    // Return a rejected promise with the error message\n";
    html += "                    return Promise.reject(errorData.error || 'HTTP ' + response.status + ': ' + response.statusText);\n";
    html += "                } catch (parseError) {\n";
    html += "                    console.log('Parse error:', parseError);\n";
    html += "                    return Promise.reject('HTTP ' + response.status + ': ' + response.statusText);\n";
    html += "                }\n";
    html += "            });\n";
    html += "        }\n";
    html += "        return response.json();\n";
    html += "    }).then(result => alert(result.status || result.error)).catch(error => alert('Error: ' + error));\n";
    html += "});\n";
    html += "document.getElementById('rebootBtn').addEventListener('click', function() {\n";
    html += "    if (confirm('Are you sure you want to reboot the ESP32? This will temporarily disconnect from Home Assistant.')) {\n";
    html += "        fetch('/api/reboot', { method: 'POST' })\n";
    html += "        .then(response => response.json())\n";
    html += "        .then(result => alert(result.status || result.error))\n";
    html += "        .catch(error => alert('Error: ' + error));\n";
    html += "    }\n";
    html += "});\n";

    // Relay Scheduling JavaScript
    html += "// Relay Scheduling JavaScript\n";
    html += "document.querySelectorAll('.add-range-btn').forEach(btn => {\n";
    html += "    btn.addEventListener('click', function() {\n";
    html += "        const day = this.getAttribute('data-day');\n";
    html += "        const container = document.getElementById(day + '-ranges');\n";
    html += "        const noSchedules = container.querySelector('.no-schedules');\n";
    html += "\n";
    html += "        if (noSchedules) {\n";
    html += "            noSchedules.remove();\n";
    html += "        }\n";
    html += "\n";
    html += "        const rangeIndex = container.querySelectorAll('.time-range').length;\n";
    html += "        const timeRange = document.createElement('div');\n";
    html += "        timeRange.className = 'time-range';\n";
    html += "        timeRange.style = 'display: inline-flex; align-items: center; margin-right: 15px; margin-bottom: 8px; gap: 8px; background: #2d2d2d; padding: 6px; border-radius: 4px;';\n";
    html += "\n";
    html += "        timeRange.innerHTML = `\n";
    html += "            <input type=\"time\" class=\"time-input\" name=\"schedule_${day}_${rangeIndex}_start_time\" value=\"09:00\" style=\"background: #333; color: #fff; border: 1px solid #555; padding: 3px 5px; border-radius: 4px; width: 90px; font-size: 16px;\">\n";
    html += "            <select class=\"level-select\" name=\"schedule_${day}_${rangeIndex}_start_level\" style=\"background: #333; color: #fff; border: 1px solid #555; padding: 3px 5px; border-radius: 4px; font-size: 16px; width: 75px;\">\n";
    html += "                <option value=\"off\">OFF</option>\n";
    html += "                <option value=\"low\" selected>LOW</option>\n";
    html += "                <option value=\"mid\">MID</option>\n";
    html += "                <option value=\"high\">HIGH</option>\n";
    html += "            </select>\n";
    html += "            <span class=\"range-separator\" style=\"color: #888; font-weight: bold;\">&rarr;</span>\n";
    html += "            <input type=\"time\" class=\"time-input\" name=\"schedule_${day}_${rangeIndex}_end_time\" value=\"17:00\" style=\"background: #333; color: #fff; border: 1px solid #555; padding: 3px 5px; border-radius: 4px; width: 90px; font-size: 16px;\">\n";
    html += "            <span style=\"background: #333; color: #fff; border: 1px solid #555; padding: 3px 5px; border-radius: 4px; font-size: 16px; width: 75px; display: inline-block; text-align: center;\">OFF</span>\n";
    html += "            <input type=\"hidden\" name=\"schedule_${day}_${rangeIndex}_end_level\" value=\"off\">\n";
    html += "            <button type=\"button\" class=\"remove-range-btn\" style=\"background: #f44336; color: white; border: none; padding: 5px 10px; border-radius: 4px; cursor: pointer;\">-</button>\n";
    html += "        `;\n";
    html += "\n";
    html += "        container.appendChild(timeRange);\n";
    html += "\n";
    html += "        // Add remove functionality to the new button\n";
    html += "        timeRange.querySelector('.remove-range-btn').addEventListener('click', function() {\n";
    html += "            timeRange.remove();\n";
    html += "            // Check if no ranges left\n";
    html += "            const ranges = container.querySelectorAll('.time-range');\n";
    html += "            if (ranges.length === 0) {\n";
    html += "                const noSchedules = document.createElement('div');\n";
    html += "                noSchedules.className = 'no-schedules';\n";
    html += "                noSchedules.style = 'color: #888; font-style: italic; text-align: center; padding: 10px;';\n";
    html += "                const dayName = day.charAt(0).toUpperCase() + day.slice(1);\n";
    html += "                noSchedules.textContent = `No schedules configured for ${dayName}`;\n";
    html += "                container.appendChild(noSchedules);\n";
    html += "            }\n";
    html += "        });\n";
    html += "    });\n";
    html += "});\n";

    // Add remove functionality to existing ranges
    html += "// Add remove functionality to existing ranges\n";
    html += "document.querySelectorAll('.remove-range-btn').forEach(btn => {\n";
    html += "    btn.addEventListener('click', function() {\n";
    html += "        const timeRange = this.closest('.time-range');\n";
    html += "        const container = timeRange.closest('.ranges-container');\n";
    html += "        const daySection = container.closest('.day-section');\n";
    html += "        const dayName = daySection.querySelector('span').textContent.toLowerCase();\n";
    html += "        timeRange.remove();\n";
    html += "\n";
    html += "        // Check if no ranges left\n";
    html += "        const ranges = container.querySelectorAll('.time-range');\n";
    html += "        if (ranges.length === 0) {\n";
    html += "            const noSchedules = document.createElement('div');\n";
    html += "            noSchedules.className = 'no-schedules';\n";
    html += "            noSchedules.style = 'color: #888; font-style: italic; text-align: center; padding: 10px;';\n";
    html += "            noSchedules.textContent = `No schedules configured for ${dayName}`;\n";
    html += "            container.appendChild(noSchedules);\n";
    html += "        }\n";
    html += "    });\n";
    html += "});\n";
    html += "</script></body></html>";

    html.finish();
}

void WebServerManager::handleDetection(webserver::IRequest& request, webserver::IResponse& response) {
    if (!checkAuthentication(request, response)) {
        return; // Authentication failed, response already sent
    }

    if (!isWiFiConnected()) {
        response.send(503, kContentTypeHtmlUtf8, "<html><body><h1>Service Unavailable</h1><p>WiFi connection lost. Please check network connectivity.</p></body></html>");
        return;
    }

    response.setContentLength(webserver::kContentLengthUnknown);
    response.send(200, kContentTypeHtmlUtf8, "");
    streamDetectionHtml(response);
    response.sendContent("");
}

void WebServerManager::handleMonitoring(webserver::IRequest& request, webserver::IResponse& response) {
    const webserver::RequestMethod requestMethod = request.method();

    if (!checkAuthentication(request, response)) {
        return; // Authentication failed, response already sent
    }

    if (!isWiFiConnected()) {
        response.send(503, kContentTypeHtmlUtf8, "<html><body><h1>Service Unavailable</h1><p>WiFi connection lost. Please check network connectivity.</p></body></html>");
        return;
    }

    if (!configManager) {
        response.send(500, kContentTypeHtmlUtf8, "Configuration manager not set");
        return;
    }

    // Check for POST request with reset_stats action
    if (requestMethod == webserver::RequestMethod::Post && request.hasArg("action") && request.arg("action") == "reset_stats") {
        if (!mainController) {
        } else {
            MainController::ControlCommand command;
            command.type = MainController::ControlCommandType::MONITORING_RESET_STATS;
            const webserver::ControlDispatchWaitResult dispatchResult =
                webserver::dispatchControlCommandAndWait(mainController, command);

            if (dispatchResult.status == webserver::ControlDispatchWaitStatus::Completed &&
                dispatchResult.commandResult.status == MainController::ControlCommandStatus::SUCCEEDED) {
            } else {
            }
        }

        // Send 303 redirect to /monitoring
        response.sendHeader("Location", "/monitoring");
        response.send(303);
        return;
    }

    // Check for POST request with reset_queue_diag action
    if (requestMethod == webserver::RequestMethod::Post && request.hasArg("action") && request.arg("action") == "reset_queue_diag") {
        if (!mainController) {
        } else {
            MainController::ControlCommand command;
            command.type = MainController::ControlCommandType::MONITORING_RESET_QUEUE_DIAGNOSTICS;
            const webserver::ControlDispatchWaitResult dispatchResult =
                webserver::dispatchControlCommandAndWait(mainController, command);

            if (dispatchResult.status == webserver::ControlDispatchWaitStatus::Completed &&
                dispatchResult.commandResult.status == MainController::ControlCommandStatus::SUCCEEDED) {
            } else {
            }
        }
        resetRelayStatusApiDiagnostics();

        response.sendHeader("Location", "/monitoring");
        response.send(303);
        return;
    }

    // Check for POST request with clear_relay_logs action
    if (requestMethod == webserver::RequestMethod::Post && request.hasArg("action") && request.arg("action") == "clear_relay_logs") {
        if (!mainController) {
        } else {
            MainController::ControlCommand command;
            command.type = MainController::ControlCommandType::RELAY_CLEAR_LOGS;
            const webserver::ControlDispatchWaitResult dispatchResult =
                webserver::dispatchControlCommandAndWait(mainController, command);

            if (dispatchResult.status == webserver::ControlDispatchWaitStatus::Completed &&
                dispatchResult.commandResult.status == MainController::ControlCommandStatus::SUCCEEDED) {
            } else {
            }
        }
        // Send 303 redirect back to monitoring page
        response.sendHeader("Location", "/monitoring");
        response.send(303);
        return;
    }

    if (requestMethod == webserver::RequestMethod::Post && request.hasArg("action") && request.arg("action") == "clear_error_logs") {
        if (!mainController) {
        } else {
            MainController::ControlCommand command;
            command.type = MainController::ControlCommandType::ERROR_CLEAR_LOGS;
            const webserver::ControlDispatchWaitResult dispatchResult =
                webserver::dispatchControlCommandAndWait(mainController, command);

            if (dispatchResult.status == webserver::ControlDispatchWaitStatus::Completed &&
                dispatchResult.commandResult.status == MainController::ControlCommandStatus::SUCCEEDED) {
            } else {
            }
        }
        // Send 303 redirect back to monitoring page
        response.sendHeader("Location", "/monitoring");
        response.send(303);
        return;
    }

    response.setContentLength(webserver::kContentLengthUnknown);
    response.send(200, kContentTypeHtmlUtf8, "");
    streamMonitoringHtml(response);
    response.sendContent("");
}

void WebServerManager::handlePerformance(webserver::IRequest& request, webserver::IResponse& response) {
    const webserver::RequestMethod requestMethod = request.method();

    if (!checkAuthentication(request, response)) {
        return; // Authentication failed, response already sent
    }

    if (!isWiFiConnected()) {
        response.send(503, kContentTypeHtmlUtf8, "<html><body><h1>Service Unavailable</h1><p>WiFi connection lost. Please check network connectivity.</p></body></html>");
        return;
    }

    if (!configManager) {
        response.send(500, kContentTypeHtmlUtf8, "Configuration manager not set");
        return;
    }

    if (requestMethod == webserver::RequestMethod::Post && request.hasArg("action") && request.arg("action") == "reset_stats") {
        if (mainController) {
            MainController::ControlCommand command;
            command.type = MainController::ControlCommandType::MONITORING_RESET_STATS;
            (void)webserver::dispatchControlCommandAndWait(mainController, command);
        }
        response.sendHeader("Location", "/performance");
        response.send(303);
        return;
    }

    if (requestMethod == webserver::RequestMethod::Post && request.hasArg("action") && request.arg("action") == "reset_queue_diag") {
        if (mainController) {
            MainController::ControlCommand command;
            command.type = MainController::ControlCommandType::MONITORING_RESET_QUEUE_DIAGNOSTICS;
            (void)webserver::dispatchControlCommandAndWait(mainController, command);
        }
        resetRelayStatusApiDiagnostics();
        response.sendHeader("Location", "/performance");
        response.send(303);
        return;
    }

    response.setContentLength(webserver::kContentLengthUnknown);
    response.send(200, kContentTypeHtmlUtf8, "");
    streamPerformanceHtml(response);
    response.sendContent("");
}

void WebServerManager::handleApiPerformanceCards(webserver::IRequest& request, webserver::IResponse& response) {
    if (!checkAuthentication(request, response)) {
        return;
    }

    if (!isWiFiConnected()) {
        response.send(503, kContentTypeHtmlUtf8, "<div class='info-card performance-card'><h3>Performance Monitoring</h3><p>Service unavailable: WiFi disconnected.</p></div>");
        return;
    }

    response.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate");
    response.send(200, kContentTypeHtmlUtf8, generatePerformanceStatsHtml());
}

void WebServerManager::handleApiMonitoringCards(webserver::IRequest& request, webserver::IResponse& response) {
    if (!checkAuthentication(request, response)) {
        return;
    }

    if (!isWiFiConnected()) {
        response.send(503, kContentTypeHtmlUtf8, "<h2>System Status Overview</h2><div class='error-list'><div class='error-item'>Service unavailable: WiFi disconnected.</div></div>");
        return;
    }

    if (!configManager) {
        response.send(500, kContentTypeHtmlUtf8, "<h2>System Status Overview</h2><div class='error-list'><div class='error-item'>Configuration manager not set.</div></div>");
        return;
    }

    const Settings settings = configManager->getSettingsSnapshot();
    response.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate");
    response.send(200, kContentTypeHtmlUtf8, generateMonitoringCardsHtml(settings));
}

void WebServerManager::streamDetectionHtml(webserver::IResponse& response) {
    HtmlStreamWriter html(response);
    html += "<html><head><title>Detection</title><style>body { background-color: #1e1e1e; color: #ffffff; font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif; margin: 0; padding: 60px 20px 20px 20px; } h1 { color: #2196f3; } ul { color: #b0b0b0; } button { background-color: #2196f3; color: white; border: none; padding: 10px 20px; margin: 5px; border-radius: 4px; cursor: pointer; } button:hover { background-color: #1976d2; } button:disabled { opacity: 0.6; cursor: not-allowed; } #scanResults { margin-top: 20px; }</style></head><body>";
    html += getNavHtml("detection");
    html += "<h1>Sensor Detection</h1>";
    html += "<p>Click a button to scan for sensors:</p>";
    html += "<button id='scanConfigured'>Scan Configured Pins</button>";
    html += "<button id='scanAll'>Scan All Viable Pins</button>";
    html += "<div id='scanResults'></div>";
    html += "<script>";
    html += "const pollIntervalMs = 500;";
    html += "let scanInFlight = false;";
    html += "let scanTimer = null;";
    html += "const scanResultsEl = document.getElementById('scanResults');";
    html += "const scanConfiguredBtn = document.getElementById('scanConfigured');";
    html += "const scanAllBtn = document.getElementById('scanAll');";
    html += "function setButtonsEnabled(enabled){";
    html += "    scanConfiguredBtn.disabled = !enabled;";
    html += "    scanAllBtn.disabled = !enabled;";
    html += "}";
    html += "function renderStatus(text){";
    html += "    scanResultsEl.innerHTML = '<h2>Scan Results</h2><p>' + text + '</p>';";
    html += "}";
    html += "function renderError(message){";
    html += "    scanResultsEl.innerHTML = '<h2>Scan Results</h2><p>Error: ' + message + '</p>';";
    html += "}";
    html += "function finishScan(){";
    html += "    scanInFlight = false;";
    html += "    setButtonsEnabled(true);";
    html += "    if (scanTimer) {";
    html += "        clearTimeout(scanTimer);";
    html += "        scanTimer = null;";
    html += "    }";
    html += "}";
    html += "function displayResults(data){";
    html += "    let html = '<h2>Scan Results</h2>';";
    html += "    if (data.error) {";
    html += "        html += '<p>Error: ' + data.error + '</p>';";
    html += "    } else {";
    html += "        const sensorCount = Number(data.sensor_count || 0);";
    html += "        html += '<p>Sensor Count: ' + sensorCount + '</p>';";
    html += "        html += '<h3>Sensors:</h3><ul>';";
    html += "        for (let i = 0; i < sensorCount; i++) {";
    html += "            const addr = (data.addresses && data.addresses[i]) ? data.addresses[i] : 'N/A';";
    html += "            const temp = (data.temperatures && i < data.temperatures.length) ? Number(data.temperatures[i]) : NaN;";
    html += "            const pin = (data.pins && i < data.pins.length) ? data.pins[i] : 'N/A';";
    html += "            const tempStr = (temp === -127.0 || Number.isNaN(temp)) ? 'NA' : temp.toFixed(2) + '&deg;C';";
    html += "            html += '<li>Sensor ' + (i + 1) + ' on pin ' + pin + ': ' + addr + ' - ' + tempStr + '</li>';";
    html += "        }";
    html += "        html += '</ul>';";
    html += "    }";
    html += "    scanResultsEl.innerHTML = html;";
    html += "}";
    html += "function pollScan(mode){";
    html += "    fetch('/scan?mode=' + encodeURIComponent(mode), { cache: 'no-store' })";
    html += "        .then(function(response){";
    html += "            return response.json().catch(function(){ return {}; }).then(function(data){";
    html += "                return { status: response.status, data: data };";
    html += "            });";
    html += "        })";
    html += "        .then(function(result){";
    html += "            const status = result.status;";
    html += "            const data = result.data || {};";
    html += "            if (status === 200) {";
    html += "                displayResults(data);";
    html += "                finishScan();";
    html += "                return;";
    html += "            }";
    html += "            if (status === 202) {";
    html += "                const scanStatus = data.status || 'in_progress';";
    html += "                renderStatus(scanStatus === 'queued' ? 'Scan queued...' : 'Scan in progress...');";
    html += "                scanTimer = setTimeout(function(){ pollScan(mode); }, pollIntervalMs);";
    html += "                return;";
    html += "            }";
    html += "            if (status === 429) {";
    html += "                const retryAfterMs = Number(data.retry_after_ms || pollIntervalMs);";
    html += "                renderStatus('Scan cooldown active. Retrying...');";
    html += "                scanTimer = setTimeout(function(){ pollScan(mode); }, retryAfterMs);";
    html += "                return;";
    html += "            }";
    html += "            if (status === 409) {";
    html += "                renderStatus('Another scan is active. Waiting...');";
    html += "                scanTimer = setTimeout(function(){ pollScan(mode); }, pollIntervalMs);";
    html += "                return;";
    html += "            }";
    html += "            renderError(data.error || ('Scan failed (' + status + ')'));";
    html += "            finishScan();";
    html += "        })";
    html += "        .catch(function(error){";
    html += "            renderError(error && error.message ? error.message : 'Request failed');";
    html += "            finishScan();";
    html += "        });";
    html += "}";
    html += "function startScan(mode){";
    html += "    if (scanInFlight) {";
    html += "        return;";
    html += "    }";
    html += "    scanInFlight = true;";
    html += "    setButtonsEnabled(false);";
    html += "    renderStatus('Starting scan...');";
    html += "    pollScan(mode);";
    html += "}";
    html += "scanConfiguredBtn.addEventListener('click', function() { startScan('configured'); });";
    html += "scanAllBtn.addEventListener('click', function() { startScan('full'); });";
    html += "</script>";
    html += "</body></html>";
    html.finish();
}

String WebServerManager::generateRelayLoggerPerformanceHtml() {
    if (!mainController) {
        return "";
    }

    const MainController::PerformanceStatsSnapshot snapshot = mainController->getPerformanceStatsSnapshot();
    const RelayLoggerStats& stats = snapshot.relayLoggerStats;

    return generateRelayLoggerRow("Relay Logs", stats.base.saveTime);
}

String WebServerManager::generateRelayLoggerRow(const char* operation, const PerformanceStats& stats) {
    String row = "<tr>";
    row += "<td style='border-bottom: 1px solid #2d2d2d; padding: 5px 0; text-align: left;'>" + String(operation) + "</td>";
    row += "<td style='border-bottom: 1px solid #2d2d2d; padding: 5px 14px 5px 0; text-align: right;'>" + String(stats.getAverage()) + "</td>";
    row += "<td style='border-bottom: 1px solid #2d2d2d; padding: 5px 14px 5px 0; text-align: right;'>" + String(stats.minTime == UINT32_MAX ? 0 : stats.minTime) + "</td>";
    row += "<td style='border-bottom: 1px solid #2d2d2d; padding: 5px 14px 5px 0; text-align: right;'>" + String(stats.maxTime) + "</td>";
    row += "</tr>";
    return row;
}

String WebServerManager::generatePerformanceStatsHtml() {
    if (!mainController) {
        return "<div class='info-card performance-card'><h3>Performance Monitoring</h3><p>Performance monitoring not available</p></div>";
    }

    const MainController::PerformanceStatsSnapshot snapshot = mainController->getPerformanceStatsSnapshot();
    const MainController::ControlQueueDiagnosticsSnapshot queueDiag = mainController->getControlQueueDiagnosticsSnapshot();
    const RelayStatusApiDiagnosticsSnapshot relayStatusApiDiag = getRelayStatusApiDiagnosticsSnapshot();
    const MainController::MemoryDiagnosticsSnapshot memoryDiag = mainController->getMemoryDiagnosticsSnapshot();
    const MainController::WiFiStabilitySnapshot wifiDiag = mainController->getWiFiStabilitySnapshot();
    const PerformanceStats& mainLoop = snapshot.mainLoopStats;
    const PerformanceStats* components = snapshot.componentStats;
    const uint32_t completedCommands = queueDiag.commandsSucceeded + queueDiag.commandsFailed + queueDiag.commandsTimedOut;
    const uint32_t avgQueueWaitMs = queueDiag.commandsStarted > 0
                                        ? static_cast<uint32_t>(queueDiag.totalQueueWaitMs / queueDiag.commandsStarted)
                                        : 0;
    const uint32_t avgExecutionMs = completedCommands > 0
                                        ? static_cast<uint32_t>(queueDiag.totalExecutionMs / completedCommands)
                                        : 0;
    const String relayLastNon200 = relayStatusApiDiag.lastNon200Status != 0 ? String(relayStatusApiDiag.lastNon200Status) : String("none");
    const String relayLastNon200Age = relayStatusApiDiag.lastNon200Status != 0 && relayStatusApiDiag.lastNon200AgeMs > 0
                                          ? (String(relayStatusApiDiag.lastNon200AgeMs / 1000UL) + "s ago")
                                          : String("never");
    const String relayLastRequestAge = relayStatusApiDiag.lastRequestAgeMs > 0
                                           ? (String(relayStatusApiDiag.lastRequestAgeMs / 1000UL) + "s ago")
                                           : String("never");
    const unsigned long uptimeMs = millis();
    const float uptimeHours = uptimeMs > 0 ? (static_cast<float>(uptimeMs) / 3600000.0f) : 0.0f;

    String html = "<div class='info-card performance-card'>";
    html += "<h3>Performance Monitoring</h3>";
    html += "<div class='component-performance'>";
    html += "<table style='width: 100%; font-family: monospace; font-size: 1.1em; border-collapse: collapse; table-layout: fixed; font-variant-numeric: tabular-nums; margin-bottom: 10px;'>";
    html += "<colgroup><col style='width: 52%;'><col style='width: 16%;'><col style='width: 16%;'><col style='width: 16%;'></colgroup>";
    html += "<thead><tr>";
    html += "<th style='font-size: 1.15em; color: #2196f3; text-align: left; padding: 5px 0; border-bottom: 1px solid #2196f3;'>Component</th>";
    html += "<th style='font-size: 1.15em; color: #2196f3; text-align: right; padding: 5px 14px 5px 0; border-bottom: 1px solid #2196f3;'>Avg</th>";
    html += "<th style='font-size: 1.15em; color: #2196f3; text-align: right; padding: 5px 14px 5px 0; border-bottom: 1px solid #2196f3;'>Min</th>";
    html += "<th style='font-size: 1.15em; color: #2196f3; text-align: right; padding: 5px 14px 5px 0; border-bottom: 1px solid #2196f3;'>Max</th>";
    html += "</tr></thead>";
    html += "<tbody>";

    // Main Loop (first row, bold)
    html += "<tr><td style='border-bottom: 1px solid #2d2d2d; padding: 5px 0; text-align: left;'><strong>Main Loop</strong></td>";
    html += "<td style='border-bottom: 1px solid #2d2d2d; padding: 5px 14px 5px 0; text-align: right;'>" + String(mainLoop.getAverage()) + "</td>";
    html += "<td style='border-bottom: 1px solid #2d2d2d; padding: 5px 14px 5px 0; text-align: right;'>" + String(mainLoop.minTime == UINT32_MAX ? 0 : mainLoop.minTime) + "</td>";
    html += "<td style='border-bottom: 1px solid #2d2d2d; padding: 5px 14px 5px 0; text-align: right;'>" + String(mainLoop.maxTime) + "</td></tr>";

    // Component names array - matches ComponentIndex enum order
    const char* componentNames[] = {
        "DS18B20",      // DS18B20_INDEX
        "SHT4x",        // SHT4X_INDEX
        "Scheduler",     // SCHEDULE_INDEX
        "MQTT",         // MQTT_INDEX
        "Web Server",   // WEB_SERVER_INDEX
        "Relay"         // RELAY_INDEX
    };

    // Add component rows
    for (int i = 0; i < COMPONENT_COUNT; i++) {
        const PerformanceStats& stats = components[i];
        //html += "<tr><td>" + String(componentNames[i]) + "</td>";
        //html += "<td>" + String(stats.getAverage()) + "</td>";
        //html += "<td>" + String(stats.minTime == UINT32_MAX ? 0 : stats.minTime) + "</td>";
        //html += "<td>" + String(stats.maxTime) + "</td></tr>";
        html += "<tr><td style='border-bottom: 1px solid #2d2d2d; padding: 5px 0; text-align: left;'>" + String(componentNames[i]) + "</td>";
        html += "<td style='border-bottom: 1px solid #2d2d2d; padding: 5px 14px 5px 0; text-align: right;'>" + String(stats.getAverage()) + "</td>";
        html += "<td style='border-bottom: 1px solid #2d2d2d; padding: 5px 14px 5px 0; text-align: right;'>" + String(stats.minTime == UINT32_MAX ? 0 : stats.minTime) + "</td>";
        html += "<td style='border-bottom: 1px solid #2d2d2d; padding: 5px 14px 5px 0; text-align: right;'>" + String(stats.maxTime) + "</td></tr>";
    }

    // Add file logger performance monitoring
    html += generateRelayLoggerPerformanceHtml();

    html += "</tbody></table>";
    html += "<table style='width: 100%; font-family: monospace; font-size: 1.05em; border-collapse: collapse; table-layout: fixed; font-variant-numeric: tabular-nums; margin-top: 8px;'>";
    html += "<colgroup><col style='width: 52%;'><col style='width: 16%;'><col style='width: 16%;'><col style='width: 16%;'></colgroup>";
    html += "<thead><tr>";
    html += "<th style='font-size: 1.1em; color: #2196f3; text-align: left; padding: 5px 0; border-bottom: 1px solid #2196f3;'>Memory Since Boot</th>";
    html += "<th style='font-size: 1.1em; color: #2196f3; text-align: right; padding: 5px 14px 5px 0; border-bottom: 1px solid #2196f3;'>Now</th>";
    html += "<th style='font-size: 1.1em; color: #2196f3; text-align: right; padding: 5px 14px 5px 0; border-bottom: 1px solid #2196f3;'>Min</th>";
    html += "<th style='font-size: 1.1em; color: #2196f3; text-align: right; padding: 5px 14px 5px 0; border-bottom: 1px solid #2196f3;'>Max</th>";
    html += "</tr></thead><tbody>";
    html += "<tr><td style='border-bottom: 1px solid #2d2d2d; padding: 5px 0; text-align:left;'>Free heap (B)</td><td style='border-bottom: 1px solid #2d2d2d; padding: 5px 14px 5px 0; text-align:right;'>" + String(memoryDiag.freeHeapCurrent) + "</td><td style='border-bottom: 1px solid #2d2d2d; padding: 5px 14px 5px 0; text-align:right;'>" + String(memoryDiag.freeHeapMin) + "</td><td style='border-bottom: 1px solid #2d2d2d; padding: 5px 14px 5px 0; text-align:right;'>" + String(memoryDiag.freeHeapMax) + "</td></tr>";
    html += "<tr><td style='border-bottom: 1px solid #2d2d2d; padding: 5px 0; text-align:left;'>Max alloc heap (B)</td><td style='border-bottom: 1px solid #2d2d2d; padding: 5px 14px 5px 0; text-align:right;'>" + String(memoryDiag.maxAllocHeapCurrent) + "</td><td style='border-bottom: 1px solid #2d2d2d; padding: 5px 14px 5px 0; text-align:right;'>" + String(memoryDiag.maxAllocHeapMin) + "</td><td style='border-bottom: 1px solid #2d2d2d; padding: 5px 14px 5px 0; text-align:right;'>" + String(memoryDiag.totalHeap) + "</td></tr>";
    if (memoryDiag.totalPsram > 0) {
        html += "<tr><td style='border-bottom: 1px solid #2d2d2d; padding: 5px 0; text-align:left;'>Free PSRAM (B)</td><td style='border-bottom: 1px solid #2d2d2d; padding: 5px 14px 5px 0; text-align:right;'>" + String(memoryDiag.freePsramCurrent) + "</td><td style='border-bottom: 1px solid #2d2d2d; padding: 5px 14px 5px 0; text-align:right;'>" + String(memoryDiag.freePsramMin) + "</td><td style='border-bottom: 1px solid #2d2d2d; padding: 5px 14px 5px 0; text-align:right;'>" + String(memoryDiag.freePsramMax) + "</td></tr>";
        html += "<tr><td style='border-bottom: 1px solid #2d2d2d; padding: 5px 0; text-align:left;'>Max alloc PSRAM (B)</td><td style='border-bottom: 1px solid #2d2d2d; padding: 5px 14px 5px 0; text-align:right;'>" + String(memoryDiag.maxAllocPsramCurrent) + "</td><td style='border-bottom: 1px solid #2d2d2d; padding: 5px 14px 5px 0; text-align:right;'>" + String(memoryDiag.maxAllocPsramMin) + "</td><td style='border-bottom: 1px solid #2d2d2d; padding: 5px 14px 5px 0; text-align:right;'>" + String(memoryDiag.totalPsram) + "</td></tr>";
    } else {
        html += "<tr><td style='border-bottom: 1px solid #2d2d2d; padding: 5px 0; text-align:left;'>PSRAM</td><td style='border-bottom: 1px solid #2d2d2d; padding: 5px 14px 5px 0; text-align:right;'>NOT AVAILABLE</td><td style='border-bottom: 1px solid #2d2d2d; padding: 5px 14px 5px 0; text-align:right;'>-</td><td style='border-bottom: 1px solid #2d2d2d; padding: 5px 14px 5px 0; text-align:right;'>-</td></tr>";
    }
    html += "</tbody></table>";
    html += "</div>";

    html += "<div class='performance-actions'>";
    html += "<form method='POST' style='display: inline;'>";
    html += "<input type='hidden' name='action' value='reset_stats'>";
    html += "<button type='submit' class='btn-reset'>Reset Stats</button>";
    html += "</form>";
    html += "</div>";
    html += "</div>";

    html += "<div class='info-card performance-card'>";
    html += "<h3>Control Queue Diagnostics</h3>";
    html += "<div class='component-performance'>";
    html += "<table style='width: 100%; font-family: monospace; font-size: 1.1em; border-collapse: collapse; margin-bottom: 10px;'>";
    html += "<thead><tr>";
    html += "<th style='font-size: 1.15em; color: #2196f3; text-align: left; padding: 5px 0; border-bottom: 1px solid #2196f3;'>Metric</th>";
    html += "<th style='font-size: 1.15em; color: #2196f3; text-align: right; padding: 5px 0; border-bottom: 1px solid #2196f3;'>Value</th>";
    html += "</tr></thead>";
    html += "<tbody>";
    html += "<tr><td style='border-bottom: 1px solid #2d2d2d; padding: 5px 0; text-align:left;'>Enqueue accepted</td><td style='border-bottom: 1px solid #2d2d2d; padding: 5px 0; text-align:right;'>" + String(queueDiag.enqueueAccepted) + "</td></tr>";
    html += "<tr><td style='border-bottom: 1px solid #2d2d2d; padding: 5px 0; text-align:left;'>Enqueue rejected</td><td style='border-bottom: 1px solid #2d2d2d; padding: 5px 0; text-align:right;'>" + String(queueDiag.enqueueRejected) + "</td></tr>";
    html += "<tr><td style='border-bottom: 1px solid #2d2d2d; padding: 5px 0; text-align:left;'>Queue depth (queued current/max)</td><td style='border-bottom: 1px solid #2d2d2d; padding: 5px 0; text-align:right;'>" + String(queueDiag.currentQueueDepth) + " / " + String(queueDiag.maxQueueDepth) + "</td></tr>";
    html += "<tr><td style='border-bottom: 1px solid #2d2d2d; padding: 5px 0; text-align:left;'>Running commands (current)</td><td style='border-bottom: 1px solid #2d2d2d; padding: 5px 0; text-align:right;'>" + String(queueDiag.runningCount) + "</td></tr>";
    html += "<tr><td style='border-bottom: 1px solid #2d2d2d; padding: 5px 0; text-align:left;'>In-flight (current/max)</td><td style='border-bottom: 1px solid #2d2d2d; padding: 5px 0; text-align:right;'>" + String(queueDiag.currentInFlight) + " / " + String(queueDiag.maxInFlight) + "</td></tr>";
    html += "<tr><td style='border-bottom: 1px solid #2d2d2d; padding: 5px 0; text-align:left;'>Commands started</td><td style='border-bottom: 1px solid #2d2d2d; padding: 5px 0; text-align:right;'>" + String(queueDiag.commandsStarted) + "</td></tr>";
    html += "<tr><td style='border-bottom: 1px solid #2d2d2d; padding: 5px 0; text-align:left;'>Commands done (ok/fail/timeout)</td><td style='border-bottom: 1px solid #2d2d2d; padding: 5px 0; text-align:right;'>" + String(queueDiag.commandsSucceeded) + " / " + String(queueDiag.commandsFailed) + " / " + String(queueDiag.commandsTimedOut) + "</td></tr>";
    html += "<tr><td style='border-bottom: 1px solid #2d2d2d; padding: 5px 0; text-align:left;'>Timeout detail (queue/exec)</td><td style='border-bottom: 1px solid #2d2d2d; padding: 5px 0; text-align:right;'>" + String(queueDiag.queueTimeouts) + " / " + String(queueDiag.executionTimeouts) + "</td></tr>";
    html += "<tr><td style='border-bottom: 1px solid #2d2d2d; padding: 5px 0; text-align:left;'>Queue wait avg/max (ms)</td><td style='border-bottom: 1px solid #2d2d2d; padding: 5px 0; text-align:right;'>" + String(avgQueueWaitMs) + " / " + String(queueDiag.maxQueueWaitMs) + "</td></tr>";
    html += "<tr><td style='border-bottom: 1px solid #2d2d2d; padding: 5px 0; text-align:left;'>Queue wait p95 (ms)</td><td style='border-bottom: 1px solid #2d2d2d; padding: 5px 0; text-align:right;'>" + String(queueDiag.p95QueueWaitMs) + "</td></tr>";
    html += "<tr><td style='border-bottom: 1px solid #2d2d2d; padding: 5px 0; text-align:left;'>Execution avg/max (ms)</td><td style='border-bottom: 1px solid #2d2d2d; padding: 5px 0; text-align:right;'>" + String(avgExecutionMs) + " / " + String(queueDiag.maxExecutionMs) + "</td></tr>";
    html += "<tr><td style='border-bottom: 1px solid #2d2d2d; padding: 5px 0; text-align:left;'>Execution p95 (ms)</td><td style='border-bottom: 1px solid #2d2d2d; padding: 5px 0; text-align:right;'>" + String(queueDiag.p95ExecutionMs) + "</td></tr>";
    html += "<tr><td style='border-bottom: 1px solid #2d2d2d; padding: 5px 0; text-align:left;'>Processor active</td><td style='border-bottom: 1px solid #2d2d2d; padding: 5px 0; text-align:right;'>" + String(queueDiag.processingActive ? "true" : "false") + "</td></tr>";
    html += "</tbody></table>";
    html += "<table style='width: 100%; font-family: monospace; font-size: 1.05em; border-collapse: collapse; margin-top: 8px;'>";
    html += "<thead><tr>";
    html += "<th style='font-size: 1.1em; color: #2196f3; text-align: left; padding: 6px 0; border-bottom: 1px solid #2196f3;'>Relay Status API</th>";
    html += "<th style='font-size: 1.1em; color: #2196f3; text-align: right; padding: 6px 0; border-bottom: 1px solid #2196f3;'>Value</th>";
    html += "</tr></thead><tbody>";
    html += "<tr><td style='border-bottom: 1px solid #2d2d2d; padding: 5px 0; text-align:left;'>Requests total</td><td style='border-bottom: 1px solid #2d2d2d; padding: 5px 0; text-align:right;'>" + String(relayStatusApiDiag.requestsTotal) + "</td></tr>";
    html += "<tr><td style='border-bottom: 1px solid #2d2d2d; padding: 5px 0; text-align:left;'>Status buckets (2xx/4xx/5xx/other)</td><td style='border-bottom: 1px solid #2d2d2d; padding: 5px 0; text-align:right;'>" + String(relayStatusApiDiag.status2xx) + " / " + String(relayStatusApiDiag.status4xx) + " / " + String(relayStatusApiDiag.status5xx) + " / " + String(relayStatusApiDiag.statusOther) + "</td></tr>";
    html += "<tr><td style='border-bottom: 1px solid #2d2d2d; padding: 5px 0; text-align:left;'>Latency avg/p95/max (ms)</td><td style='border-bottom: 1px solid #2d2d2d; padding: 5px 0; text-align:right;'>" + String(relayStatusApiDiag.avgLatencyMs) + " / " + String(relayStatusApiDiag.p95LatencyMs) + " / " + String(relayStatusApiDiag.maxLatencyMs) + "</td></tr>";
    html += "<tr><td style='border-bottom: 1px solid #2d2d2d; padding: 5px 0; text-align:left;'>Last non-200</td><td style='border-bottom: 1px solid #2d2d2d; padding: 5px 0; text-align:right;'>" + relayLastNon200 + " (" + relayLastNon200Age + ")</td></tr>";
    html += "<tr><td style='border-bottom: 1px solid #2d2d2d; padding: 5px 0; text-align:left;'>Last request</td><td style='border-bottom: 1px solid #2d2d2d; padding: 5px 0; text-align:right;'>" + relayLastRequestAge + "</td></tr>";
    html += "</tbody></table>";
    html += "</div>";
    html += "<div class='performance-actions'>";
    html += "<form method='POST' style='display: inline;'>";
    html += "<input type='hidden' name='action' value='reset_queue_diag'>";
    html += "<button type='submit' class='btn-reset'>Reset Queue Diagnostics</button>";
    html += "</form>";
    html += "</div>";
    html += "</div>";

    String mqttConnectionText = "N/A";
    String mqttConnectionColor = "#ff9800";
    String mqttConnectedForSec = "N/A";
    String mqttDisconnectedForSec = "N/A";
    String mqttPublishIntervalSec = "N/A";
    String mqttDiscoveryState = "N/A";
    String mqttAttempts = "N/A";
    String mqttSuccess = "N/A";
    String mqttFailed = "N/A";
    String mqttSuccessRateText = "N/A";
    String mqttSuccessRateColor = "#ff9800";
    String mqttReconnectState = "N/A";
    String mqttLastReconnectAttempt = "N/A";
    String mqttLastPublishAttempt = "N/A";
    String mqttDisconnectReason = "N/A";
    String mqttLastDisconnectAge = "N/A";
    String mqttBufferedEntries = "N/A";
    String mqttBufferedBytes = "N/A";
    String mqttBufferedFlushPending = "N/A";
    String mqttBufferedDrops = "N/A";
    String mqttBufferedEvictions = "N/A";
    String mqttReconnectRate = "N/A";

    String wifiConnectionText = wifiDiag.connected ? "CONNECTED" : "DISCONNECTED";
    String wifiConnectionColor = wifiDiag.connected ? "#4caf50" : "#f44336";
    String wifiRssi = wifiDiag.connected ? (String(wifiDiag.rssi) + " dBm") : "N/A";
    String wifiStatusCode = String(wifiDiag.statusCode);
    String wifiLastDisconnectStatusCode = String(wifiDiag.lastDisconnectStatusCode);
    String wifiReconnectTransitions = String(wifiDiag.reconnectTransitions);
    String wifiDisconnectTransitions = String(wifiDiag.disconnectTransitions);
    String wifiLastReconnectAge = wifiDiag.lastReconnectAgeMs > 0 ? (String(wifiDiag.lastReconnectAgeMs / 1000UL) + "s ago") : "never";
    String wifiLastDisconnectAge = wifiDiag.lastDisconnectAgeMs > 0 ? (String(wifiDiag.lastDisconnectAgeMs / 1000UL) + "s ago") : "never";
    String wifiReconnectRate = "N/A";
    if (uptimeHours > 0.0f) {
        const float wifiRate = static_cast<float>(wifiDiag.reconnectTransitions) / uptimeHours;
        wifiReconnectRate = String(wifiRate, 2) + "/h";
    }

    if (mqttManager) {
        const bool mqttConnected = mqttManager->isConnected();
        const MQTTManager::PublishStats pubStats = mqttManager->getPublishStatsSnapshot();
        const MQTTManager::DiagnosticsSnapshot mqttDiag = mqttManager->getDiagnosticsSnapshot();
        mqttConnectionText = mqttConnected ? "CONNECTED" : "DISCONNECTED";
        mqttConnectionColor = mqttConnected ? "#4caf50" : "#f44336";
        mqttConnectedForSec = String(mqttManager->getConnectionDuration() / 1000UL);
        mqttDisconnectedForSec = String(mqttManager->getDisconnectionDuration() / 1000UL);
        mqttPublishIntervalSec = String(mqttManager->getPublishInterval() / 1000UL);
        mqttDiscoveryState = mqttDiag.discoveryPublished ? "PUBLISHED" : (mqttDiag.discoveryInProgress ? "IN PROGRESS" : "PENDING");
        mqttAttempts = String(pubStats.attempts);
        mqttSuccess = String(pubStats.success);
        mqttFailed = String(pubStats.failed);
        mqttReconnectState = mqttDiag.reconnectAttemptInProgress ? "IN PROGRESS" : "IDLE";
        mqttLastReconnectAttempt = mqttDiag.reconnectAttemptAgeMs > 0 ? (String(mqttDiag.reconnectAttemptAgeMs / 1000UL) + "s ago") : "never";
        mqttLastPublishAttempt = mqttDiag.lastPublishAttemptAgeMs > 0 ? (String(mqttDiag.lastPublishAttemptAgeMs / 1000UL) + "s ago") : "never";
        mqttDisconnectReason = String(mqttDiag.lastDisconnectReason);
        mqttLastDisconnectAge = mqttDiag.lastDisconnectAgeMs > 0 ? (String(mqttDiag.lastDisconnectAgeMs / 1000UL) + "s ago") : "never";
        mqttBufferedEntries = String(static_cast<unsigned int>(mqttDiag.bufferedEntryCount));
        mqttBufferedBytes = String(static_cast<unsigned int>(mqttDiag.bufferedBytes));
        mqttBufferedFlushPending = mqttDiag.bufferedFlushPending ? "true" : "false";
        mqttBufferedDrops = String(mqttDiag.bufferedDropCount);
        mqttBufferedEvictions = String(mqttDiag.bufferedEvictionCount);
        if (uptimeHours > 0.0f) {
            const uint32_t reconnects = mqttDiag.connectTransitions > 0 ? (mqttDiag.connectTransitions - 1U) : 0U;
            const float mqttRate = static_cast<float>(reconnects) / uptimeHours;
            mqttReconnectRate = String(mqttRate, 2) + "/h";
        }

        if (pubStats.attempts > 0) {
            const float successRate = (100.0f * static_cast<float>(pubStats.success)) / static_cast<float>(pubStats.attempts);
            mqttSuccessRateText = String(successRate, 1) + "%";
            mqttSuccessRateColor = (successRate >= 95.0f) ? "#4caf50" : ((successRate >= 80.0f) ? "#ff9800" : "#f44336");
        }
    }

    html += "<div class='info-card performance-card'>";
    html += "<h3>Connectivity Diagnostics</h3>";
    html += "<div class='component-performance'>";
    html += "<table style='width: 100%; font-family: monospace; font-size: 1.1em; border-collapse: collapse; margin-bottom: 10px;'>";
    html += "<thead><tr>";
    html += "<th style='font-size: 1.15em; color: #2196f3; text-align: left; padding: 5px 0; border-bottom: 1px solid #2196f3;'>Metric</th>";
    html += "<th style='font-size: 1.15em; color: #2196f3; text-align: right; padding: 5px 0; border-bottom: 1px solid #2196f3;'>Value</th>";
    html += "</tr></thead>";
    html += "<tbody>";
    html += "<tr><td style='border-bottom: 1px solid #2d2d2d; padding: 5px 0; text-align:left; color:#2196f3;'><strong>WiFi</strong></td><td style='border-bottom: 1px solid #2d2d2d; padding: 5px 0; text-align:right;'>-</td></tr>";
    html += "<tr><td style='border-bottom: 1px solid #2d2d2d; padding: 5px 0; text-align:left;'>Connection</td><td style='border-bottom: 1px solid #2d2d2d; padding: 5px 0; text-align:right; color: " + wifiConnectionColor + ";'>" + wifiConnectionText + "</td></tr>";
    html += "<tr><td style='border-bottom: 1px solid #2d2d2d; padding: 5px 0; text-align:left;'>RSSI</td><td style='border-bottom: 1px solid #2d2d2d; padding: 5px 0; text-align:right;'>" + wifiRssi + "</td></tr>";
    html += "<tr><td style='border-bottom: 1px solid #2d2d2d; padding: 5px 0; text-align:left;'>Status code</td><td style='border-bottom: 1px solid #2d2d2d; padding: 5px 0; text-align:right;'>" + wifiStatusCode + "</td></tr>";
    html += "<tr><td style='border-bottom: 1px solid #2d2d2d; padding: 5px 0; text-align:left;'>Reconnects</td><td style='border-bottom: 1px solid #2d2d2d; padding: 5px 0; text-align:right;'>" + wifiReconnectTransitions + " (" + wifiReconnectRate + ")</td></tr>";
    html += "<tr><td style='border-bottom: 1px solid #2d2d2d; padding: 5px 0; text-align:left;'>Disconnects</td><td style='border-bottom: 1px solid #2d2d2d; padding: 5px 0; text-align:right;'>" + wifiDisconnectTransitions + "</td></tr>";
    html += "<tr><td style='border-bottom: 1px solid #2d2d2d; padding: 5px 0; text-align:left;'>Last reconnect</td><td style='border-bottom: 1px solid #2d2d2d; padding: 5px 0; text-align:right;'>" + wifiLastReconnectAge + "</td></tr>";
    html += "<tr><td style='border-bottom: 1px solid #2d2d2d; padding: 5px 0; text-align:left;'>Last disconnect</td><td style='border-bottom: 1px solid #2d2d2d; padding: 5px 0; text-align:right;'>" + wifiLastDisconnectAge + " (code " + wifiLastDisconnectStatusCode + ")</td></tr>";

    html += "<tr><td style='border-bottom: 1px solid #2d2d2d; padding: 5px 0; text-align:left; color:#2196f3;'><strong>MQTT</strong></td><td style='border-bottom: 1px solid #2d2d2d; padding: 5px 0; text-align:right;'>-</td></tr>";
    html += "<tr><td style='border-bottom: 1px solid #2d2d2d; padding: 5px 0; text-align:left;'>Connection</td><td style='border-bottom: 1px solid #2d2d2d; padding: 5px 0; text-align:right; color: " + mqttConnectionColor + ";'>" + mqttConnectionText + "</td></tr>";
    html += "<tr><td style='border-bottom: 1px solid #2d2d2d; padding: 5px 0; text-align:left;'>Connected for (s)</td><td style='border-bottom: 1px solid #2d2d2d; padding: 5px 0; text-align:right;'>" + mqttConnectedForSec + "</td></tr>";
    html += "<tr><td style='border-bottom: 1px solid #2d2d2d; padding: 5px 0; text-align:left;'>Disconnected for (s)</td><td style='border-bottom: 1px solid #2d2d2d; padding: 5px 0; text-align:right;'>" + mqttDisconnectedForSec + "</td></tr>";
    html += "<tr><td style='border-bottom: 1px solid #2d2d2d; padding: 5px 0; text-align:left;'>Reconnect rate</td><td style='border-bottom: 1px solid #2d2d2d; padding: 5px 0; text-align:right;'>" + mqttReconnectRate + "</td></tr>";
    html += "<tr><td style='border-bottom: 1px solid #2d2d2d; padding: 5px 0; text-align:left;'>Publish interval (s)</td><td style='border-bottom: 1px solid #2d2d2d; padding: 5px 0; text-align:right;'>" + mqttPublishIntervalSec + "</td></tr>";
    html += "<tr><td style='border-bottom: 1px solid #2d2d2d; padding: 5px 0; text-align:left;'>Discovery</td><td style='border-bottom: 1px solid #2d2d2d; padding: 5px 0; text-align:right;'>" + mqttDiscoveryState + "</td></tr>";
    html += "<tr><td style='border-bottom: 1px solid #2d2d2d; padding: 5px 0; text-align:left;'>Last disconnect reason</td><td style='border-bottom: 1px solid #2d2d2d; padding: 5px 0; text-align:right;'>" + mqttDisconnectReason + "</td></tr>";
    html += "<tr><td style='border-bottom: 1px solid #2d2d2d; padding: 5px 0; text-align:left;'>Last disconnect age</td><td style='border-bottom: 1px solid #2d2d2d; padding: 5px 0; text-align:right;'>" + mqttLastDisconnectAge + "</td></tr>";
    html += "<tr><td style='border-bottom: 1px solid #2d2d2d; padding: 5px 0; text-align:left;'>Publish attempts</td><td style='border-bottom: 1px solid #2d2d2d; padding: 5px 0; text-align:right;'>" + mqttAttempts + "</td></tr>";
    html += "<tr><td style='border-bottom: 1px solid #2d2d2d; padding: 5px 0; text-align:left;'>Publish success</td><td style='border-bottom: 1px solid #2d2d2d; padding: 5px 0; text-align:right;'>" + mqttSuccess + "</td></tr>";
    html += "<tr><td style='border-bottom: 1px solid #2d2d2d; padding: 5px 0; text-align:left;'>Publish failed</td><td style='border-bottom: 1px solid #2d2d2d; padding: 5px 0; text-align:right;'>" + mqttFailed + "</td></tr>";
    html += "<tr><td style='border-bottom: 1px solid #2d2d2d; padding: 5px 0; text-align:left;'>Deferred cache entries</td><td style='border-bottom: 1px solid #2d2d2d; padding: 5px 0; text-align:right;'>" + mqttBufferedEntries + "</td></tr>";
    html += "<tr><td style='border-bottom: 1px solid #2d2d2d; padding: 5px 0; text-align:left;'>Deferred cache bytes</td><td style='border-bottom: 1px solid #2d2d2d; padding: 5px 0; text-align:right;'>" + mqttBufferedBytes + "</td></tr>";
    html += "<tr><td style='border-bottom: 1px solid #2d2d2d; padding: 5px 0; text-align:left;'>Deferred flush pending</td><td style='border-bottom: 1px solid #2d2d2d; padding: 5px 0; text-align:right;'>" + mqttBufferedFlushPending + "</td></tr>";
    html += "<tr><td style='border-bottom: 1px solid #2d2d2d; padding: 5px 0; text-align:left;'>Deferred drops/evictions</td><td style='border-bottom: 1px solid #2d2d2d; padding: 5px 0; text-align:right;'>" + mqttBufferedDrops + " / " + mqttBufferedEvictions + "</td></tr>";
    html += "<tr><td style='border-bottom: 1px solid #2d2d2d; padding: 5px 0; text-align:left;'>Reconnect state</td><td style='border-bottom: 1px solid #2d2d2d; padding: 5px 0; text-align:right;'>" + mqttReconnectState + "</td></tr>";
    html += "<tr><td style='border-bottom: 1px solid #2d2d2d; padding: 5px 0; text-align:left;'>Last reconnect attempt</td><td style='border-bottom: 1px solid #2d2d2d; padding: 5px 0; text-align:right;'>" + mqttLastReconnectAttempt + "</td></tr>";
    html += "<tr><td style='border-bottom: 1px solid #2d2d2d; padding: 5px 0; text-align:left;'>Last publish attempt</td><td style='border-bottom: 1px solid #2d2d2d; padding: 5px 0; text-align:right;'>" + mqttLastPublishAttempt + "</td></tr>";
    html += "<tr><td style='border-bottom: 1px solid #2d2d2d; padding: 5px 0; text-align:left;'>Success rate</td><td style='border-bottom: 1px solid #2d2d2d; padding: 5px 0; text-align:right; color: " + mqttSuccessRateColor + ";'>" + mqttSuccessRateText + "</td></tr>";
    html += "</tbody></table>";
    html += "</div>";
    html += "</div>";

    return html;
}

void WebServerManager::streamPerformanceHtml(webserver::IResponse& response) {
    HtmlStreamWriter html(response);
    html += "<html><head><title>Performance</title><style>";
    html += "body { background-color: #1e1e1e; color: #ffffff; font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif; margin: 0; padding: 60px 20px 20px 20px; }";
    html += "h1 { color: #2196f3; margin-bottom: 20px; }";
    html += ".info-grid { display: grid; grid-template-columns: repeat(3, minmax(0, 1fr)); gap: 12px; margin-bottom: 20px; }";
    html += ".info-card { background: #2d2d2d; border: 1px solid #424242; border-radius: 8px; padding: 15px; }";
    html += ".info-card h3 { margin-top: 0; }";
    html += ".performance-card { min-width: 0; display: flex; flex-direction: column; }";
    html += ".performance-card .component-performance { overflow-x: auto; }";
    html += ".performance-card .component-performance { flex: 1 1 auto; }";
    html += ".performance-card table { width: 100%; border-collapse: collapse; }";
    html += ".performance-card th, .performance-card td { overflow-wrap: anywhere; word-break: break-word; }";
    html += ".performance-actions { margin-top: auto; display: flex; justify-content: flex-start; padding-top: 10px; }";
    html += ".btn-reset { background-color: #2196f3; color: white; border: none; padding: 10px 14px; border-radius: 4px; cursor: pointer; font-size: 0.95em; }";
    html += ".btn-reset:hover { background-color: #1976d2; }";
    html += ".muted { color: #b0b0b0; font-size: 0.95em; margin-bottom: 15px; }";
    html += "@media (max-width: 1400px) { .info-grid { grid-template-columns: repeat(2, minmax(0, 1fr)); } }";
    html += "@media (max-width: 1000px) { .info-grid { grid-template-columns: 1fr; } }";
    html += "</style></head><body>" + getNavHtml("performance");

    html += "<h1>Performance & Diagnostics</h1>";
    html += "<div class='muted'>Auto-refresh: 5 seconds</div>";
    html += "<div id='performanceGrid' class='info-grid'>";
    html += generatePerformanceStatsHtml();
    html += "</div>";
    html += "<script>";
    html += "const PERF_POLL_MS = 5000;";
    html += "let perfPollInFlight = false;";
    html += "async function refreshPerformanceCards(){";
    html += "  if (perfPollInFlight || document.visibilityState !== 'visible') return;";
    html += "  perfPollInFlight = true;";
    html += "  try {";
    html += "    const response = await fetch('/api/performance/cards?t=' + Date.now(), { credentials: 'same-origin', cache: 'no-store' });";
    html += "    if (!response.ok) throw new Error('HTTP ' + response.status);";
    html += "    const cardsHtml = await response.text();";
    html += "    const grid = document.getElementById('performanceGrid');";
    html += "    if (grid) grid.innerHTML = cardsHtml;";
    html += "  } catch (error) {";
    html += "    console.log('Performance refresh failed:', error);";
    html += "  } finally {";
    html += "    perfPollInFlight = false;";
    html += "  }";
    html += "}";
    html += "setInterval(function(){ refreshPerformanceCards().catch(function(){}); }, PERF_POLL_MS);";
    html += "document.addEventListener('visibilitychange', function(){";
    html += "  if (document.visibilityState === 'visible') { refreshPerformanceCards().catch(function(){}); }";
    html += "});";
    html += "</script>";
    html += "</body></html>";
    html.finish();
}

String WebServerManager::generateMonitoringCardsHtml(const Settings& settings) {
    String html;

    // System Status Overview
    html += "<h2>System Status Overview</h2>";
    html += "<div class='status-grid' style='grid-template-columns: repeat(auto-fit, minmax(220px, 1fr));'>";

    // WiFi Status Card
    html += "<div class='status-card'>";
    html += "<h3>WiFi Connection</h3>";
    String wifiStatus = WiFi.isConnected() ? "CONNECTED" : "DISCONNECTED";
    String wifiStatusClass = WiFi.isConnected() ? "status-good" : "status-error";
    html += "<div class='status-item'><span class='status-label'>Status:</span><span class='status-value " + wifiStatusClass + "'>" + wifiStatus + "</span></div>";
    html += "<div class='status-item'><span class='status-label'>IP Address:</span><span class='status-value'>" + WiFi.localIP().toString() + "</span></div>";
    html += "<div class='status-item'><span class='status-label'>SSID:</span><span class='status-value'>" + WiFi.SSID() + "</span></div>";
    html += "<div class='status-item'><span class='status-label'>Retry Count:</span><span class='status-value'>N/A</span></div>";
    html += "</div>";

    // MQTT Status Card
    html += "<div class='status-card'>";
    html += "<h3>MQTT Connection</h3>";
    String mqttStatus = mqttManager && mqttManager->isConnected() ? "CONNECTED" : "DISCONNECTED";
    String mqttStatusClass = mqttManager && mqttManager->isConnected() ? "status-good" : "status-error";
    html += "<div class='status-item'><span class='status-label'>Status:</span><span class='status-value " + mqttStatusClass + "'>" + mqttStatus + "</span></div>";
    html += "<div class='status-item'><span class='status-label'>Broker:</span><span class='status-value'>" + settings.mqtt.server + ":" + String(settings.mqtt.port) + "</span></div>";
    html += "<div class='status-item'><span class='status-label'>Client ID:</span><span class='status-value'>" + settings.mqtt.client_id + "</span></div>";
    if (mqttManager) {
        const MQTTManager::PublishStats pubStats = mqttManager->getPublishStatsSnapshot();
        String publishRateText = "No Data";
        String publishRateClass = "status-warning";
        if (pubStats.attempts > 0) {
            const float successRate = (100.0f * static_cast<float>(pubStats.success)) / static_cast<float>(pubStats.attempts);
            publishRateText = String(successRate, 1) + "% (" + String(pubStats.success) + "/" + String(pubStats.attempts) + ")";
            publishRateClass = (successRate >= 95.0f) ? "status-good" : ((successRate >= 80.0f) ? "status-warning" : "status-error");
        }
        html += "<div class='status-item'><span class='status-label'>Publish Rate:</span><span class='status-value " + publishRateClass + "'>" + publishRateText + "</span></div>";
    } else {
        html += "<div class='status-item'><span class='status-label'>Publish Rate:</span><span class='status-value status-warning'>N/A</span></div>";
    }
    html += "</div>";

    // DS18B20 Sensors Card
    html += "<div class='status-card'>";
    html += "<h3>DS18B20 Sensors</h3>";
    html += "<div class='status-item'><span class='status-label'>Status:</span><span class='status-value " + String(settings.sensors.ds18b20_enabled ? "status-good'>ENABLED" : "status-warning'>DISABLED") + "</span></div>";
    html += "<div class='status-item'><span class='status-label'>Configured:</span><span class='status-value'>" + String(settings.sensors.sensor_count) + "</span></div>";

    int activeSensors = 0;
    if (ds18b20Manager) {
        for (int i = 0; i < ds18b20Manager->getSensorCount(); i++) {
            if (ds18b20Manager->isSensorOperational(i)) {
                activeSensors++;
            }
        }
    }
    String activeClass = (activeSensors > 0) ? "status-good" : "status-error";
    html += "<div class='status-item'><span class='status-label'>Active:</span><span class='status-value " + activeClass + "'>" + String(activeSensors) + "</span></div>";
    html += "<div class='status-item'><span class='status-label'>Bus Count:</span><span class='status-value'>" + String(settings.sensors.one_wire_bus_count) + "</span></div>";
    html += "</div>";

    // SHT4x Sensor Card
    html += "<div class='status-card'>";
    html += "<h3>SHT4x Sensor</h3>";
    html += "<div class='status-item'><span class='status-label'>Status:</span><span class='status-value " + String(settings.sensors.sht4x_enabled ? "status-good'>ENABLED" : "status-warning'>DISABLED") + "</span></div>";
    if (settings.sensors.sht4x_enabled) {
        html += "<div class='status-item'><span class='status-label'>I2C Address:</span><span class='status-value'>0x" + String(settings.sensors.sht4x_i2c_address, HEX) + "</span></div>";
        html += "<div class='status-item'><span class='status-label'>Precision:</span><span class='status-value'>" + String(settings.sensors.sht4x_precision == 0 ? "Low" : settings.sensors.sht4x_precision == 1 ? "Medium" : "High") + "</span></div>";
        html += "<div class='status-item'><span class='status-label'>Heater:</span><span class='status-value'>" + String(settings.sensors.sht4x_heater_power == 0 ? "Off" : "On") + "</span></div>";
    }
    html += "</div>";

    // MAX31855K Thermocouple Card
    html += "<div class='status-card'>";
    html += "<h3>MAX31855 K-Probe</h3>";
    html += "<div class='status-item'><span class='status-label'>Status:</span><span class='status-value " + String(settings.sensors.max31855k_enabled ? "status-good'>ENABLED" : "status-warning'>DISABLED") + "</span></div>";
    if (settings.sensors.max31855k_enabled) {
        if (max31855kManager) {
            const char* runtimeStatus = max31855kManager->getStatusString();
            String runtimeStatusStr = String(runtimeStatus);
            String runtimeClass = (runtimeStatusStr == "OK") ? "status-good" : (runtimeStatusStr == "FAULT") ? "status-error" : "status-warning";
            html += "<div class='status-item'><span class='status-label'>Runtime:</span><span class='status-value " + runtimeClass + "'>" + runtimeStatusStr + "</span></div>";

            const bool oc = max31855kManager->faultOpenCircuit();
            const bool scg = max31855kManager->faultShortToGnd();
            const bool scv = max31855kManager->faultShortToVcc();
            const bool anyFault = max31855kManager->hasFault();
            String faultClass = anyFault ? "status-error" : "status-good";
            html += "<div class='status-item'><span class='status-label'>Faults:</span><span class='status-value " + faultClass + "'>";
            html += String("OC=") + (oc ? "1" : "0") + " SCG=" + (scg ? "1" : "0") + " SCV=" + (scv ? "1" : "0");
            html += "</span></div>";

            html += "<div class='status-item'><span class='status-label'>Fault Count:</span><span class='status-value'>" + String(max31855kManager->getFaultCount()) + "</span></div>";

            const unsigned long nowMs = millis();
            const unsigned long lastOkMs = max31855kManager->getLastOkMillis();
            String lastOkAge = (lastOkMs > 0 && nowMs >= lastOkMs) ? String((nowMs - lastOkMs) / 1000.0, 1) + "s" : "Never";
            html += "<div class='status-item'><span class='status-label'>Last OK Age:</span><span class='status-value'>" + lastOkAge + "</span></div>";
        } else {
            html += "<div class='status-item'><span class='status-label'>Runtime:</span><span class='status-value status-warning'>NOT INITIALIZED</span></div>";
        }
    }
    html += "</div>";

    // Authentication Security Card
    html += "<div class='status-card'>";
    html += "<h3>Authentication Security</h3>";
    String authStatus = authEnabled ? "ENABLED" : "DISABLED";
    String authStatusClass = authEnabled ? "status-good" : "status-warning";
    html += "<div class='status-item'><span class='status-label'>Status:</span><span class='status-value " + authStatusClass + "'>" + authStatus + "</span></div>";
    html += "<div class='status-item'><span class='status-label'>HTTP Port:</span><span class='status-value'>80</span></div>";
    html += "<div class='status-item'><span class='status-label'>Username:</span><span class='status-value'>" + String(authEnabled ? settings.security.username : "N/A") + "</span></div>";
    html += "<div class='status-item'><span class='status-label'>Method:</span><span class='status-value'>" + String(authEnabled ? "HTTP Basic Auth" : "None") + "</span></div>";
    html += "</div>";

    html += "</div>"; // End status-grid

    // Combined Error Monitoring and System Information
    html += "<div class='info-grid'>";
    html += "<h3 class='grid-header error-header'>Error Monitoring</h3>";
    html += "<h3 class='grid-header system-header'>System Information</h3>";

    html += "<div class='info-card'>";
    html += "<div id='activeErrors'>No active errors</div>";
    html += "<h3>Error Statistics</h3>";
    html += "<div class='status-item'><span class='status-label'>Total Errors:</span><span class='status-value' id='totalErrors'>0</span></div>";
    html += "<div class='status-item'><span class='status-label'>Errors This Hour:</span><span class='status-value' id='errorsLastHour'>0</span></div>";
    html += "<div class='status-item'><span class='status-label'>Last Error:</span><span class='status-value' id='lastErrorTime'>Never</span></div>";
    html += "</div>";

    html += "<div class='info-card'>";
    html += "<h3>Memory Usage</h3>";
    uint32_t freeHeap = ESP.getFreeHeap();
    uint32_t totalHeap = ESP.getHeapSize();
    uint32_t maxAlloc = ESP.getMaxAllocHeap();
    uint32_t totalPsram = ESP.getPsramSize();
    uint32_t freePsram = ESP.getFreePsram();
    uint32_t maxAllocPsram = ESP.getMaxAllocPsram();
    float maxAllocPercent = (totalHeap > 0) ? ((float)maxAlloc / (float)totalHeap) * 100.0f : 0.0f;
    float freePsramPercent = (totalPsram > 0) ? ((float)freePsram / (float)totalPsram) * 100.0f : 0.0f;
    float maxAllocPsramPercent = (totalPsram > 0) ? ((float)maxAllocPsram / (float)totalPsram) * 100.0f : 0.0f;
    if (maxAllocPercent < 0.0f) {
        maxAllocPercent = 0.0f;
    } else if (maxAllocPercent > 100.0f) {
        maxAllocPercent = 100.0f;
    }
    if (freePsramPercent < 0.0f) {
        freePsramPercent = 0.0f;
    } else if (freePsramPercent > 100.0f) {
        freePsramPercent = 100.0f;
    }
    if (maxAllocPsramPercent < 0.0f) {
        maxAllocPsramPercent = 0.0f;
    } else if (maxAllocPsramPercent > 100.0f) {
        maxAllocPsramPercent = 100.0f;
    }
    html += "<div class='status-item'><span class='status-label'>Free Heap:</span><span class='status-value'>" + String(freeHeap) + " bytes</span></div>";
    html += "<div class='status-item'><span class='status-label'>Max Alloc:</span><span class='status-value'>" + String(maxAlloc) + " bytes</span></div>";
    html += "<div class='progress-bar'><div class='progress-fill' style='width:" + String(maxAllocPercent, 1) + "%'></div><div class='progress-text'>" + String(maxAllocPercent, 1) + "%</div></div>";
    html += "<div class='status-item'><span class='status-label'>Total Heap:</span><span class='status-value'>" + String(totalHeap) + " bytes</span></div>";
    if (totalPsram > 0) {
        html += "<div style='height: 10px;'></div>";
        html += "<div class='status-item'><span class='status-label'>Free PSRAM:</span><span class='status-value'>" + String(freePsram) + " bytes</span></div>";
        html += "<div class='status-item'><span class='status-label'>Max PSRAM Alloc:</span><span class='status-value'>" + String(maxAllocPsram) + " bytes</span></div>";
        html += "<div class='progress-bar'><div class='progress-fill' style='width:" + String(maxAllocPsramPercent, 1) + "%'></div><div class='progress-text'>" + String(maxAllocPsramPercent, 1) + "%</div></div>";
        html += "<div class='status-item'><span class='status-label'>Total PSRAM:</span><span class='status-value'>" + String(totalPsram) + " bytes</span></div>";
    } else {
        html += "<div class='status-item'><span class='status-label'>PSRAM:</span><span class='status-value status-warning'>NOT AVAILABLE</span></div>";
    }
    html += "</div>";

    html += "<div class='info-card'>";
    html += "<h3>System Details</h3>";
    html += "<div class='status-item'><span class='status-label'>Code Date:</span><span class='status-value'>" + String(CODE_DATE) + "</span></div>";
    html += "<div class='status-item'><span class='status-label'>ESP32 Boot:</span><span class='status-value'>" + bootDateTime + "</span></div>";
    unsigned long uptimeSeconds = millis() / 1000;
    unsigned long hours = uptimeSeconds / 3600;
    unsigned long minutes = (uptimeSeconds % 3600) / 60;
    unsigned long seconds = uptimeSeconds % 60;
    String uptimeStr = String(hours) + "h " + String(minutes) + "m " + String(seconds) + "s";
    html += "<div class='status-item'><span class='status-label'>Uptime:</span><span class='status-value'>" + uptimeStr + "</span></div>";
    html += "<div class='status-item'><span class='status-label'>Flash Size:</span><span class='status-value'>" + String(ESP.getFlashChipSize() / (1024 * 1024)) + " MB</span></div>";
    html += "<div class='status-item'><span class='status-label'>CPU Freq:</span><span class='status-value'>" + String(ESP.getCpuFreqMHz()) + " MHz</span></div>";
    if (chipTempManager && chipTempManager->isInitialized()) {
        float chipTemp = chipTempManager->getTemperature();
        String tempStr = isnan(chipTemp) ? "NA" : String(chipTemp, 1) + "&deg;C";
        html += "<div class='status-item'><span class='status-label'>Chip Temp:</span><span class='status-value'>" + tempStr + "</span></div>";
    }
    html += "</div>";

    html += getLittleFSInfo();

    html += "<div class='info-card'>";
    html += "<h3>NTP Time Synchronization</h3>";
    if (ntpManager) {
        String ntpStatus = ntpManager->isTimeSet() ? "SYNCHRONIZED" : "CONNECTING";
        String ntpStatusClass = ntpManager->isTimeSet() ? "status-good" : "status-warning";
        html += "<div class='status-item'><span class='status-label'>Status:</span><span class='status-value " + ntpStatusClass + "'>" + ntpStatus + "</span></div>";
        html += "<div class='status-item'><span class='status-label'>Current Time:</span><span class='status-value'>" + ntpManager->getFormattedDateTime() + "</span></div>";
        html += "<div class='status-item'><span class='status-label'>Timezone:</span><span class='status-value'>" + settings.ntp.timezone + "</span></div>";

        if (mainController) {
            String timeSource = mainController->getTimeSource();
            String timeSourceClass = (timeSource == "NTP") ? "status-good" : (timeSource == "DS3231") ? "status-warning" : "status-error";
            html += "<div class='status-item'><span class='status-label'>Time Source:</span><span class='status-value " + timeSourceClass + "'>" + timeSource + "</span></div>";

            String rtcHealth;
            String rtcHealthClass;
            if (!mainController->isRtcEnabled()) {
                rtcHealth = "DISABLED";
                rtcHealthClass = "status-warning";
            } else if (!mainController->isRtcInitialized() || !mainController->isRtcPresent()) {
                rtcHealth = "NOT PRESENT";
                rtcHealthClass = "status-error";
            } else if (mainController->rtcHasLostPower()) {
                rtcHealth = "LOST POWER";
                rtcHealthClass = "status-warning";
            } else {
                rtcHealth = "OK";
                rtcHealthClass = "status-good";
            }
            html += "<div class='status-item'><span class='status-label'>RTC Health:</span><span class='status-value " + rtcHealthClass + "'>" + rtcHealth + "</span></div>";

            time_t lastRtcSyncEpoch = mainController->getLastRtcSyncEpoch();
            String lastRtcSyncStr = (lastRtcSyncEpoch > 0) ? mainController->getLastRtcSyncFormattedDateTime() : "Never";
            String lastRtcSyncClass = (lastRtcSyncEpoch > 0) ? "status-good" : "status-warning";
            html += "<div class='status-item'><span class='status-label'>RTC Last Sync:</span><span class='status-value " + lastRtcSyncClass + "'>" + lastRtcSyncStr + "</span></div>";
        }

        time_t lastSyncTime = ntpManager->getLastSyncTime();
        String lastSyncDateTime = "Never";
        if (lastSyncTime > 0) {
            struct tm timeinfo;
            localtime_r(&lastSyncTime, &timeinfo);
            char buffer[20];
            strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &timeinfo);
            lastSyncDateTime = String(buffer);
        }
        String syncStatusClass = ntpManager->wasLastSyncSuccessful() ? "status-good" : "status-error";
        html += "<div class='status-item'><span class='status-label'>Last Sync:</span><span class='status-value " + syncStatusClass + "'>" + lastSyncDateTime + "</span></div>";

        String serverInfo = ntpManager->getLastSyncServer();
        if (serverInfo.length() > 0) {
            html += "<div class='status-item'><span class='status-label'>Server Used:</span><span class='status-value'>" + serverInfo + "</span></div>";
        }
    } else {
        html += "<div class='status-item'><span class='status-label'>Status:</span><span class='status-value status-error'>NOT INITIALIZED</span></div>";
    }
    html += "</div>";

    html += "</div>"; // End info-grid
    return html;
}

void WebServerManager::streamMonitoringHtml(webserver::IResponse& response) {
    const Settings settings = configManager->getSettingsSnapshot();
    HtmlStreamWriter html(response);
    html += "<html><head><title>Monitoring</title><style>";
    html += "body { background-color: #1e1e1e; color: #ffffff; font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif; margin: 0; padding: 60px 20px 20px 20px; }";
    html += "h1 { color: #2196f3; margin-bottom: 30px; }";
    html += "h2 { color: #2196f3; font-size: 1.4em; margin-top: 25px; margin-bottom: 15px; border-bottom: 2px solid #2196f3; padding-bottom: 5px; }";
    html += "h3 { color: #b0b0b0; font-size: 1.1em; margin-top: 20px; margin-bottom: 10px; }";
    html += ".status-card { background: #2d2d2d; border: 1px solid #424242; border-radius: 8px; padding: 15px; }";
    html += ".status-card h4 { margin-top: 0; }";
    html += ".status-card h3 { margin-top: 0; }";
    html += ".status-card { padding-top: 12px; }";
    html += ".status-item { display: flex; justify-content: space-between; align-items: center; margin-bottom: 8px; }";
    html += ".status-label { font-weight: bold; }";
    html += ".status-value { font-family: monospace; font-size: 1.1em; text-align: right; }";
    html += ".status-good { color: #4caf50; }";
    html += ".status-warning { color: #ff9800; }";
    html += ".status-error { color: #f44336; }";
    html += ".error-list { background: #2d2d2d; border: 1px solid #424242; border-radius: 8px; padding: 15px; max-height: 300px; overflow-y: auto; }";
    html += ".error-item { border-left: 4px solid #f44336; padding-left: 10px; margin-bottom: 10px; background: #1a1a1a; padding: 10px; border-radius: 4px; }";
    html += "#activeErrors { font-size: 0.85em; color: #b0b0b0; margin-bottom: 10px; }";
    html += ".config-section { background: #2d2d2d; border: 1px solid #424242; border-radius: 8px; padding: 15px; margin-bottom: 20px; }";
    html += ".button-grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(150px, 1fr)); gap: 10px; }";
    html += "button { background-color: #2196f3; color: white; border: none; padding: 10px 15px; border-radius: 4px; cursor: pointer; font-size: 0.9em; }";
    html += "button:hover { background-color: #1976d2; }";
    html += "button.danger { background-color: #f44336; }";
    html += "button.danger:hover { background-color: #d32f2f; }";
    html += "button.success { background-color: #4caf50; }";
    html += "button.success:hover { background-color: #388e3c; }";
    html += ".api-section { background: #2d2d2d; border: 1px solid #424242; border-radius: 8px; padding: 15px; }";
    html += ".api-section h4 { margin-top: 0; }";
    html += ".api-section h3 { margin-top: 0; }";
    html += ".api-section { padding-top: 12px; }";
    html += ".api-row { display: flex; gap: 20px; }";
    html += ".api-group { flex: 1; }";
    html += ".api-link { display: inline-block; background: #424242; color: #ffffff; text-decoration: none; padding: 8px 12px; margin: 2px; border-radius: 4px; font-family: monospace; font-size: 1.1em; }";
    html += ".api-link:hover { background: #555555; }";
    html += ".status-grid { display: grid; grid-template-columns: repeat(5, 1fr); gap: 12px; margin-bottom: 30px; }";
    html += ".info-grid { display: grid; grid-template-columns: repeat(5, minmax(0, 1fr)); gap: 12px; margin-bottom: 30px; }";
    html += ".grid-header {";
    html += "  color: #2196f3;";
    html += "  font-size: 1.4em;";
    html += "  font-weight: bold;";
    html += "  margin: 0px 0 0px 0;";
    html += "  padding-bottom: 4px;";
    html += "  border-bottom: 2px solid #2196f3;";
    html += "}";
    html += ".error-header { grid-column: 1 / 2; }";
    html += ".system-header { grid-column: 2 / -1; }";
    html += ".info-card { background: #2d2d2d; border: 1px solid #424242; border-radius: 8px; padding: 15px; }";
    html += ".info-card h4 { margin-top: 0; }";
    html += ".info-card h3 { margin-top: 0; }";
    html += ".info-card { padding-top: 12px; }";
    html += ".info-card table { width: 100%; border-collapse: collapse; margin-bottom: 10px; }";
    html += ".info-card th { text-align: left; border-bottom: 1px solid #424242; padding: 5px 0; }";
    html += ".info-card td { border-bottom: 1px solid #2d2d2d; padding: 5px 0; }";
    html += ".info-card td:last-child { text-align: right; }";
    html += ".performance-card { min-width: 0; display: flex; flex-direction: column; }";
    html += ".performance-card .component-performance { overflow-x: auto; }";
    html += ".performance-card .component-performance { flex: 1 1 auto; }";
    html += ".performance-card table { width: 100%; }";
    html += ".performance-card th, .performance-card td { overflow-wrap: anywhere; word-break: break-word; }";
    html += ".performance-actions { margin-top: auto; display: flex; justify-content: flex-start; padding-top: 10px; }";
    html += ".monitoring-grid { display: grid; grid-template-columns: 1fr 1fr; gap: 20px; margin-bottom: 30px; }";
    html += ".monitoring-row { display: flex; gap: 20px; margin-bottom: 30px; }";
    html += ".monitoring-column { flex: 1; }";
    html += ".monitoring-section { background: #2d2d2d; border: 1px solid #424242; border-radius: 8px; padding: 15px; }";
    html += "@media (max-width: 1200px) { .info-grid { grid-template-columns: repeat(3, minmax(0, 1fr)); } }";
    html += "@media (max-width: 900px) { .info-grid { grid-template-columns: repeat(2, minmax(0, 1fr)); } .system-header { grid-column: 1 / -1; } }";
    html += ".progress-bar { width: 100%; height: 20px; background: #424242; border-radius: 10px; overflow: hidden; margin: 5px 0; position: relative; }";
    html += ".progress-fill { height: 100%; background: linear-gradient(90deg, #4caf50, #2196f3); transition: width 0.3s ease; }";
    html += ".progress-text { position: absolute; inset: 0; display: flex; align-items: center; justify-content: center; font-size: 12px; color: #ffffff; text-shadow: 0 1px 2px rgba(0,0,0,0.8); pointer-events: none; }";
    html += ".logger-section { margin-bottom: 20px; }";
    html += ".logger-section h4 { color: #3498db; margin-bottom: 10px; border-bottom: 1px solid #3498db; padding-bottom: 5px; }";
    html += ".logger-section table { width: 100%; border-collapse: collapse; margin-top: 10px; }";
    html += ".logger-section th { background: #3498db; color: white; padding: 8px; text-align: left; }";
    html += ".logger-section td { padding: 6px 8px; border-bottom: 1px solid #34495e; }";
    html += ".logger-section tr:hover { background: #34495e; }";
    html += "</style></head><body>" + getNavHtml("monitoring");

    html += "<div id='monitoringCardsRoot'>";
    html += generateMonitoringCardsHtml(settings);
    html += "</div>";

    // API Endpoints Section
    html += "<h2>API Endpoints</h2>";
    html += "<div class='api-section'>";
    html += "<div class='api-row'>";
    html += "<div class='api-group'>";
    html += "<h3>Configuration APIs</h3>";
    html += "<a href='/api/config' class='api-link'>GET /api/config</a>";
    html += "<a href='/api/settings' class='api-link'>GET /api/settings</a>";
    html += "</div>";

    html += "<div class='api-group'>";
    html += "<h3>NTP APIs</h3>";
    html += "<a href='/api/ntp/status' class='api-link'>GET /api/ntp/status</a>";
    html += "<a href='/api/ntp/logs' class='api-link'>GET /api/ntp/logs</a>";
    html += "</div>";

    html += "<div class='api-group'>";
    html += "<h3>Relay APIs</h3>";
    html += "<a href='/api/relay/status' class='api-link'>GET /api/relay/status</a>";
    html += "<a href='/api/relay/set' class='api-link'>POST /api/relay/set</a>";
    html += "<a href='/api/relay/logs' class='api-link'>GET /api/relay/logs</a>";
    html += "<form method='POST' style='display: inline; margin-left: 10px;'>";
    html += "<input type='hidden' name='action' value='clear_relay_logs'>";
    html += "<button type='submit' class='danger' style='font-size: 0.8em; padding: 4px 8px;'>Clear Logs</button>";
    html += "</form>";
    html += "</div>";


    html += "<div class='api-group'>";
    html += "<h3>Error & Monitoring APIs</h3>";
    html += "<a href='/api/errors' class='api-link'>GET /api/errors</a>";
    html += "<a href='/api/errors/logs' class='api-link'>GET /api/errors/logs</a>";
    html += "<form method='POST' style='display: inline; margin-left: 10px;'>";
    html += "<input type='hidden' name='action' value='clear_error_logs'>";
    html += "<button type='submit' class='danger' style='font-size: 0.8em; padding: 4px 8px;'>Clear Logs</button>";
    html += "</form>";
    html += "</div>";
    html += "</div>";
    html += "</div>";

    // JavaScript for interactivity
    html += "<script>";
    html += "const MONITORING_POLL_MS = 30000;";
    html += "let monitoringPollInFlight = false;";
    html += "function formatTimeAgo(timestamp,currentTime){if(!timestamp)return'Never';const diff=currentTime-timestamp;const seconds=Math.floor(diff/1000);if(seconds<60)return seconds+'s ago';const minutes=Math.floor(seconds/60);if(minutes<60)return minutes+'m ago';const hours=Math.floor(minutes/60);return hours+'h ago';}";
    html += "function updateErrors(){fetch('/api/errors',{credentials:'same-origin',cache:'no-store'}).then(response=>{if(!response.ok){throw new Error('HTTP '+response.status+': '+response.statusText);}return response.json();}).then(data=>{const activeErrorsDiv=document.getElementById('activeErrors');const totalErrorsDiv=document.getElementById('totalErrors');const errorsLastHourDiv=document.getElementById('errorsLastHour');const lastErrorTimeDiv=document.getElementById('lastErrorTime');if(!activeErrorsDiv||!totalErrorsDiv||!errorsLastHourDiv||!lastErrorTimeDiv){return;}const currentTime=data.timestamp||0;if(data.active_errors&&data.active_errors.length>0){let html='';data.active_errors.forEach(error=>{const datetimeDisplay=error.last_occurrence_datetime&&error.last_occurrence_datetime!=='NTP not synchronized'&&error.last_occurrence_datetime!=='Unknown (pre-NTP data)'?error.last_occurrence_datetime:formatTimeAgo(error.last_occurrence,currentTime);html+='<div class=\"error-item\"><strong>'+error.code+'</strong>: '+error.message+'<br><small>Count: '+error.count+', Last: '+datetimeDisplay+'</small></div>';});activeErrorsDiv.innerHTML=html;}else{activeErrorsDiv.innerHTML='No active errors';}totalErrorsDiv.textContent=data.total_errors||0;errorsLastHourDiv.textContent=data.errors_last_hour||0;const lastErrorDatetime=data.last_error_datetime&&data.last_error_datetime!=='NTP not synchronized'&&data.last_error_datetime!=='Unknown (pre-NTP data)'?data.last_error_datetime:formatTimeAgo(data.last_error_time,currentTime);lastErrorTimeDiv.textContent=lastErrorDatetime;}).catch(error=>{console.log('Error updating errors:',error);const activeErrorsDiv=document.getElementById('activeErrors');if(activeErrorsDiv){activeErrorsDiv.innerHTML='<div class=\"error-item\" style=\"border-left-color: #f44336;\">Error loading error data: '+error.message+'</div>';}});}";
    html += "async function refreshMonitoringCards(){if(monitoringPollInFlight||document.visibilityState!=='visible')return;monitoringPollInFlight=true;try{const response=await fetch('/api/monitoring/cards?t='+Date.now(),{credentials:'same-origin',cache:'no-store'});if(!response.ok){throw new Error('HTTP '+response.status+': '+response.statusText);}const cardsHtml=await response.text();const root=document.getElementById('monitoringCardsRoot');if(root){root.innerHTML=cardsHtml;}updateErrors();}catch(error){console.log('Error updating monitoring cards:',error);}finally{monitoringPollInFlight=false;}}";
    html += "updateErrors();";
    html += "setInterval(()=>{refreshMonitoringCards().catch(()=>{});}, MONITORING_POLL_MS);";
    html += "document.addEventListener('visibilitychange',()=>{if(document.visibilityState==='visible'){refreshMonitoringCards().catch(()=>{});}});";
    html += "</script>";

    html += "</body></html>";
    html.finish();
}

void WebServerManager::handleApiSettingsGet(webserver::IRequest& request, webserver::IResponse& response) {
    if (!checkAuthentication(request, response)) {
        return; // Authentication failed, response already sent
    }

    if (!configManager) {
        response.send(400, kContentTypeJsonUtf8, "{\"error\":\"Configuration manager not set\"}");
        return;
    }

    String json = configManager->getSettingsJson();

    response.send(200, kContentTypeJsonUtf8, json);
}

void WebServerManager::handleApiSettingsPost(webserver::IRequest& request, webserver::IResponse& response) {
    if (!checkAuthentication(request, response)) {
        return; // Authentication failed, response already sent
    }


    if (!configManager) {
        response.send(400, kContentTypeJsonUtf8, "{\"error\":\"Config manager not set\"}");
        return;
    }

    if (!mainController) {
        response.send(500, kContentTypeJsonUtf8, "{\"error\":\"Main controller not set\"}");
        return;
    }

    if (request.hasArg("plain")) {
        String body = request.body();

        const webserver::ControlDispatchWaitResult dispatchResult =
            webserver::dispatchSettingsUpdateAndWait(mainController, body);

        if (dispatchResult.status == webserver::ControlDispatchWaitStatus::ControllerUnavailable) {
            response.send(500, kContentTypeJsonUtf8, "{\"error\":\"Settings update unavailable\"}");
            return;
        }

        if (dispatchResult.status == webserver::ControlDispatchWaitStatus::EnqueueRejected) {
            response.send(503, kContentTypeJsonUtf8, "{\"error\":\"Settings update busy, try again\"}");
            return;
        }

        if (dispatchResult.status == webserver::ControlDispatchWaitStatus::TimedOut) {
            response.send(503, kContentTypeJsonUtf8, "{\"error\":\"Settings update timed out, try again\"}");
            return;
        }

        if (dispatchResult.status != webserver::ControlDispatchWaitStatus::Completed) {
            response.send(500, kContentTypeJsonUtf8, "{\"error\":\"Settings update failed\"}");
            return;
        }

        const bool updateResult =
            dispatchResult.commandResult.status == MainController::ControlCommandStatus::SUCCEEDED;

        if (updateResult) {
            // Configuration updated successfully
            // NOTE: updateSettingsFromJson() already validates and persists the configuration.
            response.send(200, kContentTypeJsonUtf8, "{\"status\":\"Settings saved, rebooting ESP32...\"}");
            scheduleRestart();
        } else {
            if (dispatchResult.commandResult.errorCode != -6) {
                response.send(500, kContentTypeJsonUtf8, "{\"error\":\"Settings update failed\"}");
                return;
            }

            // Get the specific validation errors
            String validationErrors = configManager->getValidationErrors();
            if (validationErrors.length() > 0) {
                // Escape newlines and quotes for JSON
                String escapedErrors = validationErrors;
                escapedErrors.replace("\n", "\\n");
                escapedErrors.replace("\"", "\\\"");

                // Return specific validation errors to the client
                String errorJson = "{\"error\":\"Configuration validation failed: " + escapedErrors + "\"}";
                response.send(400, kContentTypeJsonUtf8, errorJson);
            } else {
                // Fallback to generic error
                ErrorHandler::reportError(ErrorCode::CONFIG_VALIDATION_FAILED, ErrorSeverity::ERROR,
                                            "Failed to validate and save new settings");
                response.send(400, kContentTypeJsonUtf8, "{\"error\":\"Failed to update settings\"}");
            }
        }
    } else {
        response.send(400, kContentTypeJsonUtf8, "{\"error\":\"No body\"}");
    }
}
void WebServerManager::handleApiReboot(webserver::IRequest& request, webserver::IResponse& response) {
    if (!checkAuthentication(request, response)) {
        return; // Authentication failed, response already sent
    }

    response.send(200, kContentTypeJsonUtf8, "{\"status\":\"Rebooting ESP32...\"}");
    scheduleRestart();
}
void WebServerManager::handleApiConfigGet(webserver::IRequest& request, webserver::IResponse& response) {
    if (!checkAuthentication(request, response)) {
        return; // Authentication failed, response already sent
    }

    File meta = LittleFS.open("/config.json", "r");
    if (meta) {
        const size_t fileSize = meta.size();
        meta.close();

        const size_t bytesSent = response.sendFile(LittleFS, "/config.json", kContentTypeJsonUtf8);

        if (bytesSent != fileSize) {
        }
    } else {
        response.send(404, kContentTypeJsonUtf8, "{\"error\":\"Config file not found\"}");
    }
}

void WebServerManager::handleScan(webserver::IRequest& request, webserver::IResponse& response) {
    if (!checkAuthentication(request, response)) {
        return; // Authentication failed, response already sent
    }

    if (!ds18b20Manager) {
        response.send(400, kContentTypeJsonUtf8, "{\"error\":\"Sensor manager not set\"}");
        return;
    }

    const String mode = request.arg("mode");
    if (mode != "configured" && mode != "full") {
        response.send(400, kContentTypeJsonUtf8, "{\"error\":\"Invalid scan mode. Use 'configured' or 'full'\"}");
        return;
    }

    const bool fullScan = (mode == "full");
    unsigned long now = millis();

    // Return completed result for matching mode.
    if (ds18b20Manager->hasScanResultReady() &&
        ds18b20Manager->isLastScanResultFullMode() == fullScan) {
        String result = ds18b20Manager->consumeScanResult();
        response.send(200, kContentTypeJsonUtf8, result);
        return;
    }

    if (ds18b20Manager->isScanBusy()) {
        const char* status = ds18b20Manager->isScanInProgress() ? "in_progress" : "queued";
        String json = "{\"status\":\"";
        json += status;
        json += "\",\"mode\":\"";
        json += fullScan ? "full" : "configured";
        json += "\"}";
        response.send(202, kContentTypeJsonUtf8, json);
        return;
    }

    const unsigned long cooldownMs = fullScan ? FULL_SCAN_COOLDOWN_MS : SCAN_COOLDOWN_MS;
    if (lastScanStartedMs != 0) {
        const unsigned long elapsed = now - lastScanStartedMs;
        if (elapsed < cooldownMs) {
            const unsigned long retryAfterMs = cooldownMs - elapsed;
            String json = "{\"error\":\"Scan cooldown active\",\"retry_after_ms\":" + String(retryAfterMs) + "}";
            response.send(429, kContentTypeJsonUtf8, json);
            return;
        }
    }

    // Discard stale result from previous mode before queuing a new request.
    if (ds18b20Manager->hasScanResultReady()) {
        MainController::ControlCommand clearCommand;
        clearCommand.type = MainController::ControlCommandType::DS18_CLEAR_SCAN_RESULT;
        const webserver::ControlDispatchWaitResult clearResult =
            webserver::dispatchControlCommandAndWait(mainController, clearCommand);

        if (clearResult.status == webserver::ControlDispatchWaitStatus::ControllerUnavailable) {
            response.send(500, kContentTypeJsonUtf8, "{\"error\":\"Main controller not set\"}");
            return;
        }
        if (clearResult.status == webserver::ControlDispatchWaitStatus::EnqueueRejected) {
            response.send(503, kContentTypeJsonUtf8, "{\"error\":\"Scan command busy, try again\"}");
            return;
        }
        if (clearResult.status == webserver::ControlDispatchWaitStatus::TimedOut) {
            response.send(503, kContentTypeJsonUtf8, "{\"error\":\"Scan command timed out, try again\"}");
            return;
        }
        if (clearResult.status != webserver::ControlDispatchWaitStatus::Completed ||
            clearResult.commandResult.status != MainController::ControlCommandStatus::SUCCEEDED) {
            response.send(500, kContentTypeJsonUtf8, "{\"error\":\"Failed to clear stale scan result\"}");
            return;
        }
    }

    MainController::ControlCommand scanCommand;
    scanCommand.type = MainController::ControlCommandType::DS18_REQUEST_SCAN;
    scanCommand.arg0 = fullScan ? 1 : 0;
    const webserver::ControlDispatchWaitResult scanResult =
        webserver::dispatchControlCommandAndWait(mainController, scanCommand);

    if (scanResult.status == webserver::ControlDispatchWaitStatus::ControllerUnavailable) {
        response.send(500, kContentTypeJsonUtf8, "{\"error\":\"Main controller not set\"}");
        return;
    }
    if (scanResult.status == webserver::ControlDispatchWaitStatus::EnqueueRejected) {
        response.send(503, kContentTypeJsonUtf8, "{\"error\":\"Scan command busy, try again\"}");
        return;
    }
    if (scanResult.status == webserver::ControlDispatchWaitStatus::TimedOut) {
        response.send(503, kContentTypeJsonUtf8, "{\"error\":\"Scan command timed out, try again\"}");
        return;
    }
    if (scanResult.status != webserver::ControlDispatchWaitStatus::Completed) {
        response.send(500, kContentTypeJsonUtf8, "{\"error\":\"Scan request failed\"}");
        return;
    }

    if (scanResult.commandResult.status != MainController::ControlCommandStatus::SUCCEEDED) {
        response.send(409, kContentTypeJsonUtf8, "{\"error\":\"Scan already in progress\"}");
        return;
    }

    lastScanStartedMs = now;
    String json = "{\"status\":\"queued\",\"mode\":\"";
    json += fullScan ? "full" : "configured";
    json += "\"}";
    response.send(202, kContentTypeJsonUtf8, json);
}
void WebServerManager::handleApiErrorsGet(webserver::IRequest& request, webserver::IResponse& response) {
    if (!checkAuthentication(request, response)) {
        return; // Authentication failed, response already sent
    }

    // Return active error state (original functionality)
    PsramJsonAllocator jsonAllocator;
    JsonDocument doc(&jsonAllocator);
    doc["timestamp"] = millis();
    
    // Add active errors
    std::vector<ErrorEntry> activeErrors = ErrorHandler::getActiveErrors();
    JsonArray errorsArray = doc["active_errors"].to<JsonArray>();
    
    for (const auto& error : activeErrors) {
        JsonObject errorObj = errorsArray.add<JsonObject>();
        errorObj["code"] = static_cast<int>(error.code);
        errorObj["code_name"] = ErrorHandler::errorCodeToString(error.code);
        errorObj["severity"] = ErrorHandler::severityToString(error.severity); 
        errorObj["message"] = error.message; 
        if (!error.context.isEmpty()) { 
            errorObj["context"] = error.context; 
        } 
        errorObj["count"] = error.occurrenceCount; 
        errorObj["first_occurrence"] = error.timestamp; 
        errorObj["first_occurrence_datetime"] = error.timestampDateTime; 
        errorObj["last_occurrence"] = error.lastOccurrence; 
        errorObj["last_occurrence_datetime"] = error.lastOccurrenceDateTime;
        errorObj["resolved"] = error.resolved;
    }
    
    // Add statistics
    doc["total_errors"] = ErrorHandler::getTotalErrorCount();
    doc["errors_last_hour"] = ErrorHandler::getErrorsInLastHour();
    doc["last_error_time"] = ErrorHandler::getLastErrorTime();
    
    // Add last error datetime if available
    unsigned long lastErrorTime = ErrorHandler::getLastErrorTime();
    if (lastErrorTime > 0) {
        // Try to find the actual datetime from active errors
        String lastErrorDatetime = "Unknown";
        for (const auto& error : activeErrors) {
            if (error.lastOccurrence == lastErrorTime && !error.lastOccurrenceDateTime.isEmpty()) {
                lastErrorDatetime = error.lastOccurrenceDateTime;
                break;
            }
        }
        doc["last_error_datetime"] = lastErrorDatetime;
    } else {
        doc["last_error_datetime"] = "Never";
    }
    
    String jsonString;
    serializeJson(doc, jsonString);
    response.send(200, kContentTypeJsonUtf8, jsonString);
}

// NEW: Error History API endpoint
void WebServerManager::handleApiErrorHistoryGet(webserver::IRequest& request, webserver::IResponse& response) {
    if (!checkAuthentication(request, response)) {
        return; // Authentication failed, response already sent
    }

    // Get error history from ErrorHandler
    String historyJson = ErrorHandler::getErrorHistoryJson();
    response.send(200, kContentTypeJsonUtf8, historyJson);
}

bool WebServerManager::checkAuthentication(webserver::IRequest& request, webserver::IResponse& response) {
    // Check IP whitelist first (NEW)
    if (!checkIPWhitelist(request, response)) {
        return false; // Response already sent (404)
    }

    if (!authEnabled || !configManager) {
        return true; // No authentication required
    }

    // For HTTP Basic Auth, we need to use the plain text password
    // The configManager provides the plain text version for HTTP auth
    String plainPassword = configManager->getPlainTextAuthPassword();
    const Settings authSettings = configManager->getSettingsSnapshot();
    if (plainPassword.length() == 0) {
        // If no plain text password available, try the hashed version (backward compatibility)
        plainPassword = authSettings.security.password_hash;
    }

    if (!response.authenticate(authSettings.security.username.c_str(),
                               plainPassword.c_str())) {
        response.requestAuthentication();
        return false;
    }
    return true;
}

String WebServerManager::generateAuthRealm() {
    return "ESP32 Sensor Monitor";
}

bool WebServerManager::checkIPWhitelist(webserver::IRequest& request, webserver::IResponse& response) {
    String endpoint = request.uri();
    const bool verbose = !endpoint.startsWith("/api/debug/");
    if (verbose) {
    }

    if (!ipWhitelistManager.isEnabled()) {
        if (verbose) {
        }
        return true; // Allow if disabled
    }
    if (verbose) {
    }

    // Get client IP
    IPAddress clientIP = request.remoteIP();
    String clientIPStr = clientIP.toString();
    if (verbose) {
    }

    if (!ipWhitelistManager.isIPWhitelisted(clientIP)) {
        // Log blocked access attempt
        if (verbose) {
        }

        ErrorHandler::reportError(
            ErrorCode::IP_WHITELIST_ACCESS_DENIED,
            ErrorSeverity::WARNING,
            "IP blocked by whitelist: " + clientIPStr + " accessing " + endpoint
        );

        // Return 404 to obsfucate service
        response.send(404, kContentTypeHtmlUtf8,
            "<html><body><h1>404 Not Found</h1><p>The requested resource was not found.</p></body></html>");
        return false;
    }
    if (verbose) {
    }

    return true;
}
void WebServerManager::handleApiNtpStatus(webserver::IRequest& request, webserver::IResponse& response) {

    if (!checkAuthentication(request, response)) {
        return; // Authentication failed, response already sent
    }

    if (!isWiFiConnected()) {
        response.send(503, kContentTypeJsonUtf8, "{\"error\":\"WiFi connection lost\"}");
        return;
    }

    if (!ntpManager) {
        response.send(500, kContentTypeJsonUtf8, "{\"error\":\"NTP manager not initialized\"}");
        return;
    }

    // Build NTP status response
    String json = "{";
    json += "\"status\":\"" + String(ntpManager->isTimeSet() ? "SYNCHRONIZED" : "CONNECTING") + "\",";
    json += "\"current_time\":\"" + ntpManager->getFormattedDateTime() + "\",";
    const Settings ntpSettings = configManager->getSettingsSnapshot();
    json += "\"timezone\":\"" + ntpSettings.ntp.timezone + "\",";

    // RTC / time source information
    if (mainController) {
        json += "\"time_source\":\"" + mainController->getTimeSource() + "\",";
        json += "\"rtc\":{";
        json += "\"enabled\":" + String(mainController->isRtcEnabled() ? "true" : "false") + ",";
        json += "\"initialized\":" + String(mainController->isRtcInitialized() ? "true" : "false") + ",";
        json += "\"present\":" + String(mainController->isRtcPresent() ? "true" : "false") + ",";
        json += "\"lost_power\":" + String(mainController->rtcHasLostPower() ? "true" : "false") + ",";
        json += "\"rtc_time\":\"" + mainController->getRtcFormattedDateTime() + "\",";
        json += "\"last_rtc_sync\":\"" + mainController->getLastRtcSyncFormattedDateTime() + "\"";
        json += "},";
    }
    
    // Format last_sync with datetime for successful syncs (like handleApiNtpLogs does)
    String lastSyncValue;
    if (ntpManager->wasLastSyncSuccessful()) {
        time_t lastSyncTime = ntpManager->getLastSyncTime();
        if (lastSyncTime > 0) {
            struct tm timeinfo;
            localtime_r(&lastSyncTime, &timeinfo);
            char buffer[20];  // YYYY-MM-DD HH:MM:SS + null
            strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &timeinfo);
            lastSyncValue = "Sync successful - " + String(buffer);
        } else {
            lastSyncValue = "Sync successful - Unknown time";
        }
    } else {
        lastSyncValue = ntpManager->getLastSyncDetailedStatus();
    }
    json += "\"last_sync\":\"" + lastSyncValue + "\",";
    
    json += "\"server_used\":\"" + ntpManager->getLastSyncServer() + "\",";
    json += "\"sync_successful\":" + String(ntpManager->wasLastSyncSuccessful() ? "true" : "false");
    json += "}";

    response.send(200, kContentTypeJsonUtf8, json);
}

void WebServerManager::handleApiNtpLogs(webserver::IRequest& request, webserver::IResponse& response) {

    if (!checkAuthentication(request, response)) {
        return; // Authentication failed, response already sent
    }

    if (!isWiFiConnected()) {
        response.send(503, kContentTypeJsonUtf8, "{\"error\":\"WiFi connection lost\"}");
        return;
    }

    if (!ntpManager) {
        response.send(500, kContentTypeJsonUtf8, "{\"error\":\"NTP manager not initialized\"}");
        return;
    }
    
    // Get actual NTP logs from NTPManager's circular buffer
    String logsJson = ntpManager->getNtpLogsForApi();
    
    // Add additional NTP status information to maintain backward compatibility
    String lastSyncValue;
    if (ntpManager->wasLastSyncSuccessful()) {
        time_t lastSyncTime = ntpManager->getLastSyncTime();
        if (lastSyncTime > 0) {
            struct tm timeinfo;
            localtime_r(&lastSyncTime, &timeinfo);
            char buffer[20];  // YYYY-MM-DD HH:MM:SS + null
            strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &timeinfo);
            lastSyncValue = "Sync successful - " + String(buffer);
        } else {
            lastSyncValue = "Sync successful - Unknown time";
        }
    } else {
        lastSyncValue = ntpManager->getLastSyncDetailedStatus();
    }
    
    // Parse the logs JSON and add additional fields
    PsramJsonAllocator jsonAllocator;
    JsonDocument doc(&jsonAllocator);
    DeserializationError error = deserializeJson(doc, logsJson);
    
    if (error) {
        response.send(500, kContentTypeJsonUtf8, "{\"error\":\"Failed to process NTP logs\"}");
        return;
    }
    
    // Add additional fields for backward compatibility
    doc["last_sync"] = lastSyncValue;
    doc["server_used"] = ntpManager->getLastSyncServer();
    doc["sync_successful"] = ntpManager->wasLastSyncSuccessful();
    
    // Serialize the complete JSON response
    String jsonResponse;
    serializeJson(doc, jsonResponse);
    
    response.send(200, kContentTypeJsonUtf8, jsonResponse);
}

void WebServerManager::handleRelay(webserver::IRequest& request, webserver::IResponse& response) {
    if (!checkAuthentication(request, response)) {
        return; // Authentication failed, response already sent
    }

    if (!isWiFiConnected()) {
        response.send(503, kContentTypeHtmlUtf8, "<html><body><h1>Service Unavailable</h1><p>WiFi connection lost. Please check network connectivity.</p></body></html>");
        return;
    }

    response.setContentLength(webserver::kContentLengthUnknown);
    response.send(200, kContentTypeHtmlUtf8, "");
    streamRelayHtml(response);
    response.sendContent("");
}

namespace {
constexpr uint32_t kDebugTtlMs = 15000;
constexpr uint32_t kDebugMaxApiBytes = 8192;
constexpr size_t kDebugMaxLines = 50;

void appendJsonEscaped(String& out, const char* s, size_t len) {
    if (!s || len == 0) {
        return;
    }

    for (size_t i = 0; i < len; i++) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[7];
                    snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
                break;
        }
    }
}

String formatTimeHms(uint32_t epochSeconds) {
    time_t t = static_cast<time_t>(epochSeconds);
    struct tm tmInfo;
    localtime_r(&t, &tmInfo);
    char buf[9];
    strftime(buf, sizeof(buf), "%H:%M:%S", &tmInfo);
    return String(buf);
}

String formatMillis(uint32_t ms) {
    return String("+") + String(ms) + "ms";
}
} // namespace

void WebServerManager::handleDebug(webserver::IRequest& request, webserver::IResponse& response) {
    if (!checkAuthentication(request, response)) {
        return; // Authentication failed, response already sent
    }

    if (!isWiFiConnected()) {
        response.send(503, kContentTypeHtmlUtf8,
                    "<html><body><h1>Service Unavailable</h1><p>WiFi connection lost. Please check network connectivity.</p></body></html>");
        return;
    }

    const bool serialDebugEnabled = configManager && configManager->isConfigLoaded()
                                       ? configManager->getSettingsSnapshot().debug.logging_enabled
                                       : false;

    response.setContentLength(webserver::kContentLengthUnknown);
    response.send(200, kContentTypeHtmlUtf8, "");
    HtmlStreamWriter html(response);
    html += "<!doctype html><html><head><meta charset='utf-8'>";
    html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
    html += "<title>Debug</title>";
    html += "<style>";
    html += "body{background:#1e1e1e;color:#fff;font-family:'Segoe UI',Tahoma,Geneva,Verdana,sans-serif;margin:0;padding:60px 20px 20px 20px;}";
    html += "h1{color:#2196f3;margin:0 0 10px 0;}";
    html += ".row{display:flex;gap:10px;flex-wrap:wrap;align-items:center;margin:10px 0 12px 0;}";
    html += ".btn{padding:10px 18px;font-size:1.0em;font-weight:bold;border-radius:4px;border:0;cursor:pointer;color:white;";
    html += "background:linear-gradient(135deg,#666,#555);box-shadow:0 4px 8px rgba(0,0,0,0.3),inset 0 1px 0 rgba(255,255,255,0.2);}";
    html += ".btn:hover{transform:translateY(-1px);box-shadow:0 6px 12px rgba(0,0,0,0.4),inset 0 1px 0 rgba(255,255,255,0.3);background:linear-gradient(135deg,#777,#666);}";
    html += ".btn.danger{background:linear-gradient(135deg,#d32f2f,#b71c1c);}";
    html += ".btn.primary{background:linear-gradient(135deg,#2196f3,#1976d2);}";
    html += ".muted{color:#b0b0b0;}";
    html += ".card{background:#2d2d2d;border:1px solid #424242;border-radius:8px;padding:12px;margin:10px 0;}";
    html += "#log{background:#111;border:1px solid #333;border-radius:6px;padding:10px;white-space:pre-wrap;word-break:break-word;";
    html += "font-family:Consolas,monospace;font-size:12px;line-height:1.4;max-height:70vh;overflow:auto;}";
    html += ".status{font-size:0.95em}";
    html += ".ok{color:#81c784}.warn{color:#ffb74d}.err{color:#e57373}";
    html += "label{display:inline-flex;gap:6px;align-items:center}";
    html += "</style></head><body>";

    html += getNavHtml("debug");
    html += "<h1>Debug</h1>";

    html += "<div class='card status'>";
    if (serialDebugEnabled) {
        html += "<span class='ok'>Serial debug logging enabled</span> <span class='muted'>(Settings &rarr; Debug)</span>";
    } else {
        html += "<span class='warn'>Serial debug logging disabled</span> <span class='muted'>(enable in Settings &rarr; Debug to see logs)</span>";
    }
    html += "<div id='statusLine' class='muted' style='margin-top:6px'>Idle</div>";
    html += "</div>";

    html += "<div class='row'>";
    html += "<button id='btnPause' class='btn'>Pause</button>";
    html += "<button id='btnClear' class='btn danger'>Clear</button>";
    html += "<label class='muted'><input id='chkScroll' type='checkbox' checked>Auto-scroll</label>";
    html += "</div>";

    html += "<div id='log' aria-label='Debug log output'></div>";

    html += "<script>";
    html += "(() => {";
    html += "const POLL_MS=1000, KEEPALIVE_MS=5000, TTL_MS=15000, MAX_BYTES=8192;";
    html += "const CLIENT_WINDOW_MS=5*60*1000, CLIENT_MAX_BYTES=2*1024*1024;";
    html += "let since=0, paused=false, autoScroll=true;";
    html += "let lines=[], totalBytes=0;";
    html += "const logEl=document.getElementById('log');";
    html += "const statusEl=document.getElementById('statusLine');";
    html += "const btnPause=document.getElementById('btnPause');";
    html += "const btnClear=document.getElementById('btnClear');";
    html += "const chkScroll=document.getElementById('chkScroll');";

    html += "function setStatus(text, cls){statusEl.textContent=text; statusEl.className=cls?(''+cls):'muted';}";
    html += "function rebuild(){logEl.textContent=lines.map(x=>x.text).join('\\n') + (lines.length?'\\n':''); if(autoScroll) logEl.scrollTop=logEl.scrollHeight;}";
    html += "function pushLine(text){const now=Date.now(); lines.push({t:now,text}); totalBytes += text.length+1; logEl.textContent += text+'\\n';}";
    html += "function prune(){const cutoff=Date.now()-CLIENT_WINDOW_MS; let changed=false; while(lines.length && (lines[0].t<cutoff || totalBytes>CLIENT_MAX_BYTES)){totalBytes -= (lines[0].text.length+1); lines.shift(); changed=true;} if(changed) rebuild();}";

    html += "async function postJson(url){const r=await fetch(url,{method:'POST'}); let j=null; try{j=await r.json();}catch(e){} return {ok:r.ok,status:r.status,json:j};}";
    html += "async function getJson(url){const r=await fetch(url); let j=null; try{j=await r.json();}catch(e){} return {ok:r.ok,status:r.status,json:j};}";

    html += "async function enable(){";
    html += "  const {ok,status,json}=await postJson('/api/debug/enable');";
    html += "  if(ok){setStatus('Viewer active (TTL '+(json?.ttl_ms??TTL_MS)+'ms)', 'ok'); return true;}";
    html += "  if(status===409){const rem=Math.ceil(((json?.remaining_ms)||0)/1000); setStatus('Debug viewer already in use. Try again in ~'+rem+'s.', 'warn'); return false;}";
    html += "  setStatus('Failed to enable debug viewer ('+status+')', 'err'); return false;";
    html += "}";

    html += "async function clearServer(){const {ok,status}=await postJson('/api/debug/clear'); if(!ok){setStatus('Clear failed ('+status+')','err'); return false;} return true;}";

    html += "async function poll(){";
    html += "  if(paused) return;";
    html += "  const url='/api/debug/logs?since='+encodeURIComponent(since)+'&max_bytes='+MAX_BYTES;";
    html += "  const {ok,status,json}=await getJson(url);";
    html += "  if(!ok){";
    html += "    if(status===403){setStatus('Viewer expired. Re-enabling...', 'warn'); if(await enable()){since=0; lines=[]; totalBytes=0; logEl.textContent='';} return;}";
    html += "    if(status===409){setStatus('Viewer busy. Retry later.', 'warn'); return;}";
    html += "    setStatus('Poll failed ('+status+')', 'err'); return;";
    html += "  }";
    html += "  const arr=json?.lines||[];";
    html += "  if(json?.dropped){pushLine('[WARN] dropped buffered logs; jumping to latest');}";
    html += "  for(const it of arr){";
    html += "    const t=it.t||''; const msg=it.msg||''; const line='['+t+'] '+msg;";
    html += "    pushLine(line);";
    html += "    since = Math.max(since, (it.id||0));";
    html += "  }";
    html += "  prune();";
    html += "  if(autoScroll) logEl.scrollTop=logEl.scrollHeight;";
    html += "}";

    html += "btnPause.addEventListener('click',()=>{paused=!paused; btnPause.textContent=paused?'Resume':'Pause'; setStatus(paused?'Paused':'Viewer active','muted');});";
    html += "btnClear.addEventListener('click',async()=>{if(await clearServer()){since=0; lines=[]; totalBytes=0; logEl.textContent=''; setStatus('Cleared','muted');}});";
    html += "chkScroll.addEventListener('change',()=>{autoScroll=!!chkScroll.checked; if(autoScroll) logEl.scrollTop=logEl.scrollHeight;});";

    html += "(async()=>{";
    html += "  if(!(await enable())) return;";
    html += "  setInterval(()=>{postJson('/api/debug/enable').catch(()=>{});}, KEEPALIVE_MS);";
    html += "  setInterval(()=>{poll().catch(()=>{});}, POLL_MS);";
    html += "  poll().catch(()=>{});";
    html += "})();";
    html += "})();";
    html += "</script>";

    html += "</body></html>";
    html.finish();
    response.sendContent("");
}

void WebServerManager::handleApiDebugEnable(webserver::IRequest& request, webserver::IResponse& response) {
    if (!checkAuthentication(request, response)) {
        return; // Authentication failed, response already sent
    }

    if (!isWiFiConnected()) {
        response.send(503, kContentTypeJsonUtf8, "{\"error\":\"WiFi connection lost\"}");
        return;
    }

    const IPAddress clientIP = request.remoteIP();
    uint32_t remainingMs = 0;
    const DebugLogBuffer::EnableResult res = DebugLogBuffer::enableFor(clientIP, kDebugTtlMs, remainingMs);
    if (res != DebugLogBuffer::EnableResult::Ok) {
        if (res == DebugLogBuffer::EnableResult::Busy) {
            String json = "{\"error\":\"busy\",\"remaining_ms\":" + String(remainingMs) + "}";
            response.send(409, kContentTypeJsonUtf8, json);
            return;
        }
        response.send(500, kContentTypeJsonUtf8, "{\"error\":\"no_mem\"}");
        return;
    }

    String json = "{\"ok\":true,\"ttl_ms\":" + String(kDebugTtlMs) + ",\"poll_ms\":1000,\"max_bytes\":" + String(kDebugMaxApiBytes) + "}";
    response.send(200, kContentTypeJsonUtf8, json);
}
void WebServerManager::handleApiDebugClear(webserver::IRequest& request, webserver::IResponse& response) {
    if (!checkAuthentication(request, response)) {
        return; // Authentication failed, response already sent
    }

    if (!isWiFiConnected()) {
        response.send(503, kContentTypeJsonUtf8, "{\"error\":\"WiFi connection lost\"}");
        return;
    }

    const IPAddress clientIP = request.remoteIP();
    if (!DebugLogBuffer::isEnabled()) {
        response.send(403, kContentTypeJsonUtf8, "{\"error\":\"not_enabled\"}");
        return;
    }
    if (!DebugLogBuffer::isOwner(clientIP)) {
        response.send(409, kContentTypeJsonUtf8, "{\"error\":\"busy\"}");
        return;
    }

    DebugLogBuffer::clear();
    response.send(200, kContentTypeJsonUtf8, "{\"ok\":true}");
}
void WebServerManager::handleApiDebugLogs(webserver::IRequest& request, webserver::IResponse& response) {
    if (!checkAuthentication(request, response)) {
        return; // Authentication failed, response already sent
    }

    if (!isWiFiConnected()) {
        response.send(503, kContentTypeJsonUtf8, "{\"error\":\"WiFi connection lost\"}");
        return;
    }

    const IPAddress clientIP = request.remoteIP();
    if (!DebugLogBuffer::isEnabled()) {
        response.send(403, kContentTypeJsonUtf8, "{\"error\":\"not_enabled\"}");
        return;
    }
    if (!DebugLogBuffer::isOwner(clientIP)) {
        response.send(409, kContentTypeJsonUtf8, "{\"error\":\"busy\"}");
        return;
    }

    // Treat polling as activity (page open) and extend TTL.
    (void)DebugLogBuffer::keepAlive(clientIP, kDebugTtlMs);

    uint32_t since = 0;
    if (request.hasArg("since")) {
        since = static_cast<uint32_t>(request.arg("since").toInt());
    }

    uint32_t maxBytes = kDebugMaxApiBytes;
    if (request.hasArg("max_bytes")) {
        const uint32_t req = static_cast<uint32_t>(request.arg("max_bytes").toInt());
        if (req >= 256 && req <= kDebugMaxApiBytes) {
            maxBytes = req;
        }
    }

    DebugLogBuffer::LineOut lines[kDebugMaxLines];
    bool dropped = false;
    const size_t n = DebugLogBuffer::readSince(since, kDebugMaxLines, maxBytes, lines, dropped);
    const uint32_t latest = DebugLogBuffer::latestId();

    String json;
    json.reserve(1024 + (n * 180));
    json += "{\"latest\":";
    json += String(latest);
    json += ",\"dropped\":";
    json += (dropped ? "true" : "false");
    json += ",\"lines\":[";

    for (size_t i = 0; i < n; i++) {
        if (i > 0) json += ",";
        json += "{\"id\":";
        json += String(lines[i].id);
        json += ",\"t\":\"";
        if (lines[i].hasEpoch) {
            json += formatTimeHms(lines[i].ts);
        } else {
            json += formatMillis(lines[i].ts);
        }
        json += "\",\"msg\":\"";
        appendJsonEscaped(json, lines[i].msg, lines[i].msgLen);
        json += "\"";
        if (lines[i].truncated) {
            json += ",\"truncated\":true";
        }
        json += "}";
    }

    json += "]}";
    response.send(200, kContentTypeJsonUtf8, json);
}
String WebServerManager::getLittleFSInfo() {
    if (!LittleFS.begin()) {
        return "LittleFS not available";
    }

    size_t totalBytes = LittleFS.totalBytes();
    size_t usedBytes = LittleFS.usedBytes();

    String info = "<div class='info-card'>";
    info += "<h3>LittleFS Information</h3>";
    info += "<div class='status-item'><span class='status-label'>Total Size:</span><span class='status-value'>" + String(totalBytes) + " bytes</span></div>";
    info += "<div class='status-item'><span class='status-label'>Used Size:</span><span class='status-value'>" + String(usedBytes) + " bytes</span></div>";

    struct FileSizeEntry {
        String path;   // normalized, starts with '/'
        size_t size;
    };

    const char* keyFiles[] = {
        "/error_history.json",
        "/ntp_logs.json",
        "/relay_events.json", // RelayLogger storage
        "/config.json",
    };

    auto normalizePath = [](const char* rawName) -> String {
        String path = rawName ? String(rawName) : String("");
        if (path.length() > 0 && !path.startsWith("/")) {
            path = "/" + path;
        }
        return path;
    };

    auto displayName = [](const String& normalizedPath) -> String {
        if (normalizedPath.startsWith("/")) {
            return normalizedPath.substring(1);
        }
        return normalizedPath;
    };

    auto isBackupFile = [](const String& normalizedPath) -> bool {
        return normalizedPath.endsWith("_backup.json");
    };

    auto isVersionedFile = [](const String& normalizedPath) -> bool {
        // Match typical versioned files like: foo_v2.json, foo-v3.json, foo.v10.json
        if (!normalizedPath.endsWith(".json")) {
            return false;
        }

        int slash = normalizedPath.lastIndexOf('/');
        String filename = (slash >= 0) ? normalizedPath.substring(slash + 1) : normalizedPath;
        if (!filename.endsWith(".json") || filename.length() <= 5) {
            return false;
        }

        String base = filename.substring(0, filename.length() - 5); // strip ".json"
        if (base.length() < 3) { // minimum "a_v2"
            return false;
        }

        auto isDigitChar = [](char c) -> bool { return c >= '0' && c <= '9'; };

        int i = (int)base.length() - 1;
        if (!isDigitChar(base.charAt(i))) {
            return false;
        }
        while (i >= 0 && isDigitChar(base.charAt(i))) {
            i--;
        }
        if (i < 0) {
            return false;
        }

        char v = base.charAt(i);
        if (v != 'v' && v != 'V') {
            return false;
        }
        if (i == 0) {
            return false;
        }

        char sep = base.charAt(i - 1);
        return (sep == '_' || sep == '-' || sep == '.');
    };

    auto isKeyFile = [&](const String& normalizedPath) -> bool {
        for (const auto* key : keyFiles) {
            if (normalizedPath == key) {
                return true;
            }
        }
        return false;
    };

    FileSizeEntry largestOtherFiles[3]; // sorted desc, max size 3
    int largestOtherCount = 0;
    auto considerLargestOther = [&](const String& normalizedPath, size_t fileSize) {
        // Avoid duplicates
        for (int i = 0; i < largestOtherCount; i++) {
            if (largestOtherFiles[i].path == normalizedPath) {
                return;
            }
        }

        int insertPos = largestOtherCount; // default append
        for (int i = 0; i < largestOtherCount; i++) {
            if (fileSize > largestOtherFiles[i].size) {
                insertPos = i;
                break;
            }
        }

        if (insertPos >= 3) {
            return; // smaller than existing top 3
        }

        int newCount = (largestOtherCount < 3) ? (largestOtherCount + 1) : 3;
        for (int i = newCount - 1; i > insertPos; i--) {
            largestOtherFiles[i] = largestOtherFiles[i - 1];
        }
        largestOtherFiles[insertPos] = FileSizeEntry{normalizedPath, fileSize};
        largestOtherCount = newCount;

        // If we were already full, drop the last entry implicitly by not increasing count.
        if (largestOtherCount > 3) {
            largestOtherCount = 3;
        }
    };

    // Show key files + 3 biggest other (non-backup) files (for space usage)
    info += "<table style='width:100%; border-collapse: collapse;'>";

    File root = LittleFS.open("/");
    int fileCount = 0;

       
    if (root) {
        File file = root.openNextFile();
        while (file) {
            if (!file.isDirectory()) { // Skip directories
                fileCount++;
                String normalizedPath = normalizePath(file.name());
                const size_t fileSize = file.size();

                // Backup files are intentionally ignored for display purposes.
                if (!isKeyFile(normalizedPath) && !isBackupFile(normalizedPath) && !isVersionedFile(normalizedPath)) {
                    considerLargestOther(normalizedPath, fileSize);
                }
            }
            file.close();
            file = root.openNextFile();
        }
        root.close();
    } else {
    }


    const char* cellStyleName = "border-bottom: 1px solid #2d2d2d; font-family: monospace; font-size: 1.1em;";
    const char* cellStyleSize = "text-align: right; border-bottom: 1px solid #2d2d2d; font-family: monospace; font-size: 1.1em;";

    // Key files (explicit, non-backup)
    int keyFilesFound = 0;
    for (const auto* keyPath : keyFiles) {
        String key = String(keyPath);
        if (LittleFS.exists(key)) {
            File f = LittleFS.open(key, "r");
            size_t sz = f ? f.size() : 0;
            if (f) f.close();
            info += "<tr><td style='" + String(cellStyleName) + "'>" + displayName(key) + "</td><td style='" + String(cellStyleSize) + "'>" + String(sz) + " bytes</td></tr>";
            keyFilesFound++;
        }
    }
    if (keyFilesFound == 0) {
        info += "<tr><td colspan='2' style='text-align: center; border-bottom: 1px solid #2d2d2d;'>No key files found</td></tr>";
    }

    // Largest other files (not in key list, non-backup)
    if (largestOtherCount > 0) {
        // Small visual separator between key files and the rest.
        info += "<tr><td colspan='2' style='height: 8px; border-bottom: none;'></td></tr>";
        for (int i = 0; i < largestOtherCount; i++) {
            info += "<tr><td style='" + String(cellStyleName) + "'>" + displayName(largestOtherFiles[i].path) + "</td><td style='" + String(cellStyleSize) + "'>" + String(largestOtherFiles[i].size) + " bytes</td></tr>";
        }
    }

    info += "</table>";
    info += "</div>";

    return info;
}


void WebServerManager::streamRelayHtml(webserver::IResponse& response) {
    HtmlStreamWriter html(response);
    html += "<html><head><title>Relay Control</title>";
    html += "<style>body { background-color: #1e1e1e; color: #ffffff; font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif; margin: 0; padding: 60px 20px 20px 20px; } ";
    html += "h1 { color: #2196f3; } p { color: #b0b0b0; } ";
    html += ".relay-container { background: #2d2d2d; border: 1px solid #424242; border-radius: 8px; padding: 20px; max-width: 600px; margin: 20px auto; } ";
    html += ".relay-status { font-size: 1.5em; font-weight: bold; margin: 20px 0; text-align: center; } ";
    html += ".relay-controls { display: grid; grid-template-columns: 1fr 1fr; gap: 15px; margin-top: 20px; } ";
    html += ".relay-button { padding: 15px; font-size: 1.1em; font-weight: bold; text-align: center; border: none; border-radius: 4px; cursor: pointer; transition: all 0.3s; box-shadow: 0 4px 8px rgba(0,0,0,0.3), inset 0 1px 0 rgba(255,255,255,0.2); } ";
    html += ".relay-button-off { background: linear-gradient(135deg, #666, #555); color: white; } .relay-button-off:hover { background: linear-gradient(135deg, #777, #666); transform: translateY(-2px); box-shadow: 0 6px 12px rgba(0,0,0,0.4), inset 0 1px 0 rgba(255,255,255,0.3); } ";
    html += ".relay-button-low { background: linear-gradient(135deg, #00ff00, #009900); color: white; } .relay-button-low:hover { background: linear-gradient(135deg, #66ff66, #669966); transform: translateY(-2px); box-shadow: 0 6px 12px rgba(76,175,80,0.4), inset 0 1px 0 rgba(255,255,255,0.3); } ";
    html += ".relay-button-mid { background: linear-gradient(135deg, #ffff00, #999900); color: white; } .relay-button-mid:hover { background: linear-gradient(135deg, #ffff66, #999966); transform: translateY(-2px); box-shadow: 0 6px 12px rgba(255,152,0,0.4), inset 0 1px 0 rgba(255,255,255,0.3); } ";
    html += ".relay-button-high { background: linear-gradient(135deg, #ff0000, #990000); color: white; } .relay-button-high:hover { background: linear-gradient(135deg, #ff6666, #996666); transform: translateY(-2px); box-shadow: 0 6px 12px rgba(244,67,54,0.4), inset 0 1px 0 rgba(255,255,255,0.3); } ";
    html += ".relay-button.active { box-shadow: 0 0 15px rgba(33, 150, 243, 0.8), inset 0 1px 0 rgba(255,255,255,0.4); transform: scale(1.05); } ";
    html += ".relay-info { background: #1a1a1a; border: 1px solid #424242; border-radius: 8px; padding: 15px; margin-top: 20px; } ";
    html += ".relay-info h3 { color: #2196f3; margin-top: 0; } ";
    html += ".relay-info p { margin: 5px 0; } ";
    html += ".timer-container { background: #1a1a1a; border: 1px solid #424242; border-radius: 8px; padding: 15px; margin-top: 20px; } ";
    html += ".timer-container h3 { color: #2196f3; margin-top: 0; } ";
    html += ".timer-input { margin-bottom: 10px; } ";
    html += ".timer-input label { color: #b0b0b0; } ";
    html += ".timer-input input { background-color: #333333; color: #ffffff; border: 1px solid #424242; padding: 8px 12px; border-radius: 4px; font-size: 0.9em; } ";
    html += ".timer-controls { display: flex; gap: 10px; margin-bottom: 10px; } ";
    html += ".timer-controls button { background-color: #2196f3; color: white; border: none; padding: 10px 0; border-radius: 4px; cursor: pointer; width: 130px; text-align: center; font-weight: bold; } ";
    html += ".timer-controls button:hover { background-color: #1976d2; } ";
    html += ".timer-controls button.stop { background-color: #f44336; } ";
    html += ".timer-controls button.stop:hover { background-color: #d32f2f; } ";
    html += "#timerDisplay { display: none; text-align: center; margin-bottom: 10px; } ";
    html += "#countdown { font-size: 2em; font-weight: bold; color: #2196f3; } ";
    html += ".progress-bar { width: 100%; height: 20px; background: #424242; border-radius: 10px; overflow: hidden; margin: 10px 0; } ";
    html += "#progressFill { height: 100%; background: linear-gradient(90deg, #4caf50, #2196f3); transition: width 1s ease; } ";
    html += "@media (max-width: 600px) { .relay-controls { grid-template-columns: 1fr; } } ";
    html += "input::-webkit-outer-spin-button, input::-webkit-inner-spin-button { -webkit-appearance: none; margin: 0; } ";
    html += "input[type=number] { -moz-appearance: textfield; appearance: textfield; } ";
    html += ".timer-input input { width: 65px; text-align: center; } ";
    html += "</style></head><body>" + getNavHtml("relay");

    html += "<h1>Relay Control</h1>";
    html += "<div class='relay-container'>";

    // Current status display
    String currentMode = "OFF";
    if (relayManager && relayManager->isInitialized()) {
        currentMode = relayManager->getModeString();
        currentMode.toUpperCase();
    } else {
    }

    html += "<div class='relay-status' id='currentStatus'><strong>CURRENT MODE:</strong> <span id='modeValue'>" + currentMode + "</span></div>";

    // Control buttons - Use JavaScript for immediate UI updates
    html += "<div class='relay-controls'>";
    html += "<button onclick='setRelayMode(\"off\")' class='relay-button relay-button-off' id='offBtn'>OFF</button>";
    html += "<button onclick='setRelayMode(\"low\")' class='relay-button relay-button-low' id='lowBtn'>LOW</button>";
    html += "<button onclick='setRelayMode(\"mid\")' class='relay-button relay-button-mid' id='midBtn'>MID</button>";
    html += "<button onclick='setRelayMode(\"high\")' class='relay-button relay-button-high' id='highBtn'>HIGH</button>";
    html += "</div>";

    // Relay information
    html += "<div class='relay-info'>";
    html += "<h3>Relay Configuration</h3>";
    if (relayManager && relayManager->isInitialized()) {
        String statusText = "Initialized and ready";
        if (mainController && mainController->isSafetyShutdownActive()) {
            statusText = "Safety Shutdown Active";
        }
        html += "<p><strong>Status:</strong> <span id='relayStatus'>" + statusText + "</span></p>";

        // Show configured pins for each state
        const Settings settings = configManager->getSettingsSnapshot();

        html += "<p><strong>GPIO Pins:</strong></p>";
        html += "<ul style='margin: 5px 0; padding-left: 20px;'>";

        // LOW state pins
        html += "<li><strong>LOW:</strong> ";
        if (settings.relay.pins_low.size() > 0) {
            for (size_t i = 0; i < settings.relay.pins_low.size(); i++) {
                if (i > 0) html += ", ";
                html += String(settings.relay.pins_low[i]);
            }
        } else {
            html += "None";
        }
        html += "</li>";

        // MID state pins
        html += "<li><strong>MID:</strong> ";
        if (settings.relay.pins_mid.size() > 0) {
            for (size_t i = 0; i < settings.relay.pins_mid.size(); i++) {
                if (i > 0) html += ", ";
                html += String(settings.relay.pins_mid[i]);
            }
        } else {
            html += "None";
        }
        html += "</li>";

        // HIGH state pins
        html += "<li><strong>HIGH:</strong> ";
        if (settings.relay.pins_high.size() > 0) {
            for (size_t i = 0; i < settings.relay.pins_high.size(); i++) {
                if (i > 0) html += ", ";
                html += String(settings.relay.pins_high[i]);
            }
        } else {
            html += "None";
        }
        html += "</li>";

        html += "</ul>";

    } else {
        html += "<p><strong>Status:</strong> Not initialized</p>";
    }
    html += "</div>";

    // Timer UI
    html += "<div class='timer-container'>";
    html += "<h3>Timer Control</h3>";
    html += "<div class='timer-row' style='display: flex; justify-content: space-between; align-items: center; margin-bottom: 10px;'>";
    html += "<div class='timer-input'>";
    html += "<label for='timerDuration'>Duration (60-240 minutes):</label>";
    html += "<input type='number' id='timerDuration' class='no-arrows' min='60' max='240' value='60'>";
    html += "</div>";
    html += "<div class='timer-controls'>";
    html += "<button onclick='startTimer()'>START Timer</button>";
    html += "<button onclick='stopTimer()' class='stop'>STOP Timer</button>";
    html += "</div>";
    html += "</div>";
    html += "<div id='timerDisplay'>";
    html += "<div id='countdown'>00:00:00</div>";
    html += "<div class='progress-bar'>";
    html += "<div id='progressFill' style='width: 100%;'></div>";
    html += "</div>";
    html += "</div>";
    html += "</div>";

    html += "</div>";

    // JavaScript for relay control
    html += "<script>";
    html += "let countdownInterval = null;";
    html += "let remainingSeconds = 0;";
    html += "let totalSeconds = 0;";
    html += "function setRelayMode(mode) {";
    html += "    console.log('DEBUG: setRelayMode called with mode:', mode);";
    html += "    fetch('/relay/' + mode, {";
    html += "        method: 'GET'";
    html += "    }).then(response => {";
    html += "        console.log('DEBUG: fetch response status:', response.status);";
    html += "        if (!response.ok) {";
    html += "            throw new Error('HTTP ' + response.status + ': ' + response.statusText);";
    html += "        }";
    html += "        return response.json();";
    html += "    }).then(data => {";
    html += "        console.log('DEBUG: Relay set success:', data);";
    html += "        updateStatus();";
    html += "    }).catch(error => {";
    html += "        console.error('DEBUG: Error setting relay:', error);";
    html += "        alert('Error: ' + error.message);";
    html += "    });";
    html += "};";
    html += "function updateStatus() {";
    html += "    console.log('DEBUG: updateStatus called');";
    html += "    fetch('/api/relay/status?t=' + Date.now(), { credentials: 'include' }).then(response => {";
    html += "        console.log('DEBUG: status fetch response status:', response.status);";
    html += "        if (!response.ok) {";
    html += "            throw new Error('HTTP ' + response.status + ': ' + response.statusText);";
    html += "        }";
    html += "        return response.json();";
    html += "    }).then(data => {";
    html += "        console.log('DEBUG: status data received:', data);";
    html += "        const mode = data.current_mode;";
    html += "        document.getElementById('modeValue').textContent = mode.toUpperCase();";
    html += "        let statusText = 'Initialized and ready';";
    html += "        if (data.safety_shutdown_active) {";
    html += "            statusText = 'Safety Shutdown Active';";
    html += "        }";
    html += "        document.getElementById('relayStatus').textContent = statusText;";
    html += "        document.querySelectorAll('.relay-button').forEach(btn => {";
    html += "            btn.classList.remove('active');";
    html += "        });";
    html += "        document.getElementById(mode.toLowerCase() + 'Btn').classList.add('active');";
    html += "        if (data.timer) {";
    html += "            updateTimerDisplay(data.timer);";
    html += "        }";
    html += "    }).catch(error => {";
    html += "        console.error('DEBUG: Error updating status:', error);";
    html += "    });";
    html += "};";
    html += "function startTimer() {";
    html += "    const duration = parseInt(document.getElementById('timerDuration').value);";
    html += "    if (duration < 60 || duration > 240) {";
    html += "        alert('Duration must be between 60 and 240 minutes');";
    html += "        return;";
    html += "    }";
    html += "    fetch('/api/relay/set', {";
    html += "        method: 'POST',";
    html += "        headers: {'Content-Type': 'application/json'},";
    html += "        body: JSON.stringify({cmd: 'timer_start', duration: duration})";
    html += "    }).then(response => response.json()).then(data => {";
    html += "        if (data.status) {";
    html += "            updateStatus();";
    html += "        } else {";
    html += "            alert('Error: ' + (data.error || 'Unknown error'));";
    html += "        }";
    html += "    }).catch(error => alert('Error: ' + error));";
    html += "};";
    html += "function stopTimer() {";
    html += "    fetch('/api/relay/set', {";
    html += "        method: 'POST',";
    html += "        headers: {'Content-Type': 'application/json'},";
    html += "        body: JSON.stringify({cmd: 'timer_stop'})";
    html += "    }).then(response => response.json()).then(data => {";
    html += "        if (data.status) {";
    html += "            updateStatus();";
    html += "        } else {";
    html += "            alert('Error: ' + (data.error || 'Unknown error'));";
    html += "        }";
    html += "    }).catch(error => alert('Error: ' + error));";
    html += "};";
    html += "function updateCountdownDisplay() {";
    html += "    const hours = Math.floor(remainingSeconds / 3600);";
    html += "    const minutes = Math.floor((remainingSeconds % 3600) / 60);";
    html += "    const seconds = remainingSeconds % 60;";
    html += "    document.getElementById('countdown').textContent = String(hours).padStart(2, '0') + ':' + String(minutes).padStart(2, '0') + ':' + String(seconds).padStart(2, '0');";
    html += "    const progressPercent = (remainingSeconds / totalSeconds) * 100;";
    html += "    document.getElementById('progressFill').style.width = progressPercent + '%';";
    html += "};";
    html += "function updateTimerDisplay(timerData) {";
    html += "    const display = document.getElementById('timerDisplay');";
    html += "    if (timerData.active) {";
    html += "        display.style.display = 'block';";
    html += "        remainingSeconds = timerData.remaining_seconds;";
    html += "        totalSeconds = timerData.total_seconds;";
    html += "        updateCountdownDisplay();";
    html += "        if (countdownInterval) clearInterval(countdownInterval);";
    html += "        countdownInterval = setInterval(() => {";
    html += "            remainingSeconds--;";
    html += "            if (remainingSeconds <= 0) {";
    html += "                clearInterval(countdownInterval);";
    html += "                countdownInterval = null;";
    html += "                display.style.display = 'none';";
    html += "            } else {";
    html += "                updateCountdownDisplay();";
    html += "            }";
    html += "        }, 1000);";
    html += "    } else {";
    html += "        display.style.display = 'none';";
    html += "        if (countdownInterval) {";
    html += "            clearInterval(countdownInterval);";
    html += "            countdownInterval = null;";
    html += "        }";
    html += "    }";
    html += "};";
    html += "function checkSchedulerImmunity() {";
    html += "    fetch('/api/settings?t=' + Date.now(), { credentials: 'include' })";
    html += "        .then(response => response.json())";
    html += "        .then(data => {";
    html += "            if (data.schedules && data.schedules.enabled) {";
    html += "                document.querySelectorAll('.relay-button').forEach(btn => {";
    html += "                    btn.disabled = true;";
    html += "                    btn.style.opacity = '0.5';";
    html += "                    btn.style.cursor = 'not-allowed';";
    html += "                    btn.title = 'Manual control disabled when scheduler is enabled';";
    html += "                });";
    html += "                const statusDiv = document.getElementById('currentStatus');";
    html += "                if (statusDiv && !statusDiv.innerHTML.includes('SCHEDULER ACTIVE')) {";
    html += "                    statusDiv.innerHTML += '<br><small style=\"color: #ff9800;\">SCHEDULER ACTIVE - Manual control disabled</small>';";
    html += "                }";
    html += "            }";
    html += "        })";
    html += "        .catch(error => {";
    html += "            console.log('Error checking scheduler status:', error);";
    html += "        });";
    html += "};";
    html += "function init() {";
    html += "    updateStatus();";
    html += "    checkSchedulerImmunity();";
    html += "    setInterval(updateStatus, 5000);";
    html += "}";
    html += "if (document.readyState === 'loading') {";
    html += "    document.addEventListener('DOMContentLoaded', init);";
    html += "} else {";
    html += "    init();";
    html += "}";
    html += "</script>";

    html += "</body></html>";
    html.finish();
}

void WebServerManager::handleApiRelayStatus(webserver::IRequest& request, webserver::IResponse& response) {
    const unsigned long requestStartUs = micros();

    if (!checkAuthentication(request, response)) {
        return; // Authentication failed, response already sent
    }

    if (!isWiFiConnected()) {
        recordRelayStatusApiRequest(503, (micros() - requestStartUs) / 1000UL);
        sendJsonError(response, 503, "WiFi connection lost", kRelayErrorWifiDisconnected);
        return;
    }

    if (!relayManager || !relayManager->isInitialized()) {
        recordRelayStatusApiRequest(500, (micros() - requestStartUs) / 1000UL);
        response.send(500, kContentTypeJsonUtf8, "{\"error\":\"Relay manager not initialized\"}");
        return;
    }

    String currentMode = relayManager->getModeString();
    currentMode.toUpperCase();  // Ensure uppercase for API response

    String json = "{";
    json += "\"current_mode\":\"" + currentMode + "\"";

    // Add safety shutdown status
    bool safetyShutdownActive = mainController && mainController->isSafetyShutdownActive();
    json += ",\"safety_shutdown_active\":" + String(safetyShutdownActive ? "true" : "false");

    // Add timer status
    bool timerActive = relayManager->isTimerActive();
    if (timerActive) {
        unsigned long remaining = relayManager->getRemainingSeconds();
        unsigned long total = relayManager->getTotalSeconds();
        json += ",\"timer\":{\"active\":true,\"remaining_seconds\":" + String(remaining) + ",\"total_seconds\":" + String(total) + "}";
    } else {
        json += ",\"timer\":{\"active\":false}";
    }

    json += "}";

    recordRelayStatusApiRequest(200, (micros() - requestStartUs) / 1000UL);
    response.send(200, kContentTypeJsonUtf8, json);
}


// NEW: Unified relay control handler for both API and direct endpoints
void WebServerManager::handleApiRelaySet(webserver::IRequest& request, webserver::IResponse& response) {

    if (!checkAuthentication(request, response)) {
        return; // Authentication failed, response already sent
    }

    if (!isWiFiConnected()) {
        sendJsonError(response, 503, "WiFi connection lost", kRelayErrorWifiDisconnected);
        return;
    }

    if (!relayManager || !relayManager->isInitialized()) {
        response.send(500, kContentTypeJsonUtf8, "{\"error\":\"Relay manager not initialized\"}");
        return;
    }

    // Check if scheduler immunity is active
    if (configManager->getSettingsSnapshot().schedules.enabled) {
        UnifiedLogger::warning("[WEB] Manual relay control blocked - scheduler immunity active\n");
        response.send(403, kContentTypeJsonUtf8,
                    "{\"error\":\"Manual relay control disabled when scheduler is enabled\"}");
        return;
    }

    // Handle JSON body for API requests
    String mode = "";
    if (request.hasArg("plain")) {
        String body = request.body();

        // Parse JSON to extract cmd field
        PsramJsonAllocator jsonAllocator;
        JsonDocument doc(&jsonAllocator);
        DeserializationError error = deserializeJson(doc, body);

        if (error) {
            response.send(400, kContentTypeJsonUtf8, "{\"error\":\"Invalid JSON format\"}");
            return;
        }

        if (doc["cmd"].isNull()) {
            response.send(400, kContentTypeJsonUtf8, "{\"error\":\"Missing cmd field in JSON\"}");
            return;
        }

        mode = doc["cmd"].as<String>();
        mode.toLowerCase();

        // Handle timer commands before relay commands
        if (mode == "timer_start") {
            if (doc["duration"].isNull()) {
                response.send(400, kContentTypeJsonUtf8, "{\"error\":\"Missing duration parameter for timer_start\"}");
                return;
            }
            int duration = doc["duration"];
            if (duration < 60 || duration > 240) {
                response.send(400, kContentTypeJsonUtf8, "{\"error\":\"Duration must be between 60 and 240 minutes\"}");
                return;
            }
            MainController::ControlCommand command;
            command.type = MainController::ControlCommandType::RELAY_TIMER_START;
            command.arg0 = duration;
            copyCommandTextArg(command, "api");

            const webserver::ControlDispatchWaitResult dispatchResult =
                webserver::dispatchControlCommandAndWait(
                    mainController,
                    command,
                    kRelayControlDispatchTimeoutMs,
                    kControlDispatchPollIntervalMs);
            if (dispatchResult.status == webserver::ControlDispatchWaitStatus::ControllerUnavailable) {
                response.send(500, kContentTypeJsonUtf8, "{\"error\":\"Main controller not set\"}");
                return;
            }
            if (dispatchResult.status == webserver::ControlDispatchWaitStatus::EnqueueRejected) {
                sendJsonError(response, 503, "Relay command busy, try again", kRelayErrorQueueBusy);
                return;
            }
            if (dispatchResult.status == webserver::ControlDispatchWaitStatus::TimedOut) {
                sendJsonError(response, 503, "Relay command wait timeout, try again", kRelayErrorWaitTimeout);
                return;
            }
            if (dispatchResult.status != webserver::ControlDispatchWaitStatus::Completed ||
                dispatchResult.commandResult.status != MainController::ControlCommandStatus::SUCCEEDED) {
                if (dispatchResult.commandResult.status == MainController::ControlCommandStatus::TIMED_OUT) {
                    sendJsonError(response, 503, "Relay command queue timeout, try again", kRelayErrorQueueTimeout);
                    return;
                }
                response.send(500, kContentTypeJsonUtf8, "{\"error\":\"Failed to start timer\"}");
                return;
            }

            response.send(200, kContentTypeJsonUtf8, "{\"status\":\"Timer started successfully\"}");
            return;
        } else if (mode == "timer_stop") {
            MainController::ControlCommand command;
            command.type = MainController::ControlCommandType::RELAY_TIMER_STOP;
            copyCommandTextArg(command, "api");

            const webserver::ControlDispatchWaitResult dispatchResult =
                webserver::dispatchControlCommandAndWait(
                    mainController,
                    command,
                    kRelayControlDispatchTimeoutMs,
                    kControlDispatchPollIntervalMs);
            if (dispatchResult.status == webserver::ControlDispatchWaitStatus::ControllerUnavailable) {
                response.send(500, kContentTypeJsonUtf8, "{\"error\":\"Main controller not set\"}");
                return;
            }
            if (dispatchResult.status == webserver::ControlDispatchWaitStatus::EnqueueRejected) {
                sendJsonError(response, 503, "Relay command busy, try again", kRelayErrorQueueBusy);
                return;
            }
            if (dispatchResult.status == webserver::ControlDispatchWaitStatus::TimedOut) {
                sendJsonError(response, 503, "Relay command wait timeout, try again", kRelayErrorWaitTimeout);
                return;
            }
            if (dispatchResult.status != webserver::ControlDispatchWaitStatus::Completed ||
                dispatchResult.commandResult.status != MainController::ControlCommandStatus::SUCCEEDED) {
                if (dispatchResult.commandResult.status == MainController::ControlCommandStatus::TIMED_OUT) {
                    sendJsonError(response, 503, "Relay command queue timeout, try again", kRelayErrorQueueTimeout);
                    return;
                }
                response.send(500, kContentTypeJsonUtf8, "{\"error\":\"Failed to stop timer\"}");
                return;
            }

            response.send(200, kContentTypeJsonUtf8, "{\"status\":\"Timer stopped successfully\"}");
            return;
        }
    } else {
        response.send(400, kContentTypeJsonUtf8, "{\"error\":\"No JSON body provided\"}");
        return;
    }

    // Validate relay command
    if (mode != "off" && mode != "low" && mode != "mid" && mode != "high") {
        response.send(400, kContentTypeJsonUtf8, "{\"error\":\"Invalid relay command. Valid commands are: off, low, mid, high, timer_start, timer_stop\"}");
        return;
    }

    const RelayMode requestedMode = RelayManager::stringToMode(mode);

    // Set relay mode through queued control command (source: "api")
    MainController::ControlCommand command;
    command.type = MainController::ControlCommandType::RELAY_SET_MODE;
    command.arg0 = static_cast<int32_t>(requestedMode);
    copyCommandTextArg(command, "api");

    const webserver::ControlDispatchWaitResult dispatchResult =
        webserver::dispatchControlCommandAndWait(
            mainController,
            command,
            kRelayControlDispatchTimeoutMs,
            kControlDispatchPollIntervalMs);
    if (dispatchResult.status == webserver::ControlDispatchWaitStatus::ControllerUnavailable) {
        response.send(500, kContentTypeJsonUtf8, "{\"error\":\"Main controller not set\"}");
        return;
    }
    if (dispatchResult.status == webserver::ControlDispatchWaitStatus::EnqueueRejected) {
        sendJsonError(response, 503, "Relay command busy, try again", kRelayErrorQueueBusy);
        return;
    }
    if (dispatchResult.status == webserver::ControlDispatchWaitStatus::TimedOut) {
        sendJsonError(response, 503, "Relay command wait timeout, try again", kRelayErrorWaitTimeout);
        return;
    }
    if (dispatchResult.status != webserver::ControlDispatchWaitStatus::Completed) {
        response.send(500, kContentTypeJsonUtf8, "{\"error\":\"Relay command failed\"}");
        return;
    }

    if (dispatchResult.commandResult.status == MainController::ControlCommandStatus::TIMED_OUT) {
        sendJsonError(response, 503, "Relay command queue timeout, try again", kRelayErrorQueueTimeout);
        return;
    }

    const bool success = dispatchResult.commandResult.status == MainController::ControlCommandStatus::SUCCEEDED;

    if (success) {
        String jsonResponse = "{\"status\":\"success\",\"current_mode\":\"" + mode + "\"}";
        response.send(200, kContentTypeJsonUtf8, jsonResponse);
    } else {

        // Check if safety shutdown is active first (most specific check)
        if (relayManager && relayManager->isSafetyShutdownActive()) {
            sendJsonError(response, 429, "Relay command denied due to safety shutdown. Temperature has to drop to safe levels for normal operation.", kRelayErrorSafetyShutdown);
            return;
        }

        // Check if this was due to timeout violation by looking at error logs
        std::vector<ErrorEntry> activeErrors = ErrorHandler::getActiveErrors();
        bool isTimeoutDenial = false;

        for (const auto& error : activeErrors) {
            if (error.code == ErrorCode::RELAY_TIMEOUT_VIOLATION) {
                isTimeoutDenial = true;
                break;
            }
        }

        if (isTimeoutDenial) {
            sendJsonError(response, 429, "Relay command denied due to safety timeout. Please wait before trying again.", kRelayErrorSafetyTimeout);
        } else {
            response.send(500, kContentTypeJsonUtf8, "{\"error\":\"Failed to set relay mode\"}");
        }
    }
}
// NEW: Get relay logs endpoint
void WebServerManager::handleApiRelayLogs(webserver::IRequest& request, webserver::IResponse& response) {

    if (!checkAuthentication(request, response)) {
        return; // Authentication failed, response already sent
    }

    if (!isWiFiConnected()) {
        sendJsonError(response, 503, "WiFi connection lost", kRelayErrorWifiDisconnected);
        return;
    }

    // Get logs from RelayLogger
    String logsJson = RelayLogger::getLogsJson();
    response.send(200, kContentTypeJsonUtf8, logsJson);
}

// NEW: Unified relay control handler for both API and direct endpoints
void WebServerManager::handleRelayControl(String mode, webserver::IRequest& request, webserver::IResponse& response) {

    if (!checkAuthentication(request, response)) {
        return; // Authentication failed, response already sent
    }

    if (!isWiFiConnected()) {
        response.send(503, kContentTypeJsonUtf8, "{\"error\":\"WiFi connection lost\"}");
        return;
    }

    if (!relayManager || !relayManager->isInitialized()) {
        response.send(500, kContentTypeJsonUtf8, "{\"error\":\"Relay manager not initialized\"}");
        return;
    }

    // Check if scheduler immunity is active
    if (configManager->getSettingsSnapshot().schedules.enabled) {
        UnifiedLogger::warning("[WEB] Manual relay control blocked - scheduler immunity active\n");
        response.send(403, kContentTypeJsonUtf8,
                    "{\"error\":\"Manual relay control disabled when scheduler is enabled\"}");
        return;
    }

    // Validate command
    if (mode != "off" && mode != "low" && mode != "mid" && mode != "high") {
        response.send(400, kContentTypeJsonUtf8, "{\"error\":\"Invalid relay command. Valid commands are: off, low, mid, high\"}");
        return;
    }

    const RelayMode requestedMode = RelayManager::stringToMode(mode);

    // Set relay mode through queued control command (source: "webpage")
    MainController::ControlCommand command;
    command.type = MainController::ControlCommandType::RELAY_SET_MODE;
    command.arg0 = static_cast<int32_t>(requestedMode);
    copyCommandTextArg(command, "webpage");

    const webserver::ControlDispatchWaitResult dispatchResult =
        webserver::dispatchControlCommandAndWait(
            mainController,
            command,
            kRelayControlDispatchTimeoutMs,
            kControlDispatchPollIntervalMs);
    if (dispatchResult.status == webserver::ControlDispatchWaitStatus::ControllerUnavailable) {
        response.send(500, kContentTypeJsonUtf8, "{\"error\":\"Main controller not set\"}");
        return;
    }
    if (dispatchResult.status == webserver::ControlDispatchWaitStatus::EnqueueRejected) {
        sendJsonError(response, 503, "Relay command busy, try again", kRelayErrorQueueBusy);
        return;
    }
    if (dispatchResult.status == webserver::ControlDispatchWaitStatus::TimedOut) {
        sendJsonError(response, 503, "Relay command wait timeout, try again", kRelayErrorWaitTimeout);
        return;
    }
    if (dispatchResult.status != webserver::ControlDispatchWaitStatus::Completed) {
        response.send(500, kContentTypeJsonUtf8, "{\"error\":\"Relay command failed\"}");
        return;
    }

    if (dispatchResult.commandResult.status == MainController::ControlCommandStatus::TIMED_OUT) {
        sendJsonError(response, 503, "Relay command queue timeout, try again", kRelayErrorQueueTimeout);
        return;
    }

    const bool success = dispatchResult.commandResult.status == MainController::ControlCommandStatus::SUCCEEDED;

    if (success) {
        String jsonResponse = "{\"status\":\"success\",\"current_mode\":\"" + mode + "\"}";
        response.send(200, kContentTypeJsonUtf8, jsonResponse);
    } else {

        // Check if safety shutdown is active first (most specific check)
        if (relayManager && relayManager->isSafetyShutdownActive()) {
            sendJsonError(response, 429, "Relay command denied due to safety shutdown. Temperature has to drop to safe levels for normal operation.", kRelayErrorSafetyShutdown);
            return;
        }

        // Check if this was due to timeout violation by looking at error logs
        std::vector<ErrorEntry> activeErrors = ErrorHandler::getActiveErrors();
        bool isTimeoutDenial = false;

        for (const auto& error : activeErrors) {
            if (error.code == ErrorCode::RELAY_TIMEOUT_VIOLATION) {
                isTimeoutDenial = true;
                break;
            }
        }

        if (isTimeoutDenial) {
            sendJsonError(response, 429, "Relay command denied due to safety timeout. Please wait before trying again.", kRelayErrorSafetyTimeout);
            return;
        } else {
            String errorJson = "{\"error\":\"Failed to set relay mode\"}";
            response.send(500, kContentTypeJsonUtf8, errorJson);
        }
    }
}

bool WebServerManager::isWiFiConnected() {
    return WiFi.status() == WL_CONNECTED;
}
