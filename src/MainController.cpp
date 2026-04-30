#include "MainController.h"
#include <Adafruit_SHT4x.h>
#include <ESPmDNS.h>
#include <sys/time.h> // settimeofday
#include "MQTTManager.h"
#include "WebServerManager.h"
#include "DS18B20SensorManager.h"
#include "SHT4xSensorManager.h"
#include "ChipTempSensorManager.h"
#include "MAX31855KSensorManager.h"
#include "UnifiedLogger.h"
#include <algorithm>
#include <cstring>

namespace {
const char* controlCommandTypeToString(MainController::ControlCommandType type) {
    switch (type) {
        case MainController::ControlCommandType::NONE: return "NONE";
        case MainController::ControlCommandType::NOOP: return "NOOP";
        case MainController::ControlCommandType::APPLY_SETTINGS_JSON: return "APPLY_SETTINGS_JSON";
        case MainController::ControlCommandType::RELAY_SET_MODE: return "RELAY_SET_MODE";
        case MainController::ControlCommandType::RELAY_TIMER_START: return "RELAY_TIMER_START";
        case MainController::ControlCommandType::RELAY_TIMER_STOP: return "RELAY_TIMER_STOP";
        case MainController::ControlCommandType::DS18_CLEAR_SCAN_RESULT: return "DS18_CLEAR_SCAN_RESULT";
        case MainController::ControlCommandType::DS18_REQUEST_SCAN: return "DS18_REQUEST_SCAN";
        case MainController::ControlCommandType::OTA_REFRESH_CONFIG_STATUS: return "OTA_REFRESH_CONFIG_STATUS";
        case MainController::ControlCommandType::OTA_ARM_WINDOW: return "OTA_ARM_WINDOW";
        case MainController::ControlCommandType::MONITORING_RESET_STATS: return "MONITORING_RESET_STATS";
        case MainController::ControlCommandType::RELAY_CLEAR_LOGS: return "RELAY_CLEAR_LOGS";
        case MainController::ControlCommandType::ERROR_CLEAR_LOGS: return "ERROR_CLEAR_LOGS";
        case MainController::ControlCommandType::MONITORING_RESET_QUEUE_DIAGNOSTICS: return "MONITORING_RESET_QUEUE_DIAGNOSTICS";
        default: return "UNKNOWN";
    }
}
} // namespace

// Global pointer for HTTPS handlers
//WebServerManager* globalWebServerManager = nullptr;

MainController::MainController() :
    mqttManager(nullptr),
    webServerManager(nullptr),
    ds18b20Manager(nullptr),
    sht4xManager(nullptr),
    max31855kManager(nullptr),
    chipTempManager(nullptr),
    ledManager(nullptr),
    relayManager(nullptr),
    scheduleManager(nullptr),
    mqttFallbackManager(nullptr),
    lastMsg(0),
    webServerInitialized(false),
    wifiEverConnected(false),
    mdnsResponderActive(false),
    lastWiFiConnectedState(false),
    bootTimeMillis(0),
    lastLogFlushTime(0),
    lastLedBaseUpdateMs(0),
    lastLedBaseStatus(STATUS_WHITE),
    ledBlinkOn(true),
    lastLedBlinkToggleMs(0),
    lastLedRenderedStatus(STATUS_WHITE),
    controlQueueHead(0),
    controlQueueTail(0),
    controlQueueCount(0),
    controlQueueProcessing(false),
    nextControlCommandId(1),
    nextSettingsUpdateToken(1),
#if defined(ARDUINO_ARCH_ESP32)
    controlQueueMutex(nullptr),
#endif
    LOG_FLUSH_INTERVAL_MS(configManager.getSettings().logging.flush_interval_seconds * 1000) {
#if defined(ARDUINO_ARCH_ESP32)
    controlQueueMutex = xSemaphoreCreateRecursiveMutex();
#endif
    resetControlCommandQueue();
}

MainController::~MainController() {
    delete mqttManager;
    delete ds18b20Manager;
    delete sht4xManager;
    delete max31855kManager;
    delete chipTempManager;
    delete ledManager;
    delete relayManager;
    delete scheduleManager;
    delete mqttFallbackManager;
#if defined(ARDUINO_ARCH_ESP32)
    if (controlQueueMutex) {
        vSemaphoreDelete(controlQueueMutex);
        controlQueueMutex = nullptr;
    }
#endif
}

void MainController::setup() {
    Serial.begin(115200);
    resetControlCommandQueue();

    // Capture boot time in milliseconds immediately
    bootTimeMillis = millis();

    // Initialize safety mechanism state
    safetyHighTempStartTime = 0;
    safetyShutdownTriggered = false;
    if (relayManager) {
        relayManager->setSafetyShutdownActive(false);
    }

    // Initialize error handling system
    // Note: Active errors are non-persistent and start fresh on each boot
    // Only error history is persistent

    // Load error history
    if (!ErrorHandler::loadErrorHistory()) {
        UnifiedLogger::warning("Could not load error history\n");
    }

    // Initialize performance monitoring stats
    resetPerformanceStats();
    resetRelayLoggerStats();
    updateMemoryDiagnostics();

    // Load configuration
    if (!configManager.loadConfiguration()) {
        UnifiedLogger::warning("Configuration not loaded, using defaults\n");
        ErrorHandler::reportError(ErrorCode::CONFIG_LOAD_FAILED, ErrorSeverity::WARNING,
                                "Configuration not loaded, using defaults");
        configManager.saveConfiguration();
    }

    // Initialize unified runtime logger (single logging control switch).
    UnifiedLogger::begin(&configManager);

    // Initialize RGB LED early for error state indication
    ledManager = new RGBLedManager();
    ledManager->begin();
    ledManager->setStatus(STATUS_BLUE); // Initializing
    lastLedBaseStatus = STATUS_BLUE;
    lastLedRenderedStatus = STATUS_BLUE;
    lastLedBaseUpdateMs = millis();
    ledBlinkOn = true;
    lastLedBlinkToggleMs = lastLedBaseUpdateMs;

    // Validate critical settings before proceeding
    String criticalError;
    if (!configManager.hasValidCriticalSettings(criticalError)) {
        UnifiedLogger::error("CRITICAL: Invalid configuration - %s\n", criticalError.c_str());
        UnifiedLogger::error("System cannot start with default/invalid critical settings.\n");
        UnifiedLogger::error("Please configure WiFi and MQTT settings via web interface or serial commands.\n");
        ErrorHandler::reportError(ErrorCode::CONFIG_VALIDATION_FAILED, ErrorSeverity::ERROR,
                                "Critical settings not configured: " + criticalError);

        // Set LED to error state
        ledManager->setStatus(STATUS_RED);
        criticalConfigInvalid = true;
        lastLedBaseStatus = STATUS_RED;
        lastLedRenderedStatus = STATUS_RED;
        lastLedBaseUpdateMs = millis();
        ledBlinkOn = true;
        lastLedBlinkToggleMs = lastLedBaseUpdateMs;

        // Keep system running but in error state - user can still access web interface to configure
        UnifiedLogger::warning("System running in configuration mode. Access web interface to configure settings.\n");
        return; // Exit setup early but don't halt completely
    }

    UnifiedLogger::info("Total heap: %d bytes\n", ESP.getHeapSize());

    // Initialize sensors
    initializeSensors();
    
    // Update DS18B20 sensor interval after initialization
    if (ds18b20Manager) {
        const Settings& settings = configManager.getSettings();
        ds18b20Manager->setSensorReadInterval(settings.sensors.ds18b20_measurement_interval * 1000);
    }

    // Initialize relay manager
    initializeRelayManager();

    // Initialize schedule manager
    scheduleManager = new ScheduleManager();
    scheduleManager->begin(configManager, ntpManager, *relayManager);
    scheduleManager->setMainController(this);

    const Settings& settings = configManager.getSettings();

    // OTA hostname follows the normal network hostname (device-side).
    // Host-side OTA password/port are read from ota.json by PlatformIO scripts.
    otaManager.setHostname(settings.network.hostname);
    otaManager.refreshConfigStatus();

    // Initialize NTP timezone rules immediately (does not require WiFi).
    // This ensures localtime/mktime behave correctly for DS3231 local-time mode.
    ntpManager.setupNTP(settings.ntp.timezone, settings.ntp.servers);

    // Set NTP manager for error handling/logging early.
    ErrorHandler::setNTPManager(&ntpManager);
    RelayLogger::setNTPManager(&ntpManager);

    // Initialize DS3231 RTC (optional) and seed system time on boot when WiFi/NTP are unavailable.
    if (settings.ntp.rtc.enabled) {
        UnifiedLogger::info("[RTC] Initializing DS3231: SDA=%d SCL=%d addr=0x%02X storeUtc=%s\n",
                             settings.ntp.rtc.sda_pin,
                             settings.ntp.rtc.scl_pin,
                             settings.ntp.rtc.i2c_address,
                             settings.ntp.rtc.store_utc ? "true" : "false");

        bool rtcOk = ds3231Manager.begin(
            settings.ntp.rtc.sda_pin,
            settings.ntp.rtc.scl_pin,
            settings.ntp.rtc.i2c_address,
            settings.ntp.rtc.store_utc
        );

        if (!rtcOk) {
            UnifiedLogger::error("[RTC] DS3231 init failed (not present or I2C error)\n");
        } else {
            UnifiedLogger::info("[RTC] DS3231 ready. lostPower=%s\n", ds3231Manager.hasLostPower() ? "true" : "false");

            DateTime rtcNow;
            if (ds3231Manager.readNow(rtcNow)) {
                UnifiedLogger::info("[RTC] DS3231 time: %04d-%02d-%02d %02d:%02d:%02d\n",
                                     rtcNow.year(), rtcNow.month(), rtcNow.day(),
                                     rtcNow.hour(), rtcNow.minute(), rtcNow.second());
            }

            // Seed system time from RTC only if RTC did not lose power and time is plausible.
            if (settings.ntp.rtc.set_system_time_on_boot && !ntpManager.isTimeSet() && !ds3231Manager.hasLostPower()) {
                if (ds3231Manager.readNow(rtcNow) && DS3231Manager::isDateTimePlausible(rtcNow)) {
                    time_t epoch = ds3231Manager.toEpoch(rtcNow);
                    if (epoch > 1577836800) { // after 2020-01-01
                        struct timeval tv;
                        tv.tv_sec = epoch;
                        tv.tv_usec = 0;
                        settimeofday(&tv, nullptr);

                        UnifiedLogger::info("[RTC] System time seeded from DS3231: %s\n", formatDateTime(epoch).c_str());

                        // Now that system time is valid, compute boot timestamp immediately.
                        updateBootDateTime();
                    } else {
                        UnifiedLogger::warning("[RTC] DS3231 epoch not plausible (%ld), skipping seed\n", (long)epoch);
                    }
                } else {
                    UnifiedLogger::warning("[RTC] DS3231 time not plausible, skipping seed\n");
                }
            } else if (ds3231Manager.hasLostPower()) {
                UnifiedLogger::warning("[RTC] DS3231 lostPower flag set. RTC time is considered invalid until calibrated from NTP.\n");
            }
        }
    } else {
        UnifiedLogger::info("[RTC] DS3231 disabled by configuration\n");
    }

    // Initialize WiFi

    // Set garage relay mode from configuration (but don't override if scheduler immunity is active)
    if (relayManager && !settings.schedules.enabled) {
        relayManager->setGarageRelayModeEnabled(settings.relay.garage_relay_mode_enabled);
    }
    wifiManager.setupWiFi(settings.wifi.ssid, settings.wifi.password);

    // Initialize boot datetime (may already be set if RTC seeded system time)
    if (!ntpManager.isTimeSet()) {
        bootDateTime = "Waiting for time (NTP/RTC)...";
    }

    // Initialize MQTT
    mqttManager = new MQTTManager();
    mqttManager->setupMQTT(settings.mqtt.server, settings.mqtt.port, settings.mqtt.client_id, settings.mqtt.user, settings.mqtt.password);
    mqttManager->setPublishInterval(settings.mqtt.publish_interval * 1000); // Convert seconds to milliseconds
    mqttManager->setMainController(this);
    mqttManager->setManagers(ds18b20Manager, sht4xManager, max31855kManager, chipTempManager, relayManager);
    mqttManager->setSettings(&configManager.getSettings());

    // Initialize MQTT fallback manager
    mqttFallbackManager = new MQTTFallbackManager();
    mqttFallbackManager->begin(configManager, *mqttManager, *scheduleManager, relayManager);

    // Initialize Web Server Manager
    webServerManager = new WebServerManager();

    // Note: Web server will be initialized after WiFi connection is established
    webServerManager->setManagers(&configManager, ds18b20Manager, sht4xManager, max31855kManager, chipTempManager, mqttManager, &ntpManager, relayManager, scheduleManager, this);

    // Set boot time in web server manager (moved here after webServerManager initialization)
    webServerManager->setBootDateTime(bootDateTime);

    if (ds18b20Manager) {
        UnifiedLogger::info("Checking power mode...\n");
        if (ds18b20Manager->isParasitePowerMode()) {
            UnifiedLogger::warning("Parasitic power detected on bus\n");
        } else {
            UnifiedLogger::info("External power confirmed on bus\n");
        }

        UnifiedLogger::info("Checking individual sensor power modes:\n");
        if (!ds18b20Manager->hasCompletedAtLeastOneReadout()) {
            UnifiedLogger::info("Initial DS18B20 temperatures not yet available\n");
        } else {
            for (int i = 0; i < settings.sensors.sensor_count; i++) {
            if (ds18b20Manager->readPowerSupply(i)) {
                UnifiedLogger::info("Sensor %d: PARASITIC power\n", i + 1);
            } else {
                UnifiedLogger::info("Sensor %d: EXTERNAL power\n", i + 1);
            }
        }

        UnifiedLogger::info("Requesting initial DS18B20 conversion/readout...\n");
        ds18b20Manager->updateTemperatures();
        if (ds18b20Manager->hasCompletedAtLeastOneReadout()) {
            UnifiedLogger::info("Initial DS18B20 readout completed\n");
        } else {
            UnifiedLogger::info("Initial DS18B20 readout pending\n");
        }

        // Store parasitic power information
        for (int i = 0; i < settings.sensors.sensor_count; i++) {
            bool isParasitic = ds18b20Manager->getParasiticPowerStatus(i);
            UnifiedLogger::info("Stored power mode for sensor %d: %s\n", i + 1,
                         isParasitic ? "PARASITIC" : "EXTERNAL");
        }

        // Log initial temperatures for verification
        for (int i = 0; i < settings.sensors.sensor_count; i++) {
            float temp = ds18b20Manager->getTemperature(i);
            UnifiedLogger::info("Sensor %d initial temp: %.2f C\n", i + 1, temp);
            }
        }
    } else {
        UnifiedLogger::info("DS18B20 sensors disabled - skipping power mode checks and initial conversion\n");
    }

    // Discovery messages will be published after MQTT connection is established

    // Wait a bit for connection to stabilize
    delay(2000);

}

void MainController::loop() {
    unsigned long loopStart = millis();
    processControlCommandQueue();

    // Periodic flush check
    if (millis() - lastLogFlushTime > LOG_FLUSH_INTERVAL_MS) {
        RelayLogger::flushRelayEventsIfDirty();
        ErrorHandler::flushErrorStateIfDirty();
        ErrorHandler::flushErrorHistoryIfDirty();
        lastLogFlushTime = millis();
    }

    // Update relay manager for pulse timing (needs to be called frequently)
    // Note: RelayManager now self-reports timing only for actual operations
    if (relayManager) {
        relayManager->updateRelays();
    }

    // Single-core mode - original logic
    // Check WiFi connection first
    wifiManager.checkWiFiConnection();
    const bool wifiConnected = wifiManager.isConnected();
    const bool hadEverConnected = wifiEverConnected;
    if (wifiConnected) {
        wifiEverConnected = true;
    }

    if (wifiConnected != lastWiFiConnectedState) {
        if (!wifiConnected) {
            lockPerformanceStats();
            wifiStability.disconnectTransitions++;
            wifiStability.lastDisconnectMs = millis();
            wifiStability.lastDisconnectStatusCode = static_cast<int32_t>(WiFi.status());
            unlockPerformanceStats();
            UnifiedLogger::warning("[WIFI] Disconnected - stopping mDNS responder\n");
            stopMdnsResponder();
        } else if (webServerInitialized) {
            lockPerformanceStats();
            if (hadEverConnected) {
                wifiStability.reconnectTransitions++;
            }
            wifiStability.lastReconnectMs = millis();
            unlockPerformanceStats();
            UnifiedLogger::info("[WIFI] Reconnected - refreshing mDNS responder\n");
            refreshMdnsResponder(true);
        }
        lastWiFiConnectedState = wifiConnected;
    }

    // OTA handling (only active while armed via /ota)
    otaManager.loop();

    // Keep MQTT disconnect timing accurate even when WiFi is down.
    // This is required for offline-autonomous scheduler fallback behavior.
    if (mqttManager) {
        mqttManager->updateConnectionState(wifiManager.isConnected());
    }

    // Update MQTT fallback manager (may enable scheduler when MQTT/WiFi are down)
    if (mqttFallbackManager) {
        mqttFallbackManager->update();
    }

    // Check schedules continuously for NTP time changes (only if enabled)
    if (scheduleManager && configManager.getSettings().schedules.enabled) {
        scheduleManager->update();
    }
 
    // Sync NTP time periodically if WiFi is connected
    static unsigned long lastNtpSync = 0;
    static bool bootTimeCalculated = false;
    if (wifiConnected) {
        unsigned long now = millis();
        // Sync every hour (3600000 ms)
        if (now - lastNtpSync > 3600000 || lastNtpSync == 0) {
            // Start a new sync window once and avoid repeating attempt logs while it runs.
            if (!ntpManager.isSyncInProgress()) {
                const bool timeSetBefore = ntpManager.isTimeSet();
                time_t epochBefore = time(nullptr);
                UnifiedLogger::info("[NTP] Sync attempt: timeSetBefore=%s epochBefore=%ld lastNtpSync=%lu\n",
                                     timeSetBefore ? "true" : "false",
                                     (long)epochBefore,
                                     lastNtpSync);
                ntpManager.beginSync();
            }

            if (ntpManager.isSyncInProgress() && ntpManager.processSync() && ntpManager.wasLastSyncSuccessful()) {
                lastNtpSync = now;

                {
                    time_t epochAfter = time(nullptr);
                    UnifiedLogger::info("[NTP] Sync success: epochAfter=%ld formatted=%s\n",
                                         (long)epochAfter,
                                         formatDateTime(epochAfter).c_str());
                }
                // Calculate boot time on first successful NTP sync
                if (!bootTimeCalculated) {
                    updateBootDateTime();
                    bootTimeCalculated = true;
                }

                // Periodic RTC calibration from NTP (only when enabled and RTC is present)
                const Settings& settings = configManager.getSettings();
                if (settings.ntp.rtc.enabled && settings.ntp.rtc.sync_rtc_from_ntp && ds3231Manager.isInitialized() && ds3231Manager.isPresent()) {
                    // Fixed 60-minute RTC calibration interval (kept in sync with NTP cadence).
                    const unsigned long intervalMs = 3600000UL;

                    if (lastRtcSyncMillis == 0 || (now - lastRtcSyncMillis) >= intervalMs) {
                        time_t systemEpoch = time(nullptr);
                        DateTime rtcNow;
                        const bool rtcReadOk = ds3231Manager.readNow(rtcNow);
                        const bool rtcPlausible = rtcReadOk && DS3231Manager::isDateTimePlausible(rtcNow);

                        UnifiedLogger::info("[RTC] Cal check: systemEpoch=%ld formatted=%s rtcReadOk=%s rtc=%04d-%02d-%02d %02d:%02d:%02d plausible=%s lostPower=%s\n",
                                             (long)systemEpoch,
                                             formatDateTime(systemEpoch).c_str(),
                                             rtcReadOk ? "true" : "false",
                                             rtcNow.year(), rtcNow.month(), rtcNow.day(),
                                             rtcNow.hour(), rtcNow.minute(), rtcNow.second(),
                                             rtcPlausible ? "true" : "false",
                                             ds3231Manager.hasLostPower() ? "true" : "false");

                        // IMPORTANT:
                        // If the RTC lost power (OSF set) or time is implausible (e.g., 2000-01-01),
                        // we must *initialize* it from NTP even though drift cannot be computed.
                        if (ds3231Manager.hasLostPower() || !rtcPlausible) {
                            UnifiedLogger::warning("[RTC] RTC time invalid or lostPower=true -> initializing RTC from NTP\n");

                            if (ds3231Manager.setFromEpoch(systemEpoch)) {
                                bool cleared = ds3231Manager.clearLostPowerFlag();
                                lastRtcSyncMillis = now;
                                lastRtcSyncEpoch = systemEpoch;

                                DateTime rtcAfter;
                                if (ds3231Manager.readNow(rtcAfter)) {
                                    UnifiedLogger::info("[RTC] RTC initialized from NTP. clearOSF=%s lostPowerNow=%s rtcNow=%04d-%02d-%02d %02d:%02d:%02d\n",
                                                         cleared ? "true" : "false",
                                                         ds3231Manager.hasLostPower() ? "true" : "false",
                                                         rtcAfter.year(), rtcAfter.month(), rtcAfter.day(),
                                                         rtcAfter.hour(), rtcAfter.minute(), rtcAfter.second());
                                } else {
                                    UnifiedLogger::warning("[RTC] RTC initialized from NTP. clearOSF=%s lostPowerNow=%s (readback failed)\n",
                                                         cleared ? "true" : "false",
                                                         ds3231Manager.hasLostPower() ? "true" : "false");
                                }
                            } else {
                                UnifiedLogger::error("[RTC] Failed to initialize RTC from NTP\n");
                            }
                        } else {
                            // Normal calibration path (RTC is plausible and OSF not set): compare drift and step if needed.
                            time_t rtcEpoch = ds3231Manager.toEpoch(rtcNow);
                            long drift = labs((long)(systemEpoch - rtcEpoch));

                            UnifiedLogger::info("[RTC] Cal drift: rtcEpoch=%ld formatted=%s drift=%ld threshold=%lu\n",
                                                 (long)rtcEpoch,
                                                 formatDateTime(rtcEpoch).c_str(),
                                                 drift,
                                                 settings.ntp.rtc.max_step_seconds);

                            if (drift > (long)settings.ntp.rtc.max_step_seconds) {
                                if (ds3231Manager.setFromEpoch(systemEpoch)) {
                                    (void)ds3231Manager.clearLostPowerFlag();
                                    lastRtcSyncMillis = now;
                                    lastRtcSyncEpoch = systemEpoch;
                                    UnifiedLogger::info("[RTC] DS3231 calibrated from NTP. Drift=%ld seconds\n", drift);
                                } else {
                                    UnifiedLogger::error("[RTC] Failed to calibrate DS3231 from NTP\n");
                                }
                            } else {
                                lastRtcSyncMillis = now;
                                lastRtcSyncEpoch = systemEpoch;
                                UnifiedLogger::info("[RTC] DS3231 drift within threshold (%ld <= %lu), no adjust\n",
                                                     drift, settings.ntp.rtc.max_step_seconds);
                            }
                        }
                    }
                }
            }
        }
    }
 
    // Initialize web server and mDNS after first WiFi connection
    if (wifiConnected && !webServerInitialized) {
        UnifiedLogger::info("WiFi connected - initializing web server and mDNS\n");

        refreshMdnsResponder(false);

        // Set global pointer for HTTPS handlers
        //globalWebServerManager = webServerManager;

        webServerManager->setupWebServer();
        webServerInitialized = true;
        UnifiedLogger::info("Web server initialized and ready\n");

        // Update LED to show WiFi connected but MQTT not yet connected
        if (ledManager) {
            ledManager->setStatus(STATUS_YELLOW); // WiFi OK, MQTT connecting
            lastLedBaseStatus = STATUS_YELLOW;
            lastLedRenderedStatus = STATUS_YELLOW;
            lastLedBaseUpdateMs = millis();
            ledBlinkOn = true;
            lastLedBlinkToggleMs = lastLedBaseUpdateMs;
        }
    }
 
    // Only attempt MQTT if WiFi is connected
    if (wifiConnected) {
        if (!mqttManager->isConnected()) {
            mqttManager->reconnect();
        }
 
        // MQTT loop() is lightweight maintenance - timing moved to actual publish operations
        mqttManager->loop();

        // Publish discovery messages after first successful MQTT connection.
        // Use explicit connected-edge consumption so async transport connects are handled reliably.
        if (mqttManager->isConnected() &&
            mqttManager->consumeConnectedEvent() &&
            !mqttManager->hasDiscoveryBeenPublished()) {
            const Settings& settings = configManager.getSettings();
            mqttManager->publishDiscovery(settings, ds18b20Manager, sht4xManager, chipTempManager, relayManager);

            // Update LED to show MQTT connected (will be updated to final status on next cycle)
            if (ledManager) {
                ledManager->setStatus(STATUS_CYAN); // MQTT connected, sensors initializing
                lastLedBaseStatus = STATUS_CYAN;
                lastLedRenderedStatus = STATUS_CYAN;
                lastLedBaseUpdateMs = millis();
                ledBlinkOn = true;
                lastLedBlinkToggleMs = lastLedBaseUpdateMs;
            }
        }
    }
 
    // Only handle web server requests if it's initialized
    // Timing is now done inside individual request handlers
    if (webServerInitialized) {
        webServerManager->handleClient();
    }
 
    handleSerialCommands();
 
    const Settings& settings = configManager.getSettings();
 
    // Update SHT4x readings with heating strategy if enabled (uses its own timing)
    if (sht4xManager && settings.sensors.sht4x_heating_strategy > 0) {
        sht4xManager->updateReadingsWithHeatingStrategy();
    }
 
    unsigned long now = millis();
  
    // DS18B20 preconversion timing requires frequent calls
    if (ds18b20Manager) {
        ds18b20Manager->processScanJob();
        ds18b20Manager->updateTemperatures();
    }

    // Update RGB LED status (decoupled from sensor/publish cadence)
    updateLedStatus(now);
  
    if (now - lastMsg > settings.sensors.ds18b20_measurement_interval * 1000) {
        lastMsg = now;

        updateOtherSensorReadings();

        // Safety monitoring logic
        // Only meaningful when relay functionality is enabled and initialized.
        if (settings.relay.safety_enabled && settings.relay.enabled && relayManager &&
            ds18b20Manager && settings.relay.safety_duration_seconds > 0) {
            int sensorIndex = settings.relay.safety_sensor_number - 1; // Convert to 0-based
            if (sensorIndex >= 0 && sensorIndex < ds18b20Manager->getSensorCount()) {
                float currentTemp = ds18b20Manager->getTemperature(sensorIndex);
                if (currentTemp >= settings.relay.safety_temperature_threshold && currentTemp != -127.0) {
                    // Temperature is above threshold
                    if (safetyHighTempStartTime == 0) {
                        // Start timing when temperature first exceeds threshold
                        safetyHighTempStartTime = millis();
                        UnifiedLogger::warning("[SAFETY] Temperature %.1f C exceeded threshold %.1f C, starting %d second timer\n",
                                      currentTemp, settings.relay.safety_temperature_threshold,
                                      settings.relay.safety_duration_seconds);
                    } else {
                        // Check if duration has elapsed
                        unsigned long elapsedSeconds = (millis() - safetyHighTempStartTime) / 1000;
                        if (elapsedSeconds >= settings.relay.safety_duration_seconds && !safetyShutdownTriggered) {
                            // Safety condition met for required duration - shutdown relays
                            const bool relayWasAlreadyOff = (relayManager->getCurrentMode() == RELAY_OFF);
                            if (relayManager->setModeWithVerification(RELAY_OFF, "safety_temperature", true)) {
                                safetyShutdownTriggered = true;
                                relayManager->setSafetyShutdownActive(true);

                                if (!relayWasAlreadyOff) {
                                    ErrorHandler::reportError(ErrorCode::RELAY_SAFETY_SHUTDOWN,
                                                            ErrorSeverity::WARNING,
                                                            "Relay safety shutdown executed by sensor " +
                                                            String(settings.relay.safety_sensor_number) +
                                                            " temperature " + String(currentTemp, 1) + "Ã‚Â°C >= " +
                                                            String(settings.relay.safety_temperature_threshold, 1) + "Ã‚Â°C for " +
                                                            String(settings.relay.safety_duration_seconds) + " seconds");
                                }
                            }
                        }
                    }
                } else {
                    // Temperature dropped below threshold - reset timer
                    if (safetyHighTempStartTime > 0) {
                        UnifiedLogger::info("[SAFETY] Temperature %.1f C dropped below threshold %.1f C, resetting timer\n",
                                      currentTemp, settings.relay.safety_temperature_threshold);
                        safetyHighTempStartTime = 0;
                        safetyShutdownTriggered = false; // Allow re-triggering if temperature rises again
                        if (relayManager) {
                            relayManager->setSafetyShutdownActive(false);
                        }
                    }
                }
            } else {
                // Invalid sensor number - log error once
                static bool sensorErrorLogged = false;
                if (!sensorErrorLogged) {
                    ErrorHandler::reportError(ErrorCode::CONFIG_VALIDATION_FAILED,
                                            ErrorSeverity::ERROR,
                                            "Safety sensor number " + String(settings.relay.safety_sensor_number) +
                                            " is invalid (must be 1-" + String(settings.sensors.sensor_count) + ")");
                    sensorErrorLogged = true;
                }
            }
        } else if (settings.relay.safety_enabled && (!settings.relay.enabled || !relayManager)) {
            // Prevent null dereferences and confusing behavior when safety is enabled but relays are disabled.
            static bool safetyRelayDisabledWarned = false;
            if (!safetyRelayDisabledWarned) {
                UnifiedLogger::warning("[SAFETY] Temp safety is enabled but relay functionality is disabled; safety shutdown is inactive\n");
                safetyRelayDisabledWarned = true;
            }

            // Reset safety timer state so it can't "carry over" if relays are later enabled.
            safetyHighTempStartTime = 0;
            safetyShutdownTriggered = false;
        }

        // Timer expiry check
        if (relayManager && relayManager->isTimerActive() && relayManager->isExpired()) {
            UnifiedLogger::warning("[TIMER] Timer expired - deactivating relays\n");
            String oldMode = relayManager->getModeString();
            bool success = relayManager->setModeWithVerification(RELAY_OFF, "timer_expiry", true);
            if (success) {
                UnifiedLogger::info("[TIMER] Relay mode change from %s to OFF: SUCCESS\n", oldMode.c_str());
            } else {
                UnifiedLogger::warning("[TIMER] Relay mode change from %s to OFF: FAILED\n", oldMode.c_str());
            }
            relayManager->stopTimer();
            UnifiedLogger::info("[TIMER] Timer stopped after expiry\n");
        }

    }

    // Update main loop statistics
    updateMemoryDiagnostics();
    unsigned long loopDuration = millis() - loopStart;
    updateMainLoopStats(loopDuration);
}

void MainController::refreshMdnsResponder(bool forceRestart) {
    const Settings& settings = configManager.getSettings();
    if (!settings.network.mdns_enabled || settings.network.hostname.length() == 0) {
        if (mdnsResponderActive) {
            stopMdnsResponder();
        } else {
            UnifiedLogger::warning("[mDNS] Disabled or hostname not configured\n");
        }
        return;
    }

    if (forceRestart && mdnsResponderActive) {
        stopMdnsResponder();
    } else if (mdnsResponderActive) {
        return;
    }

    if (MDNS.begin(settings.network.hostname.c_str())) {
        mdnsResponderActive = true;
        UnifiedLogger::info("[mDNS] Responder started: %s.local\n", settings.network.hostname.c_str());
        MDNS.addService("http", "tcp", 80);
        UnifiedLogger::info("[mDNS] HTTP service registered\n");
    } else {
        mdnsResponderActive = false;
        UnifiedLogger::error("[mDNS] Failed to start responder\n");
    }
}

void MainController::stopMdnsResponder() {
    if (!mdnsResponderActive) {
        return;
    }
    MDNS.end();
    mdnsResponderActive = false;
    UnifiedLogger::warning("[mDNS] Responder stopped\n");
}

bool MainController::enqueueControlCommand(const ControlCommand& command, uint32_t& outCommandId) {
    outCommandId = 0;
    if (!lockControlQueue()) {
        UnifiedLogger::error("[CTRLQ] enqueue failed: mutex lock unavailable for command %s\n",
                             controlCommandTypeToString(command.type));
        return false;
    }

    if (command.type == ControlCommandType::NONE) {
        UnifiedLogger::warning("[CTRLQ] enqueue rejected: invalid command type NONE\n");
        controlQueueDiagnostics.enqueueRejected++;
        unlockControlQueue();
        return false;
    }

    if (isControlCommandQueueFull()) {
        UnifiedLogger::warning("[CTRLQ] enqueue rejected: queue full (%u/%u) for command %s\n",
                               static_cast<unsigned int>(controlQueueCount),
                               static_cast<unsigned int>(CONTROL_QUEUE_CAPACITY),
                               controlCommandTypeToString(command.type));
        controlQueueDiagnostics.enqueueRejected++;
        unlockControlQueue();
        return false;
    }

    int slotIndex = -1;
    for (uint8_t i = 0; i < CONTROL_QUEUE_CAPACITY; i++) {
        ControlCommandSlot& slot = controlCommandSlots[i];
        if (!slot.allocated) {
            slotIndex = static_cast<int>(i);
            break;
        }

        if (!slot.queued && !slot.running && isTerminalControlCommandStatus(slot.result.status)) {
            slotIndex = static_cast<int>(i);
            break;
        }
    }

    if (slotIndex < 0) {
        UnifiedLogger::warning("[CTRLQ] enqueue rejected: no reusable slot available for command %s\n",
                               controlCommandTypeToString(command.type));
        controlQueueDiagnostics.enqueueRejected++;
        unlockControlQueue();
        return false;
    }

    ControlCommandSlot& slot = controlCommandSlots[slotIndex];
    slot = ControlCommandSlot();
    slot.allocated = true;
    slot.queued = true;
    slot.running = false;
    slot.command = command;
    slot.commandId = nextControlCommandId++;
    if (nextControlCommandId == 0) {
        nextControlCommandId = 1;
    }

    const unsigned long nowMs = millis();
    slot.queuedAtMs = nowMs;
    slot.result.commandId = slot.commandId;
    slot.result.status = ControlCommandStatus::QUEUED;
    slot.result.errorCode = 0;
    slot.result.updatedAtMs = nowMs;
    writeControlCommandMessage(slot.result, "queued");

    controlQueueIndices[controlQueueTail] = static_cast<uint8_t>(slotIndex);
    controlQueueTail = static_cast<uint8_t>((controlQueueTail + 1U) % CONTROL_QUEUE_CAPACITY);
    controlQueueCount++;
    controlQueueDiagnostics.enqueueAccepted++;
    if (controlQueueCount > controlQueueDiagnostics.maxQueueDepth) {
        controlQueueDiagnostics.maxQueueDepth = controlQueueCount;
    }
    const uint32_t currentInFlight = controlQueueCount + countRunningControlCommandsUnlocked();
    if (currentInFlight > controlQueueDiagnostics.maxInFlight) {
        controlQueueDiagnostics.maxInFlight = currentInFlight;
    }

    outCommandId = slot.commandId;
    unlockControlQueue();
    return true;
}

bool MainController::enqueueSettingsUpdateCommand(const String& jsonBody, uint32_t& outCommandId, String& outError) {
    outCommandId = 0;
    outError = "";
    if (!lockControlQueue()) {
        outError = "Control command queue lock unavailable";
        UnifiedLogger::error("[CTRLQ] settings enqueue failed: mutex lock unavailable\n");
        return false;
    }

    if (jsonBody.length() == 0) {
        outError = "No settings payload";
        UnifiedLogger::warning("[CTRLQ] settings enqueue rejected: empty payload\n");
        unlockControlQueue();
        return false;
    }

    if (pendingSettingsUpdate.active) {
        outError = "Settings update already pending";
        UnifiedLogger::warning("[CTRLQ] settings enqueue rejected: another settings update is pending (commandId=%lu)\n",
                               static_cast<unsigned long>(pendingSettingsUpdate.commandId));
        unlockControlQueue();
        return false;
    }

    pendingSettingsUpdate.active = true;
    pendingSettingsUpdate.token = nextSettingsUpdateToken++;
    if (nextSettingsUpdateToken == 0) {
        nextSettingsUpdateToken = 1;
    }
    pendingSettingsUpdate.commandId = 0;
    pendingSettingsUpdate.jsonBody = jsonBody;

    ControlCommand command;
    command.type = ControlCommandType::APPLY_SETTINGS_JSON;
    command.arg0 = static_cast<int32_t>(pendingSettingsUpdate.token);

    if (!enqueueControlCommand(command, outCommandId)) {
        clearPendingSettingsUpdate();
        outError = "Control command queue is full";
        UnifiedLogger::warning("[CTRLQ] settings enqueue rejected: control command queue full\n");
        unlockControlQueue();
        return false;
    }

    pendingSettingsUpdate.commandId = outCommandId;
    unlockControlQueue();
    return true;
}

bool MainController::getControlCommandResult(uint32_t commandId, ControlCommandResult& outResult) const {
    if (!lockControlQueue()) {
        return false;
    }
    const ControlCommandSlot* slot = findControlCommandSlotById(commandId);
    if (!slot) {
        unlockControlQueue();
        return false;
    }

    outResult = slot->result;
    unlockControlQueue();
    return true;
}

bool MainController::clearControlCommandResult(uint32_t commandId) {
    if (!lockControlQueue()) {
        return false;
    }
    ControlCommandSlot* slot = findControlCommandSlotById(commandId);
    if (!slot || slot->queued || slot->running) {
        unlockControlQueue();
        return false;
    }

    *slot = ControlCommandSlot();
    unlockControlQueue();
    return true;
}

bool MainController::isControlCommandQueueFull() const {
    if (!lockControlQueue()) {
        return true;
    }
    const bool isFull = controlQueueCount >= CONTROL_QUEUE_CAPACITY;
    unlockControlQueue();
    return isFull;
}

void MainController::resetControlCommandQueue() {
    if (!lockControlQueue()) {
        return;
    }
    for (uint8_t i = 0; i < CONTROL_QUEUE_CAPACITY; i++) {
        controlCommandSlots[i] = ControlCommandSlot();
        controlQueueIndices[i] = 0;
    }
    controlQueueHead = 0;
    controlQueueTail = 0;
    controlQueueCount = 0;
    controlQueueProcessing = false;
    if (nextControlCommandId == 0) {
        nextControlCommandId = 1;
    }
    clearPendingSettingsUpdate();
    unlockControlQueue();
}

void MainController::processControlCommandQueue() {
    if (!lockControlQueue()) {
        return;
    }
    if (controlQueueProcessing) {
        unlockControlQueue();
        return;
    }
    controlQueueProcessing = true;
    unlockControlQueue();

    uint8_t processed = 0;
    while (processed < CONTROL_COMMANDS_PER_LOOP) {
        if (!lockControlQueue()) {
            break;
        }
        if (controlQueueCount == 0) {
            unlockControlQueue();
            break;
        }

        const uint8_t slotIndex = controlQueueIndices[controlQueueHead];
        controlQueueHead = static_cast<uint8_t>((controlQueueHead + 1U) % CONTROL_QUEUE_CAPACITY);
        controlQueueCount--;
        processed++;

        if (slotIndex >= CONTROL_QUEUE_CAPACITY) {
            unlockControlQueue();
            continue;
        }

        ControlCommandSlot& slot = controlCommandSlots[slotIndex];
        if (!slot.allocated || !slot.queued) {
            unlockControlQueue();
            continue;
        }

        const unsigned long dequeueNowMs = millis();
        if ((dequeueNowMs - slot.queuedAtMs) > CONTROL_COMMAND_TIMEOUT_MS) {
            slot.queued = false;
            slot.running = false;
            releaseControlCommandResources(slot.command);
            slot.result.status = ControlCommandStatus::TIMED_OUT;
            slot.result.errorCode = -2;
            slot.result.updatedAtMs = dequeueNowMs;
            writeControlCommandMessage(slot.result, "queue timeout");
            UnifiedLogger::warning("[CTRLQ] queue timeout: command %lu (%s) waited %lu ms\n",
                                   static_cast<unsigned long>(slot.commandId),
                                   controlCommandTypeToString(slot.command.type),
                                   static_cast<unsigned long>(dequeueNowMs - slot.queuedAtMs));
            controlQueueDiagnostics.commandsTimedOut++;
            controlQueueDiagnostics.queueTimeouts++;
            unlockControlQueue();
            continue;
        }

        slot.queued = false;
        slot.running = true;
        slot.startedAtMs = dequeueNowMs;
        slot.result.status = ControlCommandStatus::RUNNING;
        slot.result.updatedAtMs = dequeueNowMs;
        writeControlCommandMessage(slot.result, "running");
        controlQueueDiagnostics.commandsStarted++;
        const uint32_t currentInFlight = controlQueueCount + countRunningControlCommandsUnlocked();
        if (currentInFlight > controlQueueDiagnostics.maxInFlight) {
            controlQueueDiagnostics.maxInFlight = currentInFlight;
        }
        const unsigned long queueWaitMs = dequeueNowMs - slot.queuedAtMs;
        controlQueueDiagnostics.totalQueueWaitMs += queueWaitMs;
        if (queueWaitMs > controlQueueDiagnostics.maxQueueWaitMs) {
            controlQueueDiagnostics.maxQueueWaitMs = queueWaitMs;
        }
        pushQueueWaitSampleUnlocked(queueWaitMs);
        ControlCommandSlot localSlot = slot;
        unlockControlQueue();
        executeControlCommand(localSlot, dequeueNowMs);
        if (!lockControlQueue()) {
            break;
        }

        if (!slot.allocated || slot.commandId != localSlot.commandId) {
            unlockControlQueue();
            continue;
        }

        slot.running = localSlot.running;
        slot.queued = localSlot.queued;
        slot.startedAtMs = localSlot.startedAtMs;
        slot.result = localSlot.result;

        // Future-proof: if a command ever stays running, put it back into the queue for cooperative progress.
        if (slot.running) {
            const unsigned long runningNowMs = millis();
            if ((runningNowMs - slot.startedAtMs) > CONTROL_COMMAND_TIMEOUT_MS) {
                slot.running = false;
                releaseControlCommandResources(slot.command);
                slot.result.status = ControlCommandStatus::TIMED_OUT;
                slot.result.errorCode = -3;
                slot.result.updatedAtMs = runningNowMs;
                writeControlCommandMessage(slot.result, "execution timeout");
                UnifiedLogger::warning("[CTRLQ] execution timeout: command %lu (%s) exceeded %lu ms\n",
                                       static_cast<unsigned long>(slot.commandId),
                                       controlCommandTypeToString(slot.command.type),
                                       static_cast<unsigned long>(CONTROL_COMMAND_TIMEOUT_MS));
                controlQueueDiagnostics.commandsTimedOut++;
                controlQueueDiagnostics.executionTimeouts++;
                unlockControlQueue();
                continue;
            }

            if (controlQueueCount < CONTROL_QUEUE_CAPACITY) {
                controlQueueIndices[controlQueueTail] = slotIndex;
                controlQueueTail = static_cast<uint8_t>((controlQueueTail + 1U) % CONTROL_QUEUE_CAPACITY);
                controlQueueCount++;
                if (controlQueueCount > controlQueueDiagnostics.maxQueueDepth) {
                    controlQueueDiagnostics.maxQueueDepth = controlQueueCount;
                }
                const uint32_t currentInFlight = controlQueueCount + countRunningControlCommandsUnlocked();
                if (currentInFlight > controlQueueDiagnostics.maxInFlight) {
                    controlQueueDiagnostics.maxInFlight = currentInFlight;
                }
            } else {
                slot.running = false;
                releaseControlCommandResources(slot.command);
                slot.result.status = ControlCommandStatus::FAILED;
                slot.result.errorCode = -4;
                slot.result.updatedAtMs = millis();
                writeControlCommandMessage(slot.result, "queue overflow while requeueing");
                UnifiedLogger::error("[CTRLQ] queue overflow while requeueing command %lu (%s)\n",
                                     static_cast<unsigned long>(slot.commandId),
                                     controlCommandTypeToString(slot.command.type));
            }
        }
        if (!slot.running && isTerminalControlCommandStatus(slot.result.status)) {
            const unsigned long executionMs = millis() - slot.startedAtMs;
            controlQueueDiagnostics.totalExecutionMs += executionMs;
            if (executionMs > controlQueueDiagnostics.maxExecutionMs) {
                controlQueueDiagnostics.maxExecutionMs = executionMs;
            }
            pushExecutionSampleUnlocked(executionMs);
            if (slot.result.status == ControlCommandStatus::SUCCEEDED) {
                controlQueueDiagnostics.commandsSucceeded++;
            } else if (slot.result.status == ControlCommandStatus::FAILED) {
                controlQueueDiagnostics.commandsFailed++;
            } else if (slot.result.status == ControlCommandStatus::TIMED_OUT) {
                controlQueueDiagnostics.commandsTimedOut++;
            }
        }
        unlockControlQueue();
    }

    if (lockControlQueue()) {
        controlQueueProcessing = false;
        unlockControlQueue();
    } else {
        controlQueueProcessing = false;
    }
}

bool MainController::executeControlCommand(ControlCommandSlot& slot, unsigned long nowMs) {
    switch (slot.command.type) {
        case ControlCommandType::NOOP:
            slot.running = false;
            slot.result.status = ControlCommandStatus::SUCCEEDED;
            slot.result.errorCode = 0;
            slot.result.updatedAtMs = nowMs;
            writeControlCommandMessage(slot.result, "noop completed");
            return true;
        case ControlCommandType::APPLY_SETTINGS_JSON: {
            const uint32_t token = static_cast<uint32_t>(slot.command.arg0);
            String settingsPayload;
            bool hasMatchingPendingPayload = false;
            if (lockControlQueue()) {
                hasMatchingPendingPayload =
                    pendingSettingsUpdate.active &&
                    pendingSettingsUpdate.token == token &&
                    pendingSettingsUpdate.commandId == slot.commandId;
                if (hasMatchingPendingPayload) {
                    settingsPayload = pendingSettingsUpdate.jsonBody;
                }
                unlockControlQueue();
            }

            if (!hasMatchingPendingPayload) {
                slot.running = false;
                slot.result.status = ControlCommandStatus::FAILED;
                slot.result.errorCode = -5;
                slot.result.updatedAtMs = nowMs;
                writeControlCommandMessage(slot.result, "missing staged settings payload");
                UnifiedLogger::warning("[CTRLQ] settings apply failed: missing staged payload (commandId=%lu)\n",
                                       static_cast<unsigned long>(slot.commandId));
                releaseControlCommandResources(slot.command);
                return false;
            }

            const bool updateResult = configManager.updateSettingsFromJson(settingsPayload);
            if (updateResult) {
                const Settings& settings = configManager.getSettings();
                if (scheduleManager) {
                    scheduleManager->reloadSchedules();
                    scheduleManager->updateSafetySettings();
                }
                if (relayManager) {
                    relayManager->setRelayTimeout(settings.relay.timeout_ms);
                }

                slot.running = false;
                slot.result.status = ControlCommandStatus::SUCCEEDED;
                slot.result.errorCode = 0;
                slot.result.updatedAtMs = nowMs;
                writeControlCommandMessage(slot.result, "settings update applied");
                UnifiedLogger::info("[CTRLQ] settings update applied successfully (commandId=%lu)\n",
                                    static_cast<unsigned long>(slot.commandId));
                releaseControlCommandResources(slot.command);
                return true;
            }

            slot.running = false;
            slot.result.status = ControlCommandStatus::FAILED;
            slot.result.errorCode = -6;
            slot.result.updatedAtMs = nowMs;
            writeControlCommandMessage(slot.result, "settings validation or save failed");
            UnifiedLogger::warning("[CTRLQ] settings update failed validation/save (commandId=%lu)\n",
                                   static_cast<unsigned long>(slot.commandId));
            releaseControlCommandResources(slot.command);
            return false;
        }
        case ControlCommandType::RELAY_SET_MODE: {
            if (!relayManager || !relayManager->isInitialized()) {
                slot.running = false;
                slot.result.status = ControlCommandStatus::FAILED;
                slot.result.errorCode = -8;
                slot.result.updatedAtMs = nowMs;
                writeControlCommandMessage(slot.result, "relay manager not initialized");
                return false;
            }

            const int modeValue = static_cast<int>(slot.command.arg0);
            if (modeValue < static_cast<int>(RELAY_OFF) || modeValue > static_cast<int>(RELAY_HIGH)) {
                slot.running = false;
                slot.result.status = ControlCommandStatus::FAILED;
                slot.result.errorCode = -9;
                slot.result.updatedAtMs = nowMs;
                writeControlCommandMessage(slot.result, "invalid relay mode");
                return false;
            }

            const RelayMode mode = static_cast<RelayMode>(modeValue);
            const String source = slot.command.textArg[0] != '\0' ? String(slot.command.textArg) : String("queued");
            const bool isSafetyOperation = slot.command.arg1 != 0;
            const bool success = relayManager->setModeWithVerification(mode, source, isSafetyOperation);

            slot.running = false;
            slot.result.status = success ? ControlCommandStatus::SUCCEEDED : ControlCommandStatus::FAILED;
            slot.result.errorCode = success ? 0 : -10;
            slot.result.updatedAtMs = nowMs;
            writeControlCommandMessage(slot.result, success ? "relay mode set" : "relay mode change failed");
            return success;
        }
        case ControlCommandType::RELAY_TIMER_START: {
            if (!relayManager || !relayManager->isInitialized()) {
                slot.running = false;
                slot.result.status = ControlCommandStatus::FAILED;
                slot.result.errorCode = -8;
                slot.result.updatedAtMs = nowMs;
                writeControlCommandMessage(slot.result, "relay manager not initialized");
                return false;
            }

            const int durationMinutes = static_cast<int>(slot.command.arg0);
            if (durationMinutes <= 0) {
                slot.running = false;
                slot.result.status = ControlCommandStatus::FAILED;
                slot.result.errorCode = -12;
                slot.result.updatedAtMs = nowMs;
                writeControlCommandMessage(slot.result, "invalid timer duration");
                return false;
            }

            relayManager->startTimer(durationMinutes);
            slot.running = false;
            slot.result.status = ControlCommandStatus::SUCCEEDED;
            slot.result.errorCode = 0;
            slot.result.updatedAtMs = nowMs;
            writeControlCommandMessage(slot.result, "relay timer started");
            return true;
        }
        case ControlCommandType::RELAY_TIMER_STOP: {
            if (!relayManager || !relayManager->isInitialized()) {
                slot.running = false;
                slot.result.status = ControlCommandStatus::FAILED;
                slot.result.errorCode = -8;
                slot.result.updatedAtMs = nowMs;
                writeControlCommandMessage(slot.result, "relay manager not initialized");
                return false;
            }

            relayManager->stopTimer();
            slot.running = false;
            slot.result.status = ControlCommandStatus::SUCCEEDED;
            slot.result.errorCode = 0;
            slot.result.updatedAtMs = nowMs;
            writeControlCommandMessage(slot.result, "relay timer stopped");
            return true;
        }
        case ControlCommandType::DS18_CLEAR_SCAN_RESULT: {
            if (!ds18b20Manager) {
                slot.running = false;
                slot.result.status = ControlCommandStatus::FAILED;
                slot.result.errorCode = -13;
                slot.result.updatedAtMs = nowMs;
                writeControlCommandMessage(slot.result, "ds18 manager not available");
                return false;
            }

            ds18b20Manager->clearScanResult();
            slot.running = false;
            slot.result.status = ControlCommandStatus::SUCCEEDED;
            slot.result.errorCode = 0;
            slot.result.updatedAtMs = nowMs;
            writeControlCommandMessage(slot.result, "scan result cleared");
            return true;
        }
        case ControlCommandType::DS18_REQUEST_SCAN: {
            if (!ds18b20Manager) {
                slot.running = false;
                slot.result.status = ControlCommandStatus::FAILED;
                slot.result.errorCode = -13;
                slot.result.updatedAtMs = nowMs;
                writeControlCommandMessage(slot.result, "ds18 manager not available");
                return false;
            }

            const bool fullScan = slot.command.arg0 != 0;
            const bool queued = ds18b20Manager->requestScan(fullScan);
            slot.running = false;
            slot.result.status = queued ? ControlCommandStatus::SUCCEEDED : ControlCommandStatus::FAILED;
            slot.result.errorCode = queued ? 0 : -14;
            slot.result.updatedAtMs = nowMs;
            writeControlCommandMessage(slot.result, queued ? "scan request queued" : "scan request rejected");
            if (!queued) {
                UnifiedLogger::warning("[CTRLQ] DS18 scan request rejected (fullScan=%s, commandId=%lu)\n",
                                       fullScan ? "true" : "false",
                                       static_cast<unsigned long>(slot.commandId));
            }
            return queued;
        }
        case ControlCommandType::OTA_REFRESH_CONFIG_STATUS: {
            otaManager.refreshConfigStatus();
            slot.running = false;
            slot.result.status = ControlCommandStatus::SUCCEEDED;
            slot.result.errorCode = 0;
            slot.result.updatedAtMs = nowMs;
            writeControlCommandMessage(slot.result, "ota config status refreshed");
            return true;
        }
        case ControlCommandType::OTA_ARM_WINDOW: {
            if (!wifiManager.isConnected()) {
                slot.running = false;
                slot.result.status = ControlCommandStatus::FAILED;
                slot.result.errorCode = -15;
                slot.result.updatedAtMs = nowMs;
                writeControlCommandMessage(slot.result, "WiFi not connected");
                return false;
            }

            String errorMessage;
            const bool armed = otaManager.armFromWeb(errorMessage);
            slot.running = false;
            slot.result.status = armed ? ControlCommandStatus::SUCCEEDED : ControlCommandStatus::FAILED;
            slot.result.errorCode = armed ? 0 : -16;
            slot.result.updatedAtMs = nowMs;
            writeControlCommandMessage(
                slot.result,
                armed ? "ota window armed" : (errorMessage.length() ? errorMessage.c_str() : "ota arm failed"));
            if (armed) {
                UnifiedLogger::info("[CTRLQ] OTA arm command succeeded (commandId=%lu)\n",
                                    static_cast<unsigned long>(slot.commandId));
            } else {
                UnifiedLogger::warning("[CTRLQ] OTA arm command failed: %s (commandId=%lu)\n",
                                       errorMessage.length() ? errorMessage.c_str() : "ota arm failed",
                                       static_cast<unsigned long>(slot.commandId));
            }
            return armed;
        }
        case ControlCommandType::MONITORING_RESET_STATS: {
            resetPerformanceStats();
            slot.running = false;
            slot.result.status = ControlCommandStatus::SUCCEEDED;
            slot.result.errorCode = 0;
            slot.result.updatedAtMs = nowMs;
            writeControlCommandMessage(slot.result, "monitoring stats reset");
            return true;
        }
        case ControlCommandType::RELAY_CLEAR_LOGS: {
            const bool success = RelayLogger::clearLogs();
            slot.running = false;
            slot.result.status = success ? ControlCommandStatus::SUCCEEDED : ControlCommandStatus::FAILED;
            slot.result.errorCode = success ? 0 : -17;
            slot.result.updatedAtMs = nowMs;
            writeControlCommandMessage(slot.result, success ? "relay logs cleared" : "relay log clear failed");
            return success;
        }
        case ControlCommandType::ERROR_CLEAR_LOGS: {
            const bool success = ErrorHandler::clearErrorHistory();
            slot.running = false;
            slot.result.status = success ? ControlCommandStatus::SUCCEEDED : ControlCommandStatus::FAILED;
            slot.result.errorCode = success ? 0 : -18;
            slot.result.updatedAtMs = nowMs;
            writeControlCommandMessage(slot.result, success ? "error logs cleared" : "error log clear failed");
            return success;
        }
        case ControlCommandType::MONITORING_RESET_QUEUE_DIAGNOSTICS: {
            resetControlQueueDiagnostics();
            slot.running = false;
            slot.result.status = ControlCommandStatus::SUCCEEDED;
            slot.result.errorCode = 0;
            slot.result.updatedAtMs = nowMs;
            writeControlCommandMessage(slot.result, "queue diagnostics reset");
            return true;
        }
        case ControlCommandType::NONE:
        default:
            slot.running = false;
            slot.result.status = ControlCommandStatus::FAILED;
            slot.result.errorCode = -1;
            slot.result.updatedAtMs = nowMs;
            writeControlCommandMessage(slot.result, "unsupported command");
            return false;
    }
}

void MainController::clearPendingSettingsUpdate() {
    if (!lockControlQueue()) {
        return;
    }
    pendingSettingsUpdate.active = false;
    pendingSettingsUpdate.token = 0;
    pendingSettingsUpdate.commandId = 0;
    pendingSettingsUpdate.jsonBody = "";
    unlockControlQueue();
}

void MainController::releaseControlCommandResources(const ControlCommand& command) {
    if (!lockControlQueue()) {
        return;
    }
    if (command.type != ControlCommandType::APPLY_SETTINGS_JSON) {
        unlockControlQueue();
        return;
    }

    const uint32_t token = static_cast<uint32_t>(command.arg0);
    if (pendingSettingsUpdate.active && pendingSettingsUpdate.token == token) {
        clearPendingSettingsUpdate();
    }
    unlockControlQueue();
}

void MainController::resetControlQueueDiagnostics() {
    if (!lockControlQueue()) {
        return;
    }
    resetControlQueueDiagnosticsUnlocked();
    unlockControlQueue();
}

void MainController::resetControlQueueDiagnosticsUnlocked() {
    controlQueueDiagnostics = ControlQueueDiagnosticsData();
}

void MainController::pushQueueWaitSampleUnlocked(uint32_t queueWaitMs) {
    const uint16_t sample = static_cast<uint16_t>(std::min<uint32_t>(queueWaitMs, UINT16_MAX));
    const uint16_t index = controlQueueDiagnostics.queueWaitSampleWriteIndex;
    controlQueueDiagnostics.queueWaitSamples[index] = sample;
    controlQueueDiagnostics.queueWaitSampleWriteIndex =
        static_cast<uint16_t>((index + 1U) % (sizeof(controlQueueDiagnostics.queueWaitSamples) / sizeof(controlQueueDiagnostics.queueWaitSamples[0])));
    if (controlQueueDiagnostics.queueWaitSampleCount < (sizeof(controlQueueDiagnostics.queueWaitSamples) / sizeof(controlQueueDiagnostics.queueWaitSamples[0]))) {
        controlQueueDiagnostics.queueWaitSampleCount++;
    }
}

void MainController::pushExecutionSampleUnlocked(uint32_t executionMs) {
    const uint16_t sample = static_cast<uint16_t>(std::min<uint32_t>(executionMs, UINT16_MAX));
    const uint16_t index = controlQueueDiagnostics.executionSampleWriteIndex;
    controlQueueDiagnostics.executionSamples[index] = sample;
    controlQueueDiagnostics.executionSampleWriteIndex =
        static_cast<uint16_t>((index + 1U) % (sizeof(controlQueueDiagnostics.executionSamples) / sizeof(controlQueueDiagnostics.executionSamples[0])));
    if (controlQueueDiagnostics.executionSampleCount < (sizeof(controlQueueDiagnostics.executionSamples) / sizeof(controlQueueDiagnostics.executionSamples[0]))) {
        controlQueueDiagnostics.executionSampleCount++;
    }
}

uint32_t MainController::computeP95SampleMs(const uint16_t* samples, uint16_t count) {
    if (!samples || count == 0) {
        return 0;
    }

    uint16_t sorted[128] = {0};
    const uint16_t boundedCount = static_cast<uint16_t>(std::min<size_t>(count, sizeof(sorted) / sizeof(sorted[0])));
    memcpy(sorted, samples, static_cast<size_t>(boundedCount) * sizeof(uint16_t));
    std::sort(sorted, sorted + boundedCount);

    // Nearest-rank percentile (P95), rank 1..N.
    size_t rank = ((static_cast<size_t>(boundedCount) * 95U) + 99U) / 100U;
    if (rank == 0) {
        rank = 1;
    }
    if (rank > boundedCount) {
        rank = boundedCount;
    }
    return static_cast<uint32_t>(sorted[rank - 1U]);
}

bool MainController::lockControlQueue() const {
#if defined(ARDUINO_ARCH_ESP32)
    if (!controlQueueMutex) {
        return true;
    }
    return xSemaphoreTakeRecursive(controlQueueMutex, portMAX_DELAY) == pdTRUE;
#else
    return true;
#endif
}

void MainController::unlockControlQueue() const {
#if defined(ARDUINO_ARCH_ESP32)
    if (controlQueueMutex) {
        xSemaphoreGiveRecursive(controlQueueMutex);
    }
#endif
}

uint32_t MainController::countRunningControlCommandsUnlocked() const {
    uint32_t runningCount = 0;
    for (uint8_t i = 0; i < CONTROL_QUEUE_CAPACITY; i++) {
        const ControlCommandSlot& slot = controlCommandSlots[i];
        if (slot.allocated && slot.running) {
            runningCount++;
        }
    }
    return runningCount;
}

MainController::ControlCommandSlot* MainController::findControlCommandSlotById(uint32_t commandId) {
    if (commandId == 0) {
        return nullptr;
    }

    for (uint8_t i = 0; i < CONTROL_QUEUE_CAPACITY; i++) {
        ControlCommandSlot& slot = controlCommandSlots[i];
        if (slot.allocated && slot.commandId == commandId) {
            return &slot;
        }
    }

    return nullptr;
}

const MainController::ControlCommandSlot* MainController::findControlCommandSlotById(uint32_t commandId) const {
    if (commandId == 0) {
        return nullptr;
    }

    for (uint8_t i = 0; i < CONTROL_QUEUE_CAPACITY; i++) {
        const ControlCommandSlot& slot = controlCommandSlots[i];
        if (slot.allocated && slot.commandId == commandId) {
            return &slot;
        }
    }

    return nullptr;
}

bool MainController::isTerminalControlCommandStatus(ControlCommandStatus status) {
    return status == ControlCommandStatus::SUCCEEDED ||
           status == ControlCommandStatus::FAILED ||
           status == ControlCommandStatus::TIMED_OUT;
}

void MainController::writeControlCommandMessage(ControlCommandResult& result, const char* message) {
    if (!message) {
        result.message[0] = '\0';
        return;
    }

    strncpy(result.message, message, sizeof(result.message) - 1);
    result.message[sizeof(result.message) - 1] = '\0';
}

void MainController::lockPerformanceStats() const {
#if defined(ARDUINO_ARCH_ESP32)
    portENTER_CRITICAL(&performanceStatsLock);
#endif
}

void MainController::unlockPerformanceStats() const {
#if defined(ARDUINO_ARCH_ESP32)
    portEXIT_CRITICAL(&performanceStatsLock);
#endif
}

MainController::PerformanceStatsSnapshot MainController::getPerformanceStatsSnapshot() const {
    PerformanceStatsSnapshot snapshot;
    lockPerformanceStats();
    snapshot.mainLoopStats = mainLoopStats;
    for (int i = 0; i < COMPONENT_COUNT; i++) {
        snapshot.componentStats[i] = componentStats[i];
    }
    snapshot.relayLoggerStats = relayLoggerStats;
    unlockPerformanceStats();
    return snapshot;
}

MainController::ControlQueueDiagnosticsSnapshot MainController::getControlQueueDiagnosticsSnapshot() const {
    ControlQueueDiagnosticsSnapshot snapshot;
    uint16_t queueWaitSamples[128] = {0};
    uint16_t executionSamples[128] = {0};
    uint16_t queueWaitSampleCount = 0;
    uint16_t executionSampleCount = 0;

    if (!lockControlQueue()) {
        return snapshot;
    }
    snapshot.enqueueAccepted = controlQueueDiagnostics.enqueueAccepted;
    snapshot.enqueueRejected = controlQueueDiagnostics.enqueueRejected;
    snapshot.commandsStarted = controlQueueDiagnostics.commandsStarted;
    snapshot.commandsSucceeded = controlQueueDiagnostics.commandsSucceeded;
    snapshot.commandsFailed = controlQueueDiagnostics.commandsFailed;
    snapshot.commandsTimedOut = controlQueueDiagnostics.commandsTimedOut;
    snapshot.queueTimeouts = controlQueueDiagnostics.queueTimeouts;
    snapshot.executionTimeouts = controlQueueDiagnostics.executionTimeouts;
    snapshot.maxQueueDepth = controlQueueDiagnostics.maxQueueDepth;
    snapshot.maxQueueWaitMs = controlQueueDiagnostics.maxQueueWaitMs;
    snapshot.totalQueueWaitMs = controlQueueDiagnostics.totalQueueWaitMs;
    snapshot.maxExecutionMs = controlQueueDiagnostics.maxExecutionMs;
    snapshot.totalExecutionMs = controlQueueDiagnostics.totalExecutionMs;
    snapshot.currentQueueDepth = controlQueueCount;
    snapshot.runningCount = countRunningControlCommandsUnlocked();
    snapshot.currentInFlight = snapshot.currentQueueDepth + snapshot.runningCount;
    snapshot.maxInFlight = controlQueueDiagnostics.maxInFlight;
    snapshot.processingActive = controlQueueProcessing;
    queueWaitSampleCount = controlQueueDiagnostics.queueWaitSampleCount;
    executionSampleCount = controlQueueDiagnostics.executionSampleCount;
    if (queueWaitSampleCount > 0) {
        memcpy(queueWaitSamples,
               controlQueueDiagnostics.queueWaitSamples,
               static_cast<size_t>(queueWaitSampleCount) * sizeof(uint16_t));
    }
    if (executionSampleCount > 0) {
        memcpy(executionSamples,
               controlQueueDiagnostics.executionSamples,
               static_cast<size_t>(executionSampleCount) * sizeof(uint16_t));
    }
    unlockControlQueue();
    snapshot.p95QueueWaitMs = computeP95SampleMs(queueWaitSamples, queueWaitSampleCount);
    snapshot.p95ExecutionMs = computeP95SampleMs(executionSamples, executionSampleCount);
    return snapshot;
}

MainController::MemoryDiagnosticsSnapshot MainController::getMemoryDiagnosticsSnapshot() const {
    MemoryDiagnosticsSnapshot snapshot;
    lockPerformanceStats();
    snapshot.totalHeap = memoryDiagnostics.totalHeap;
    snapshot.freeHeapCurrent = memoryDiagnostics.freeHeapCurrent;
    snapshot.freeHeapMin = memoryDiagnostics.freeHeapMin;
    snapshot.freeHeapMax = memoryDiagnostics.freeHeapMax;
    snapshot.maxAllocHeapCurrent = memoryDiagnostics.maxAllocHeapCurrent;
    snapshot.maxAllocHeapMin = memoryDiagnostics.maxAllocHeapMin;
    snapshot.totalPsram = memoryDiagnostics.totalPsram;
    snapshot.freePsramCurrent = memoryDiagnostics.freePsramCurrent;
    snapshot.freePsramMin = memoryDiagnostics.freePsramMin;
    snapshot.freePsramMax = memoryDiagnostics.freePsramMax;
    snapshot.maxAllocPsramCurrent = memoryDiagnostics.maxAllocPsramCurrent;
    snapshot.maxAllocPsramMin = memoryDiagnostics.maxAllocPsramMin;
    unlockPerformanceStats();
    return snapshot;
}

MainController::WiFiStabilitySnapshot MainController::getWiFiStabilitySnapshot() const {
    WiFiStabilitySnapshot snapshot;
    const bool connected = wifiManager.isConnected();
    snapshot.connected = connected;
    snapshot.statusCode = static_cast<int32_t>(WiFi.status());
    snapshot.rssi = connected ? static_cast<int32_t>(WiFi.RSSI()) : 0;

    const unsigned long nowMs = millis();
    lockPerformanceStats();
    snapshot.reconnectTransitions = wifiStability.reconnectTransitions;
    snapshot.disconnectTransitions = wifiStability.disconnectTransitions;
    snapshot.lastDisconnectStatusCode = wifiStability.lastDisconnectStatusCode;
    if (wifiStability.lastReconnectMs > 0 && nowMs >= wifiStability.lastReconnectMs) {
        snapshot.lastReconnectAgeMs = nowMs - wifiStability.lastReconnectMs;
    }
    if (wifiStability.lastDisconnectMs > 0 && nowMs >= wifiStability.lastDisconnectMs) {
        snapshot.lastDisconnectAgeMs = nowMs - wifiStability.lastDisconnectMs;
    }
    unlockPerformanceStats();
    return snapshot;
}

void MainController::updateMemoryDiagnosticsUnlocked(uint32_t freeHeap,
                                                     uint32_t maxAllocHeap,
                                                     uint32_t totalHeap,
                                                     uint32_t freePsram,
                                                     uint32_t maxAllocPsram,
                                                     uint32_t totalPsram) {
    if (!memoryDiagnostics.initialized) {
        memoryDiagnostics.initialized = true;
        memoryDiagnostics.totalHeap = totalHeap;
        memoryDiagnostics.freeHeapCurrent = freeHeap;
        memoryDiagnostics.freeHeapMin = freeHeap;
        memoryDiagnostics.freeHeapMax = freeHeap;
        memoryDiagnostics.maxAllocHeapCurrent = maxAllocHeap;
        memoryDiagnostics.maxAllocHeapMin = maxAllocHeap;
        memoryDiagnostics.totalPsram = totalPsram;
        memoryDiagnostics.freePsramCurrent = freePsram;
        memoryDiagnostics.freePsramMin = freePsram;
        memoryDiagnostics.freePsramMax = freePsram;
        memoryDiagnostics.maxAllocPsramCurrent = maxAllocPsram;
        memoryDiagnostics.maxAllocPsramMin = maxAllocPsram;
        return;
    }

    memoryDiagnostics.totalHeap = totalHeap;
    memoryDiagnostics.freeHeapCurrent = freeHeap;
    if (freeHeap < memoryDiagnostics.freeHeapMin) {
        memoryDiagnostics.freeHeapMin = freeHeap;
    }
    if (freeHeap > memoryDiagnostics.freeHeapMax) {
        memoryDiagnostics.freeHeapMax = freeHeap;
    }

    memoryDiagnostics.maxAllocHeapCurrent = maxAllocHeap;
    if (maxAllocHeap < memoryDiagnostics.maxAllocHeapMin) {
        memoryDiagnostics.maxAllocHeapMin = maxAllocHeap;
    }

    memoryDiagnostics.totalPsram = totalPsram;
    memoryDiagnostics.freePsramCurrent = freePsram;
    if (freePsram < memoryDiagnostics.freePsramMin) {
        memoryDiagnostics.freePsramMin = freePsram;
    }
    if (freePsram > memoryDiagnostics.freePsramMax) {
        memoryDiagnostics.freePsramMax = freePsram;
    }

    memoryDiagnostics.maxAllocPsramCurrent = maxAllocPsram;
    if (maxAllocPsram < memoryDiagnostics.maxAllocPsramMin) {
        memoryDiagnostics.maxAllocPsramMin = maxAllocPsram;
    }
}

void MainController::updateMemoryDiagnostics() {
    const uint32_t totalHeap = ESP.getHeapSize();
    const uint32_t freeHeap = ESP.getFreeHeap();
    const uint32_t maxAllocHeap = ESP.getMaxAllocHeap();
    const uint32_t totalPsram = ESP.getPsramSize();
    const uint32_t freePsram = ESP.getFreePsram();
    const uint32_t maxAllocPsram = ESP.getMaxAllocPsram();

    lockPerformanceStats();
    updateMemoryDiagnosticsUnlocked(freeHeap, maxAllocHeap, totalHeap, freePsram, maxAllocPsram, totalPsram);
    unlockPerformanceStats();
}

void MainController::resetMemoryDiagnosticsUnlocked() {
    memoryDiagnostics = MemoryDiagnosticsData();
    updateMemoryDiagnosticsUnlocked(
        ESP.getFreeHeap(),
        ESP.getMaxAllocHeap(),
        ESP.getHeapSize(),
        ESP.getFreePsram(),
        ESP.getMaxAllocPsram(),
        ESP.getPsramSize());
}

void MainController::updateComponentStats(int index, unsigned long duration) {
    if (index < 0 || index >= COMPONENT_COUNT) {
        return;
    }
    lockPerformanceStats();
    PerformanceStats& stats = componentStats[index];
    stats.totalTime += duration;
    stats.callCount++;

    if (duration < stats.minTime) stats.minTime = duration;
    if (duration > stats.maxTime) stats.maxTime = duration;
    unlockPerformanceStats();
}

void MainController::updateMainLoopStats(unsigned long duration) {
    lockPerformanceStats();
    mainLoopStats.totalTime += duration;
    mainLoopStats.callCount++;

    if (duration < mainLoopStats.minTime) mainLoopStats.minTime = duration;
    if (duration > mainLoopStats.maxTime) mainLoopStats.maxTime = duration;
    unlockPerformanceStats();
}

void MainController::timeComponentUpdate(int componentIndex, std::function<void()> updateFunction) {
    unsigned long startTime = micros();
    updateFunction();
    unsigned long durationMs = (micros() - startTime) / 1000;
    updateComponentStats(componentIndex, durationMs);
}

void MainController::resetPerformanceStats() {
    lockPerformanceStats();
    mainLoopStats.reset();
    for (int i = 0; i < COMPONENT_COUNT; i++) {
        componentStats[i].reset();
    }
    resetMemoryDiagnosticsUnlocked();

    // Reset file logger stats
    resetRelayLoggerStatsUnlocked();
    unlockPerformanceStats();
}

void MainController::resetRelayLoggerStats() {
    lockPerformanceStats();
    resetRelayLoggerStatsUnlocked();
    unlockPerformanceStats();
}

void MainController::resetRelayLoggerStatsUnlocked() {
    relayLoggerStats.base.saveTime.reset();
}

void MainController::updateRelayLoggerStats(int operationIndex, unsigned long duration) {
    lockPerformanceStats();
    switch (operationIndex) {
        case 1: // Relay save
            updateFileLoggerStatUnlocked(relayLoggerStats.base.saveTime, duration);
            break;
    }
    unlockPerformanceStats();
}


void MainController::updateFileLoggerStat(PerformanceStats& stats, unsigned long duration) {
    lockPerformanceStats();
    updateFileLoggerStatUnlocked(stats, duration);
    unlockPerformanceStats();
}

void MainController::updateFileLoggerStatUnlocked(PerformanceStats& stats, unsigned long duration) {
    stats.totalTime += duration;
    stats.callCount++;

    if (duration < stats.minTime) stats.minTime = duration;
    if (duration > stats.maxTime) stats.maxTime = duration;
}


void MainController::startSchedulerEvaluation() {
    schedulerEvalStart = micros();
}

void MainController::endSchedulerEvaluation() {
    if (schedulerEvalStart > 0) {
        unsigned long duration = micros() - schedulerEvalStart;
        updateComponentStats(SCHEDULE_INDEX, duration / 1000);
        schedulerEvalStart = 0;
    }
}

void MainController::initializeSensors() {
    const Settings& settings = configManager.getSettings();

    // Initialize DS18B20 sensors if enabled
    if (settings.sensors.ds18b20_enabled) {
        ds18b20Manager = new DS18B20SensorManager(settings.sensors.one_wire_pins, settings.sensors.one_wire_bus_count, settings.sensors.viable_scan_pins);
        ds18b20Manager->begin();
        ds18b20Manager->setSensorAddresses(settings.sensors.sensor_addresses, settings.sensors.sensor_count);
        ds18b20Manager->setSensorReadInterval(settings.sensors.ds18b20_measurement_interval * 1000);
        ds18b20Manager->setMainController(this); // Set MainController for performance monitoring

        // Very early conversion request: start the first conversion as soon as sensors are initialized
        // so the first real readout can happen before MQTT starts publishing.
        ds18b20Manager->updateTemperatures();
    } else {
        ds18b20Manager = nullptr;
        UnifiedLogger::info("DS18B20 sensors disabled - skipping initialization\n");
    }

    // Initialize SHT4x sensor if enabled
    if (settings.sensors.sht4x_enabled) {
        initializeSHT4xSensor();
    }

    if (settings.sensors.max31855k_enabled) {
        max31855kManager = new MAX31855KSensorManager(
            settings.sensors.max31855k_do_pin,
            settings.sensors.max31855k_clk_pin,
            settings.sensors.max31855k_cs_pin
        );
        max31855kManager->begin();
    } else {
        max31855kManager = nullptr;
    }

    // Initialize chip temperature sensor (always available)
    chipTempManager = new ChipTempSensorManager();
    if (chipTempManager->begin()) {
        UnifiedLogger::info("Chip temperature sensor initialized successfully\n");
    } else {
        UnifiedLogger::error("Failed to initialize chip temperature sensor\n");
        delete chipTempManager;
        chipTempManager = nullptr;
    }
}

void MainController::initializeSHT4xSensor() {
    const Settings& settings = configManager.getSettings();

    sht4xManager = new SHT4xSensorManager(
        settings.sensors.sht4x_sda_pin,
        settings.sensors.sht4x_scl_pin,
        settings.sensors.sht4x_i2c_address
    );

    if (sht4xManager->begin()) {
        // Set MainController reference for performance monitoring
        sht4xManager->setMainController(this);
        
        // Configure sensor precision and heater
        Adafruit_SHT4x* sensor = sht4xManager->getSensor();
        if (sensor) {
            switch(settings.sensors.sht4x_precision) {
                case 0: sensor->setPrecision(SHT4X_LOW_PRECISION); break;
                case 1: sensor->setPrecision(SHT4X_MED_PRECISION); break;
                case 2: sensor->setPrecision(SHT4X_HIGH_PRECISION); break;
            }

            // Map heater power and duration to appropriate constants
            auto heaterSetting = SHT4X_NO_HEATER;
            if (settings.sensors.sht4x_heater_power > 0) {
                if (settings.sensors.sht4x_heater_duration == 0) {
                    // Short duration (0.1s)
                    switch(settings.sensors.sht4x_heater_power) {
                        case 1: heaterSetting = SHT4X_LOW_HEATER_100MS; break;
                        case 2: heaterSetting = SHT4X_MED_HEATER_100MS; break;
                        case 3: heaterSetting = SHT4X_HIGH_HEATER_100MS; break;
                    }
                } else {
                    // Long duration (1s)
                    switch(settings.sensors.sht4x_heater_power) {
                        case 1: heaterSetting = SHT4X_LOW_HEATER_1S; break;
                        case 2: heaterSetting = SHT4X_MED_HEATER_1S; break;
                        case 3: heaterSetting = SHT4X_HIGH_HEATER_1S; break;
                    }
                }
            }
            sensor->setHeater(heaterSetting);
        }

        // Configure heater settings
        sht4xManager->setHeaterConfiguration(
            settings.sensors.sht4x_heater_power,
            settings.sensors.sht4x_heater_duration
        );

        // Configure heating strategy if enabled
        if (settings.sensors.sht4x_heating_strategy > 0) {
            sht4xManager->setHeatingStrategy(
                settings.sensors.sht4x_heating_strategy,
                settings.sensors.sht4x_measurement_interval
            );
        }

        UnifiedLogger::info("SHT4x sensor initialized successfully\n");
    } else {
        UnifiedLogger::error("Failed to initialize SHT4x sensor\n");
        delete sht4xManager;
        sht4xManager = nullptr;
    }
}

void MainController::initializeRelayManager() {
    const Settings& settings = configManager.getSettings();

    if (settings.relay.enabled) {
        relayManager = new RelayManager();
        
        // Initialize relay logger (static class)
        RelayLogger::begin();
        RelayLogger::setMainController(this);
        
        relayManager->begin(
            settings.relay.pins_low,
            settings.relay.pins_mid,
            settings.relay.pins_high
        );

        // Set safety timeout configuration (but don't override if scheduler immunity is active)
        if (!settings.schedules.enabled) {
            relayManager->setRelayTimeout(settings.relay.timeout_ms);
        }

        // Set default state to OFF with verification
        relayManager->setModeWithVerification(RELAY_OFF, "startup");

        // Set MainController reference for performance monitoring
        relayManager->setMainController(this);

        auto joinPins = [](const std::vector<int>& pins) {
            String result;
            for (size_t i = 0; i < pins.size(); i++) {
                if (i > 0) {
                    result += ",";
                }
                result += String(pins[i]);
            }
            return result;
        };

        UnifiedLogger::info("RelayManager initialized successfully\n");
        UnifiedLogger::info("Relay LOW pins: %s | MID pins: %s | HIGH pins: %s | Safety Timeout: %lu ms\n",
                            joinPins(settings.relay.pins_low).c_str(),
                            joinPins(settings.relay.pins_mid).c_str(),
                            joinPins(settings.relay.pins_high).c_str(),
                            settings.relay.timeout_ms);
    } else {
        UnifiedLogger::info("Relay functionality disabled in configuration\n");
        relayManager = nullptr;
    }
}

void MainController::updateOtherSensorReadings() {
    const Settings& settings = configManager.getSettings();

    // Update SHT4x readings if enabled AND no heating strategy
    timeComponentUpdate(SHT4X_INDEX, [this, &settings]() {
        if (sht4xManager && settings.sensors.sht4x_heating_strategy == 0) {
            sht4xManager->updateReadings();
        }
    });

    if (max31855kManager) {
        max31855kManager->update();
    }

    // Update chip temperature reading
    if (chipTempManager) {
        chipTempManager->updateTemperature();
    }
}

void MainController::handleSerialCommands() {
    if (Serial.available()) {
        String command = Serial.readStringUntil('\n');
        command.trim();
        if (command == "getconfig") {
            File file = LittleFS.open("/config.json", "r");
            if (file) {
                UnifiedLogger::info("Current config.json contents:\n");
                while (file.available()) {
                    Serial.write(file.read());
                }
                UnifiedLogger::info("\n");
                file.close();
            } else {
                UnifiedLogger::error("Config file not found\n");
            }
        } else if (command == "testlogging") {
            UnifiedLogger::info("Starting logging performance test...\n");
            runLoggingPerformanceTest();
            UnifiedLogger::info("Test completed.\n");
        }
    }
}

void MainController::updateBootDateTime() {
    if (ntpManager.isTimeSet() && bootTimeMillis > 0) {
        // Calculate how long ago the system booted (in seconds)
        unsigned long currentMillis = millis();
        unsigned long uptimeSeconds = (currentMillis - bootTimeMillis) / 1000;

        // Get current time and subtract uptime to get boot time
        time_t currentTime = ntpManager.getCurrentTime();
        time_t bootTime = currentTime - uptimeSeconds;

        // Format boot time
        struct tm bootTimeInfo;
        localtime_r(&bootTime, &bootTimeInfo);

        char buffer[20];
        strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &bootTimeInfo);
        bootDateTime = String(buffer);

        // Update WebServerManager with new boot time (if initialized)
        if (webServerManager) {
            webServerManager->setBootDateTime(bootDateTime);
        }

        UnifiedLogger::info("Boot time calculated: %s (uptime: %lu seconds)\n", bootDateTime.c_str(), uptimeSeconds);
    }
}

String MainController::formatDateTime(time_t t) const {
    if (t <= 0) return "Time not set";
    struct tm ti;
    localtime_r(&t, &ti);
    char buffer[20];
    strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &ti);
    return String(buffer);
}

bool MainController::isRtcEnabled() const {
    return configManager.getSettings().ntp.rtc.enabled;
}

String MainController::getRtcFormattedDateTime() const {
    if (!ds3231Manager.isInitialized() || !ds3231Manager.isPresent()) {
        return "RTC not available";
    }

    DateTime rtcNow;
    if (!ds3231Manager.readNow(rtcNow) || !DS3231Manager::isDateTimePlausible(rtcNow)) {
        return "RTC time invalid";
    }

    time_t epoch = ds3231Manager.toEpoch(rtcNow);
    return formatDateTime(epoch);
}

String MainController::getLastRtcSyncFormattedDateTime() const {
    return formatDateTime(lastRtcSyncEpoch);
}

String MainController::getTimeSource() const {
    // If at least one NTP sync succeeded, we consider NTP the authoritative source.
    // Otherwise, if system time is set and RTC is present, assume RTC seeded it.
    if (ntpManager.wasLastSyncSuccessful()) {
        return "NTP";
    }

    if (ntpManager.isTimeSet() && ds3231Manager.isInitialized() && ds3231Manager.isPresent()) {
        return "DS3231";
    }

    return "UNSET";
}

LedStatus MainController::determineLedStatus() {
    const Settings& settings = configManager.getSettings();

    if (criticalConfigInvalid) {
        return STATUS_RED;
    }

    if (!wifiManager.isConnected()) {
        // Keep "initializing" blue until we've connected at least once.
        return wifiEverConnected ? STATUS_RED : STATUS_BLUE;
    } else if (!mqttManager || !mqttManager->isConnected()) {
        return STATUS_YELLOW;
    } else {
        // NEW: Check NTP sync status first (Option B - NTP priority over green status)
        if (!ntpManager.isTimeSet()) {
            return STATUS_WHITE;
        }
        
        // Check sensor errors only if NTP is synced
        bool hasErrors = false;

        // Check DS18B20 sensors if enabled
        if (ds18b20Manager) {
            for (int i = 0; i < ds18b20Manager->getSensorCount(); i++) {
                if (!ds18b20Manager->isSensorOperational(i)) {
                    hasErrors = true;
                    break;
                }
            }
        }

        // Check SHT4x sensor if enabled
        if (!hasErrors && sht4xManager) {
            // No failure tracking for SHT4x
        }

        if (hasErrors) {
            return STATUS_MAGENTA;
        } else {
            return STATUS_GREEN;
        }
    }
}

void MainController::updateLedStatus(unsigned long nowMs) {
    if (!ledManager) {
        return;
    }

    // Recompute the "base" status periodically; rendering/animation can update more frequently.
    if (lastLedBaseUpdateMs == 0 || (nowMs - lastLedBaseUpdateMs) >= LED_BASE_UPDATE_INTERVAL_MS) {
        const LedStatus newBaseStatus = determineLedStatus();
        if (newBaseStatus != lastLedBaseStatus) {
            const char* name = "UNKNOWN";
            switch (newBaseStatus) {
                case STATUS_OFF: name = "OFF"; break;
                case STATUS_RED: name = "RED"; break;
                case STATUS_YELLOW: name = "YELLOW"; break;
                case STATUS_GREEN: name = "GREEN"; break;
                case STATUS_MAGENTA: name = "MAGENTA"; break;
                case STATUS_BLUE: name = "BLUE"; break;
                case STATUS_CYAN: name = "CYAN"; break;
                case STATUS_WHITE: name = "WHITE"; break;
            }
            UnifiedLogger::info("[LED] Base status -> %s\n", name);

            lastLedBaseStatus = newBaseStatus;

            // Reset blink phase on status transitions so we start "on".
            ledBlinkOn = true;
            lastLedBlinkToggleMs = nowMs;
        }

        lastLedBaseUpdateMs = nowMs;
    }

    const LedStatus baseStatus = lastLedBaseStatus;
    LedStatus renderedStatus = baseStatus;

    if (baseStatus == STATUS_RED || baseStatus == STATUS_YELLOW) {
        const unsigned long intervalMs =
            (baseStatus == STATUS_RED) ? LED_RED_BLINK_INTERVAL_MS : LED_YELLOW_BLINK_INTERVAL_MS;

        if (lastLedBlinkToggleMs == 0) {
            lastLedBlinkToggleMs = nowMs;
            ledBlinkOn = true;
        } else {
            const unsigned long elapsed = nowMs - lastLedBlinkToggleMs;
            if (elapsed >= intervalMs && intervalMs > 0) {
                const unsigned long toggles = elapsed / intervalMs;
                if ((toggles & 1U) != 0U) {
                    ledBlinkOn = !ledBlinkOn;
                }
                lastLedBlinkToggleMs += toggles * intervalMs;
            }
        }

        renderedStatus = ledBlinkOn ? baseStatus : STATUS_OFF;
    } else {
        // Solid (non-blinking) statuses.
        ledBlinkOn = true;
        lastLedBlinkToggleMs = nowMs;
    }

    if (renderedStatus != lastLedRenderedStatus) {
        ledManager->setStatus(renderedStatus);
        lastLedRenderedStatus = renderedStatus;
    }
}

bool MainController::runLoggingPerformanceTest() {
    UnifiedLogger::info("[MainController] Running logging performance test...\n");

    // Set up the test managers
    LoggingPerformanceTest::setManagers(this, relayManager);

    // Run the test
    bool result = LoggingPerformanceTest::runHighFrequencyLoggingTest();

    if (result) {
        UnifiedLogger::info("[MainController] Logging performance test completed: PASSED\n");
    } else {
        UnifiedLogger::warning("[MainController] Logging performance test completed: FAILED\n");
    }
    return result;
}

OtaStatusSnapshot MainController::getOtaStatus() const {
    return otaManager.snapshot();
}

String MainController::getOtaLogText() const {
    return otaManager.getLogText();
}
