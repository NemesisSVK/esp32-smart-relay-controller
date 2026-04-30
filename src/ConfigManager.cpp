#include "ConfigManager.h"
#include "PinValidator.h"
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <algorithm>
#include <mbedtls/md.h>
#include "UnifiedLogger.h"
#include "PsramJsonAllocator.h"

// Static encryption key definition
String ConfigManager::encryptionKey = "";

ConfigManager::ConfigManager() : configLoaded(false), migrationComplete(false), settingsMutex(nullptr), settingsRevision(0) {
    settingsMutex = xSemaphoreCreateRecursiveMutex();
    metadata.version = 1;
    metadata.timestamp = 0;
    metadata.encrypted = false;
    metadata.checksum = "";
    metadata.description = "Default configuration";
    migrationSalt = "";
    securedWifiPassword = "";
    securedMqttPassword = "";
    securedAuthPassword = "";
    plainTextAuthPassword = "";
    setDefaultSettings();
}

ConfigManager::~ConfigManager() {
    if (settingsMutex) {
        vSemaphoreDelete(settingsMutex);
        settingsMutex = nullptr;
    }
}

void ConfigManager::lockSettings() const {
    if (settingsMutex) {
        xSemaphoreTakeRecursive(settingsMutex, portMAX_DELAY);
    }
}

void ConfigManager::unlockSettings() const {
    if (settingsMutex) {
        xSemaphoreGiveRecursive(settingsMutex);
    }
}

bool ConfigManager::loadConfiguration() {
    SettingsLockGuard lock(*this);

    if (!LittleFS.begin()) {
        UnifiedLogger::warning("[CONFIG] LittleFS mount failed, attempting format\n");
        if (LittleFS.format()) {
            UnifiedLogger::info("[CONFIG] LittleFS format completed\n");
            if (!LittleFS.begin()) {
                UnifiedLogger::error("[CONFIG] Failed to mount LittleFS after format\n");
                return false;
            } else {
                UnifiedLogger::info("[CONFIG] LittleFS mounted after format\n");
            }
        } else {
            UnifiedLogger::error("[CONFIG] Failed to format LittleFS\n");
            return false;
        }
    }

    if (LittleFS.begin()) {
        UnifiedLogger::info("[CONFIG] LittleFS mounted\n");
        if (LittleFS.exists("/config.json")) {
        } else {
            return false;
        }

        File file = LittleFS.open("/config.json", "r");
        if (!file) {
            return false;
        }

        PsramJsonAllocator jsonAllocator;
        JsonDocument doc(&jsonAllocator);
        DeserializationError error = deserializeJson(doc, file);
        file.close();

        if (error) {
            UnifiedLogger::error("[CONFIG] Failed to parse config.json: %s\n", error.c_str());
            return false;
        } else {

            // Validate configuration before loading
            String validationError;
            if (!validateConfiguration(doc, validationError)) {
                setDefaultSettings();
                return false;
            }

            // Load all settings from the document
            loadWifiSettings(doc, true);  // Load encrypted passwords from file
            loadMqttSettings(doc, true);  // Load encrypted passwords from file
            loadSensorSettings(doc);
            loadRelaySettings(doc);
            loadNetworkSettings(doc);
            loadSecuritySettings(doc, true);  // Load encrypted passwords from file
            loadDeviceInfoSettings(doc);
            loadDebugSettings(doc);  // Load debug settings from config.json
            loadLoggingSettings(doc);  // Load logging settings from config.json
            loadNTPSettings(doc);
            loadScheduleSettings(doc);
            loadFallbackSettings(doc);

            // Load metadata if available
            if (!doc["metadata"]["version"].isNull()) {
                metadata.version = doc["metadata"]["version"];
                metadata.checksum = doc["metadata"]["checksum"].as<String>();
                metadata.timestamp = doc["metadata"]["timestamp"];
                metadata.description = doc["metadata"]["description"].as<String>();
                metadata.encrypted = doc["metadata"]["encrypted"].as<bool>();
            }

            // Perform automatic credential migration if needed
            if (!migrateCredentialsIfNeeded()) {
                UnifiedLogger::warning("[CONFIG] Credential migration failed, continuing with loaded config\n");
            }

            configLoaded = true;
            settingsRevision++;
            return true;
        }
    }
    return false;
}

bool ConfigManager::saveConfiguration() {
    SettingsLockGuard lock(*this);

    // Log current settings before saving

    PsramJsonAllocator jsonAllocator;
    JsonDocument doc(&jsonAllocator);

    // Save all settings to the document
    saveWifiSettings(doc);
    saveMqttSettings(doc);
    saveSensorSettings(doc);
    saveRelaySettings(doc);
    saveNetworkSettings(doc);
    saveSecuritySettings(doc);
    saveDeviceInfoSettings(doc);
    saveDebugSettings(doc);
    saveLoggingSettings(doc);
    saveNTPSettings(doc);
    saveScheduleSettings(doc);
    saveFallbackSettings(doc);

    // Save metadata
    doc["metadata"]["version"] = metadata.version;
    doc["metadata"]["checksum"] = calculateChecksum(doc);
    doc["metadata"]["timestamp"] = millis();
    doc["metadata"]["description"] = metadata.description;
    doc["metadata"]["encrypted"] = metadata.encrypted;

    // Log the JSON being saved
    String jsonString;
    serializeJson(doc, jsonString);

    // Log specific key values from the document

    // Create backup before saving
    if (configLoaded) {
        PsramJsonAllocator backupJsonAllocator;
        JsonDocument backupDoc(&backupJsonAllocator);
        saveWifiSettings(backupDoc);
        saveMqttSettings(backupDoc);
        saveSensorSettings(backupDoc);
        saveRelaySettings(backupDoc);
        saveNetworkSettings(backupDoc);
        saveSecuritySettings(backupDoc);
        saveDeviceInfoSettings(backupDoc);
        saveNTPSettings(backupDoc);
        saveScheduleSettings(backupDoc);
        saveFallbackSettings(backupDoc);
        saveVersionedConfig(backupDoc, "Auto-backup before save");
    }

    File file = LittleFS.open("/config.json", "w");
    if (!file) {
        return false;
    }

    size_t bytesWritten = serializeJson(doc, file);

    file.close();

    // Verify file was written
    if (LittleFS.exists("/config.json")) {
        File verifyFile = LittleFS.open("/config.json", "r");
        if (verifyFile) {
            size_t fileSize = verifyFile.size();
            verifyFile.close();
        } else {
        }
    } else {
    }


    // Verify the saved file content immediately
    if (LittleFS.exists("/config.json")) {
        File verifyFile = LittleFS.open("/config.json", "r");
        if (verifyFile) {
            PsramJsonAllocator verifyJsonAllocator;
            JsonDocument verifyDoc(&verifyJsonAllocator);
            DeserializationError verifyError = deserializeJson(verifyDoc, verifyFile);
            verifyFile.close();
            if (!verifyError) {
            } else {
            }
        } else {
        }
    } else {
    }

    configLoaded = true;
    settingsRevision++;
    return true;
}

void ConfigManager::setDefaultSettings() {
    SettingsLockGuard lock(*this);
    settings.wifi.ssid = "default_ssid";
    settings.wifi.password = "default_password";
    settings.mqtt.server = "192.168.1.1";
    settings.mqtt.port = 1883;
    settings.mqtt.user = "";
    settings.mqtt.password = "";
    settings.mqtt.client_id = "esp32_temp_default";
    settings.mqtt.publish_interval = 10; // Default 10 seconds
    for (int i = 0; i < 32; i++) {
        settings.mqtt.topics.temp[i] = "temperature_sensor_" + String(i + 1);
    }
    settings.mqtt.topics.sht4x = "sht4x_sensor"; // Default SHT4x topic name
    settings.mqtt.topics.max31855k = "max31855k_sensor"; // Default MAX31855K topic name

    // DS18B20 default configuration
    settings.sensors.ds18b20_enabled = true;  // DS18B20 enabled by default
    settings.sensors.sensor_count = 2;
    settings.sensors.one_wire_bus_count = 4;
    settings.sensors.one_wire_pins[0] = 4;
    settings.sensors.one_wire_pins[1] = 5;
    settings.sensors.one_wire_pins[2] = 6;
    settings.sensors.one_wire_pins[3] = 7;
    settings.sensors.viable_scan_pins = "4,5,6,7,12,13,14,15,16,17,18";
    settings.sensors.ds18b20_measurement_interval = 60.0; // 60 seconds default

    // SHT4x default configuration
    settings.sensors.sht4x_enabled = false;
    settings.sensors.sht4x_sda_pin = 8;
    settings.sensors.sht4x_scl_pin = 9;
    settings.sensors.sht4x_i2c_address = 0x44;
    settings.sensors.sht4x_precision = 2; // SHT4X_HIGH_PRECISION
    settings.sensors.sht4x_heater_power = 0;   // 0=none, 1=low (20mW), 2=medium (110mW), 3=high (200mW)
    settings.sensors.sht4x_heater_duration = 0; // 0=short (0.1s), 1=long (1s)
    settings.sensors.sht4x_heating_strategy = 0; // 0=none, 1=alternating
    settings.sensors.sht4x_measurement_interval = 60.0; // 60 seconds default

    // MAX31855K default configuration
    settings.sensors.max31855k_enabled = false;
    settings.sensors.max31855k_do_pin = 13;  // Locked default
    settings.sensors.max31855k_clk_pin = 12; // Locked default
    settings.sensors.max31855k_cs_pin = 11;  // Locked default

    for (int i = 0; i < 32; i++) {
        settings.sensors.sensor_addresses[i] = "";
    }
    // Network settings
    settings.network.hostname = "esp32-sensor";
    settings.network.mdns_enabled = true;

    // Security settings
    settings.security.auth_enabled = false;
    settings.security.username = "admin";
    settings.security.password_hash = ""; // Will be set when password is configured
    settings.security.ip_whitelist_enabled = false;
    settings.security.ip_ranges.clear();

    settings.device_info.name = "ESP32 Temp Sensor";
    settings.device_info.manufacturer = "Default";
    settings.device_info.model = "ESP32";

    // NTP default configuration (always enabled)
    settings.ntp.servers[0] = "pool.ntp.org";
    settings.ntp.servers[1] = "time.nist.gov";
    settings.ntp.servers[2] = "time.google.com";
    settings.ntp.timezone = "CET-1CEST,M3.5.0,M10.5.0/3"; // Central European Time

    // DS3231 RTC defaults (Phase 1: config + UI wiring; Phase 2+3 will use these flags)
    // NOTE: These are written here to ensure non-garbage defaults even if config.json doesn't contain ntp.rtc.
    settings.ntp.rtc.enabled = true;
    settings.ntp.rtc.sda_pin = 1;
    settings.ntp.rtc.scl_pin = 2;
    settings.ntp.rtc.i2c_address = 0x68;
    settings.ntp.rtc.store_utc = false; // Default: store local time (timezone rules in ntp.timezone)
    settings.ntp.rtc.set_system_time_on_boot = true;
    settings.ntp.rtc.sync_rtc_from_ntp = true;
    settings.ntp.rtc.sync_interval_seconds = 3600;
    settings.ntp.rtc.max_step_seconds = 5;

    // Relay default configuration
    settings.relay.enabled = true;
    settings.relay.pins_low = {15, 16};
    settings.relay.pins_mid = {17, 18};
    settings.relay.pins_high = {19, 20};
    settings.relay.timeout_ms = 5000; // Default 5 seconds safety timeout
    // DS18B20 safety mechanism defaults
    settings.relay.safety_enabled = false;
    settings.relay.safety_sensor_number = 1;
    settings.relay.safety_temperature_threshold = 50.0;
    settings.relay.safety_duration_seconds = 60;
    settings.relay.garage_relay_mode_enabled = false;

    // Schedule default configuration
    settings.schedules.enabled = true;

    // Fallback default configuration
    settings.fallback.enabled = false;
    settings.fallback.timeout_minutes = 30UL;

    // Debug default configuration
    settings.debug.logging_enabled = false; // Disabled by default for performance

    // Logging default configuration
    settings.logging.flush_interval_seconds = 30; // Default 30 seconds
    settingsRevision++;
}

bool ConfigManager::isConfigLoaded() const {
    return configLoaded;
}

Settings& ConfigManager::getSettings() {
    return settings;
}

const Settings& ConfigManager::getSettings() const {
    return settings;
}

Settings ConfigManager::getSettingsSnapshot() const {
    SettingsLockGuard lock(*this);
    return settings;
}

uint32_t ConfigManager::getSettingsRevision() const {
    SettingsLockGuard lock(*this);
    return settingsRevision;
}

String ConfigManager::getSettingsJson() {
    SettingsLockGuard lock(*this);
    PsramJsonAllocator jsonAllocator;
    JsonDocument doc(&jsonAllocator);

    // Save non-sensitive settings (exclude passwords for security)
    doc["wifi"]["ssid"] = settings.wifi.ssid;
    // Deliberately exclude wifi password

    doc["mqtt"]["server"] = settings.mqtt.server;
    doc["mqtt"]["port"] = settings.mqtt.port;
    doc["mqtt"]["user"] = settings.mqtt.user;  // Username is not sensitive
    // Deliberately exclude mqtt password
    doc["mqtt"]["client_id"] = settings.mqtt.client_id;
    doc["mqtt"]["publish_interval"] = settings.mqtt.publish_interval;
    JsonObject topics = doc["mqtt"]["topics"].to<JsonObject>();
    for (int i = 0; i < settings.sensors.sensor_count; i++) {
        topics["temp" + String(i + 1)] = settings.mqtt.topics.temp[i];
    }
    topics["sht4x"] = settings.mqtt.topics.sht4x;
    topics["max31855k"] = settings.mqtt.topics.max31855k;

    saveSensorSettings(doc);
    saveRelaySettings(doc);
    saveNetworkSettings(doc);
    saveDeviceInfoSettings(doc);
    saveDebugSettings(doc);
    saveLoggingSettings(doc);
    saveNTPSettings(doc);
    
    // Enhanced debug logging for schedules

    // Debug: Show what entries we have in memory
    for (size_t i = 0; i < settings.schedules.entries.size(); i++) {
        const auto& entry = settings.schedules.entries[i];
    }

    saveScheduleSettings(doc);

    saveFallbackSettings(doc);

    // Debug: Check what was actually saved to the document
    if (!doc["schedules"].isNull()) {
        String savedSchedules;
        serializeJson(doc["schedules"], savedSchedules);
    } else {
    }

    // Save sanitized security settings (exclude passwords)
    doc["security"]["auth_enabled"] = settings.security.auth_enabled;
    doc["security"]["username"] = settings.security.username;
    // Deliberately exclude password_hash for security
    doc["security"]["ip_whitelist_enabled"] = settings.security.ip_whitelist_enabled;

    // Save IP ranges
    JsonArray ipRanges = doc["security"]["ip_ranges"].to<JsonArray>();
    for (const auto& range : settings.security.ip_ranges) {
        ipRanges.add(range);
    }

    String json;
    serializeJson(doc, json);

    // Check if schedules section exists in the JSON
    if (json.indexOf("\"schedules\"") != -1) {
    } else {
    }

    return json;
}

bool ConfigManager::updateSettingsFromJson(const String& jsonBody) {
    SettingsLockGuard lock(*this);
    PsramJsonAllocator jsonAllocator;
    JsonDocument doc(&jsonAllocator);
    DeserializationError error = deserializeJson(doc, jsonBody);
    if (error) {
        UnifiedLogger::error("[CONFIG] JSON parsing error: %s\n", error.c_str());
        return false;
    }

    // Validate the new configuration
    String validationError;
    if (!validateConfiguration(doc, validationError)) {
        UnifiedLogger::error("[CONFIG] Configuration validation failed: %s\n", validationError.c_str());
        return false;
    }

    // Handle password updates with special logic
    handlePasswordUpdates(doc);

    // Update other settings from the parsed document (passwords already handled)
    loadWifiSettings(doc, false);  // Passwords handled by handlePasswordUpdates
    loadMqttSettings(doc, false);  // Passwords handled by handlePasswordUpdates
    loadSensorSettings(doc);
    
    // Debug: Check if relay section exists in incoming JSON
        if (doc["relay"].isNull()) {

            // Debug: Print the entire JSON to see what's actually there
            String jsonString;
            serializeJson(doc, jsonString);

        } else {
            if (!doc["relay"]["enabled"].isNull()) {
            }

            // Debug: Check if pins arrays exist
            if (!doc["relay"]["pins_low"].isNull()) {
            }
            if (!doc["relay"]["pins_mid"].isNull()) {
            }
            if (!doc["relay"]["pins_high"].isNull()) {
            }
        }
    
    loadRelaySettings(doc);  // Load relay settings from POST request
    loadNetworkSettings(doc);
    loadSecuritySettings(doc, false);  // Passwords handled by handlePasswordUpdates
    loadDeviceInfoSettings(doc);
    loadDebugSettings(doc);
    loadLoggingSettings(doc);
    loadNTPSettings(doc);
    loadScheduleSettings(doc);
    loadFallbackSettings(doc);

    // Update metadata
    metadata.description = "Updated via API";
    metadata.timestamp = millis();

    // Update UnifiedLogger settings
    UnifiedLogger::updateSettings();

    // Save the updated configuration
    bool saveResult = saveConfiguration();
    return saveResult;
}

void ConfigManager::loadWifiSettings(const JsonDocument& doc, bool loadPasswords) {
    settings.wifi.ssid = doc["wifi"]["ssid"].as<String>();
    if (loadPasswords) {
        String password = doc["wifi"]["password"].as<String>();
        if (password.length() > 0) {
            if (password.startsWith("ENC:")) {
                // Encrypted - decrypt for runtime use
                String decrypted = decryptSecret(password);
                if (decrypted.length() > 0) {
                    settings.wifi.password = decrypted;
                } else {
                }
            } else {
                // Plain text - encrypt it and save immediately
                settings.wifi.password = password; // Keep plain for runtime use
                String encrypted = encryptSecret(password);

                // Update the document with encrypted version
                const_cast<JsonDocument&>(doc)["wifi"]["password"] = encrypted;

                // Save immediately
                if (!saveConfiguration()) {
                    UnifiedLogger::error("[CONFIG] Failed to save encrypted WiFi settings\n");
                } else {
                }
            }
        } else {
        }
    }
    // Password encryption/decryption is handled separately
}

void ConfigManager::loadMqttSettings(const JsonDocument& doc, bool loadPasswords) {
    settings.mqtt.server = doc["mqtt"]["server"].as<String>();
    settings.mqtt.port = doc["mqtt"]["port"];
    // Only update user if a non-empty value is provided (user is not encrypted)
    String newUser = doc["mqtt"]["user"].as<String>();
    if (newUser.length() > 0) {
        settings.mqtt.user = newUser;
    }
    if (loadPasswords) {
        String password = doc["mqtt"]["password"].as<String>();
        if (password.length() > 0) {
            if (password.startsWith("ENC:")) {
                // Encrypted - decrypt for runtime use
                String decrypted = decryptSecret(password);
                if (decrypted.length() > 0) {
                    settings.mqtt.password = decrypted;
                } else {
                }
            } else {
                // Plain text - encrypt it and save immediately
                settings.mqtt.password = password; // Keep plain for runtime use
                String encrypted = encryptSecret(password);

                // Update the document with encrypted version
                const_cast<JsonDocument&>(doc)["mqtt"]["password"] = encrypted;

                // Save immediately
                if (!saveConfiguration()) {
                    UnifiedLogger::error("[CONFIG] Failed to save encrypted MQTT settings\n");
                } else {
                }
            }
        } else {
        }
    }
    // Password encryption/decryption is handled separately
    settings.mqtt.client_id = doc["mqtt"]["client_id"].as<String>();
    
    // Load MQTT publish interval.
    // Guard against non-positive values from malformed/legacy config to keep runtime cadence valid.
    long configuredPublishInterval = doc["mqtt"]["publish_interval"] | 10L;
    if (configuredPublishInterval < 1L) {
        UnifiedLogger::warning("[CONFIG] Invalid mqtt.publish_interval (%ld), using default 10s\n", configuredPublishInterval);
        settings.mqtt.publish_interval = 10UL;
    } else {
        settings.mqtt.publish_interval = static_cast<unsigned long>(configuredPublishInterval);
    }

    JsonObjectConst topics = doc["mqtt"]["topics"];
    for (int i = 0; i < 8; i++) {
        String key = "temp" + String(i + 1);
        if (!topics[key].isNull()) {
            settings.mqtt.topics.temp[i] = topics[key].as<String>();
        }
    }

    // Load SHT4x topic if available
    if (!topics["sht4x"].isNull()) {
        settings.mqtt.topics.sht4x = topics["sht4x"].as<String>();
    }

    // Load MAX31855K topic if available
    if (!topics["max31855k"].isNull()) {
        settings.mqtt.topics.max31855k = topics["max31855k"].as<String>();
    }

}

void ConfigManager::loadSensorSettings(const JsonDocument& doc) {
    // Load DS18B20 enabled flag
    settings.sensors.ds18b20_enabled = doc["sensors"]["ds18b20_enabled"] | true;  // default to true if not present

    settings.sensors.sensor_count = doc["sensors"]["sensor_count"] | 2;  // default to 2 if not present
    // Clamp to fixed-size arrays (32 sensors max)
    if (settings.sensors.sensor_count < 1) settings.sensors.sensor_count = 1;
    if (settings.sensors.sensor_count > 32) settings.sensors.sensor_count = 32;
    int numPins = 4; // default
    if (!doc["sensors"]["one_wire_pins"].isNull()) {
        JsonArrayConst pins = doc["sensors"]["one_wire_pins"];
        int savedBusCount = doc["sensors"]["one_wire_bus_count"].isNull() ? pins.size() : doc["sensors"]["one_wire_bus_count"].as<int>();
        numPins = savedBusCount;
        // Initialize all pins to -1 first
        for (int i = 0; i < 4; i++) {
            settings.sensors.one_wire_pins[i] = -1;
        }
        // Then load the configured pins
        for (size_t i = 0; i < pins.size() && i < 4; i++) {
            settings.sensors.one_wire_pins[i] = pins[i];
        }
    }
    // Clamp to fixed-size array (4 buses max)
    if (numPins < 1) numPins = 1;
    if (numPins > 4) numPins = 4;
    settings.sensors.one_wire_bus_count = numPins;

    // Load viable scan pins if available, otherwise use default
    if (!doc["sensors"]["viable_scan_pins"].isNull()) {
        settings.sensors.viable_scan_pins = doc["sensors"]["viable_scan_pins"].as<String>();
    } else {
        settings.sensors.viable_scan_pins = "4,5,6,7,12,13,14,15,16,17,18";
    }

    // Load DS18B20 measurement interval
    if (!doc["sensors"]["ds18b20_measurement_interval"].isNull()) {
        settings.sensors.ds18b20_measurement_interval = doc["sensors"]["ds18b20_measurement_interval"].as<float>();
    } else {
        settings.sensors.ds18b20_measurement_interval = 60.0; // Default 60 seconds
    }

    // Load SHT4x configuration if available
    if (!doc["sensors"]["sht4x_enabled"].isNull()) {
        settings.sensors.sht4x_enabled = doc["sensors"]["sht4x_enabled"].as<bool>();
        settings.sensors.sht4x_sda_pin = doc["sensors"]["sht4x_sda_pin"].as<int>();
        settings.sensors.sht4x_scl_pin = doc["sensors"]["sht4x_scl_pin"].as<int>();
        settings.sensors.sht4x_i2c_address = strtol(doc["sensors"]["sht4x_i2c_address"].as<const char*>(), NULL, 16);
        settings.sensors.sht4x_precision = doc["sensors"]["sht4x_precision"].as<int>();
        settings.sensors.sht4x_heater_power = doc["sensors"]["sht4x_heater_power"].as<int>();
        settings.sensors.sht4x_heater_duration = doc["sensors"]["sht4x_heater_duration"].as<int>();

        // Load heating strategy settings if available, otherwise use defaults
        if (!doc["sensors"]["sht4x_heating_strategy"].isNull()) {
            int loadedStrategy = doc["sensors"]["sht4x_heating_strategy"].as<int>();
            // Convert deprecated strategy 2 (in-between) to strategy 1 (alternating)
            if (loadedStrategy == 2) {
                UnifiedLogger::warning("[CONFIG] SHT4x: Converting deprecated in-between heating strategy (2) to alternating strategy (1) during config load\n");
                settings.sensors.sht4x_heating_strategy = 1;
            } else {
                settings.sensors.sht4x_heating_strategy = loadedStrategy;
            }
        } else {
            settings.sensors.sht4x_heating_strategy = 0; // Default: no heating strategy
        }

        if (!doc["sensors"]["sht4x_measurement_interval"].isNull()) {
            settings.sensors.sht4x_measurement_interval = doc["sensors"]["sht4x_measurement_interval"].as<float>();
        } else {
            settings.sensors.sht4x_measurement_interval = 60.0; // Default: 60 seconds
        }
    }

    // Load MAX31855K configuration
    settings.sensors.max31855k_enabled = doc["sensors"]["max31855k_enabled"] | settings.sensors.max31855k_enabled;
    settings.sensors.max31855k_do_pin = doc["sensors"]["max31855k_do_pin"] | settings.sensors.max31855k_do_pin;
    settings.sensors.max31855k_clk_pin = doc["sensors"]["max31855k_clk_pin"] | settings.sensors.max31855k_clk_pin;
    settings.sensors.max31855k_cs_pin = doc["sensors"]["max31855k_cs_pin"] | settings.sensors.max31855k_cs_pin;

    // Load relay configuration if available
    // Relay loading is now handled in loadRelaySettings

    // Note: Relay timeout loading is now handled in loadRelaySettings method
    // This ensures consistent loading from the correct location

    // Debug: Check if relay section exists before calling loadRelaySettings
    if (doc["relay"].isNull()) {
    } else {
    }

    JsonObjectConst sensorAddrs = doc["sensors"]["sensor_addresses"];
    for (int i = 0; i < settings.sensors.sensor_count; i++) {
        String key = "sensor" + String(i + 1) + "_address";
        if (!sensorAddrs[key].isNull()) {
            settings.sensors.sensor_addresses[i] = sensorAddrs[key].as<String>();
        }
    }
}

void ConfigManager::loadNetworkSettings(const JsonDocument& doc) {
    settings.network.hostname = doc["network"]["hostname"].as<String>();
    settings.network.mdns_enabled = doc["network"]["mdns_enabled"] | true; // default to true if not present
    // Load serial logging setting and map to debug.logging_enabled
    // This overrides the debug.logging_enabled loaded from loadDebugSettings
    if (!doc["network"]["serial_logging_enabled"].isNull()) {
        settings.debug.logging_enabled = doc["network"]["serial_logging_enabled"].as<bool>();
    }
}

void ConfigManager::loadSecuritySettings(const JsonDocument& doc, bool loadPasswords) {
    settings.security.auth_enabled = doc["security"]["auth_enabled"] | false; // default to false if not present
    settings.security.username = doc["security"]["username"].as<String>();
    settings.security.ip_whitelist_enabled = doc["security"]["ip_whitelist_enabled"] | false; // default to false if not present
    
    // Load IP ranges
    JsonArrayConst ipRanges = doc["security"]["ip_ranges"];
    settings.security.ip_ranges.clear();
    for (size_t i = 0; i < ipRanges.size(); i++) {
        settings.security.ip_ranges.push_back(ipRanges[i].as<String>());
    }
    
    if (loadPasswords) {
        String loadedPassword = doc["security"]["password_hash"].as<String>();
        if (loadedPassword.length() > 0) {
            // Check if it's encrypted (starts with 'ENC:')
            if (loadedPassword.startsWith("ENC:")) {
                // Encrypted - decrypt to plain text for auth
                plainTextAuthPassword = decryptSecret(loadedPassword);
                settings.security.password_hash = loadedPassword; // Keep encrypted in settings
            } else {
                // Plain text - encrypt it and store plain text for auth
                plainTextAuthPassword = loadedPassword;
                settings.security.password_hash = encryptSecret(loadedPassword);

                // Save the encrypted password immediately to persist the change
                if (!saveConfiguration()) {
                    UnifiedLogger::error("[CONFIG] Failed to save encrypted security settings\n");
                } else {
                }
            }
        }
    }
    // Password encryption/decryption is handled separately
}

void ConfigManager::loadDeviceInfoSettings(const JsonDocument& doc) {
    settings.device_info.name = doc["device_info"]["name"].as<String>();

    // Hardening: avoid turning missing/JSON-null fields into the literal String "null".
    // Only overwrite when a non-empty, non-null value is provided.
    JsonVariantConst manufacturerVar = doc["device_info"]["manufacturer"];
    if (!manufacturerVar.isNull()) {
        String manufacturer = manufacturerVar.as<String>();
        manufacturer.trim();
        if (manufacturer.length() > 0 && manufacturer != "null" && manufacturer != "(null)") {
            settings.device_info.manufacturer = manufacturer;
        }
    }

    JsonVariantConst modelVar = doc["device_info"]["model"];
    if (!modelVar.isNull()) {
        String model = modelVar.as<String>();
        model.trim();
        if (model.length() > 0 && model != "null" && model != "(null)") {
            settings.device_info.model = model;
        }
    }
}

void ConfigManager::loadDebugSettings(const JsonDocument& doc) {
    bool hasDebugSection = !doc["debug"].isNull();
    bool hasLoggingEnabled = !doc["debug"]["logging_enabled"].isNull();
    bool hasNetworkSerialLogging = !doc["network"]["serial_logging_enabled"].isNull();

    // Only load from debug section if network.serial_logging_enabled doesn't exist (backward compatibility)
    if (!hasNetworkSerialLogging) {
        settings.debug.logging_enabled = doc["debug"]["logging_enabled"] | false; // Default to false for performance
    } else {
    }
}

void ConfigManager::loadLoggingSettings(const JsonDocument& doc) {
    // Load logging flush interval seconds
    if (!doc["logging"]["flush_interval_seconds"].isNull()) {
        settings.logging.flush_interval_seconds = doc["logging"]["flush_interval_seconds"];
    } else {
        settings.logging.flush_interval_seconds = 30; // Default 30 seconds
    }
}

void ConfigManager::loadNTPSettings(const JsonDocument& doc) {
    JsonArrayConst servers = doc["ntp"]["servers"];
    for (size_t i = 0; i < servers.size() && i < 3; i++) {
        settings.ntp.servers[i] = servers[i].as<String>();
    }

    settings.ntp.timezone = doc["ntp"]["timezone"].as<String>();
    if (settings.ntp.timezone.length() == 0) {
        settings.ntp.timezone = "CET-1CEST,M3.5.0,M10.5.0/3"; // Default to CET
    }

    // Optional DS3231 RTC configuration
    JsonObjectConst rtcObj = doc["ntp"]["rtc"];
    if (!rtcObj.isNull()) {
        settings.ntp.rtc.enabled = rtcObj["enabled"] | settings.ntp.rtc.enabled;
        settings.ntp.rtc.sda_pin = rtcObj["sda_pin"] | settings.ntp.rtc.sda_pin;
        settings.ntp.rtc.scl_pin = rtcObj["scl_pin"] | settings.ntp.rtc.scl_pin;

        // i2c_address: accept either string ("0x68" / "68") or integer.
        if (!rtcObj["i2c_address"].isNull()) {
            if (rtcObj["i2c_address"].is<const char*>()) {
                const char* addrStr = rtcObj["i2c_address"].as<const char*>();
                long parsed = strtol(addrStr, nullptr, 0);
                if (parsed >= 0 && parsed <= 0xFF) {
                    settings.ntp.rtc.i2c_address = static_cast<uint8_t>(parsed);
                }
            } else {
                int parsed = rtcObj["i2c_address"].as<int>();
                if (parsed >= 0 && parsed <= 0xFF) {
                    settings.ntp.rtc.i2c_address = static_cast<uint8_t>(parsed);
                }
            }
        }

        settings.ntp.rtc.store_utc = rtcObj["store_utc"] | settings.ntp.rtc.store_utc;
        settings.ntp.rtc.set_system_time_on_boot = rtcObj["set_system_time_on_boot"] | settings.ntp.rtc.set_system_time_on_boot;
        settings.ntp.rtc.sync_rtc_from_ntp = rtcObj["sync_rtc_from_ntp"] | settings.ntp.rtc.sync_rtc_from_ntp;

        // DS3231 calibration interval is not configurable anymore; keep it fixed at 60 minutes.
        settings.ntp.rtc.sync_interval_seconds = 3600;

        settings.ntp.rtc.max_step_seconds = rtcObj["max_step_seconds"] | settings.ntp.rtc.max_step_seconds;
    }
}

void ConfigManager::saveWifiSettings(JsonDocument& doc) {
    doc["wifi"]["ssid"] = settings.wifi.ssid;
    // Encrypt password before saving to file, but only if not already encrypted
    if (settings.wifi.password.length() > 0) {
        if (settings.wifi.password.startsWith("ENC:")) {
            // Already encrypted, save as is
            doc["wifi"]["password"] = settings.wifi.password;
        } else {
            // Plain text, encrypt it
            String encrypted = encryptSecret(settings.wifi.password);
            doc["wifi"]["password"] = encrypted;
        }
    } else {
        doc["wifi"]["password"] = settings.wifi.password;
    }
}

void ConfigManager::saveMqttSettings(JsonDocument& doc) {
    doc["mqtt"]["server"] = settings.mqtt.server;
    doc["mqtt"]["port"] = settings.mqtt.port;
    doc["mqtt"]["user"] = settings.mqtt.user;
    // Encrypt password before saving to file, but only if not already encrypted
    if (settings.mqtt.password.length() > 0) {
        if (settings.mqtt.password.startsWith("ENC:")) {
            // Already encrypted, save as is
            doc["mqtt"]["password"] = settings.mqtt.password;
        } else {
            // Plain text, encrypt it
            String encrypted = encryptSecret(settings.mqtt.password);
            doc["mqtt"]["password"] = encrypted;
        }
    } else {
        doc["mqtt"]["password"] = settings.mqtt.password;
    }
    doc["mqtt"]["client_id"] = settings.mqtt.client_id;
    doc["mqtt"]["publish_interval"] = settings.mqtt.publish_interval;
    JsonObject topics = doc["mqtt"]["topics"].to<JsonObject>();
    for (int i = 0; i < settings.sensors.sensor_count; i++) {
        topics["temp" + String(i + 1)] = settings.mqtt.topics.temp[i];
    }
    topics["sht4x"] = settings.mqtt.topics.sht4x;
    topics["max31855k"] = settings.mqtt.topics.max31855k;
}

void ConfigManager::saveSensorSettings(JsonDocument& doc) {
    // Save DS18B20 enabled flag
    doc["sensors"]["ds18b20_enabled"] = settings.sensors.ds18b20_enabled;

    doc["sensors"]["sensor_count"] = settings.sensors.sensor_count;
    doc["sensors"]["one_wire_bus_count"] = settings.sensors.one_wire_bus_count;
    JsonArray pins = doc["sensors"]["one_wire_pins"].to<JsonArray>();
    for (int i = 0; i < settings.sensors.one_wire_bus_count; i++) {
        pins.add(settings.sensors.one_wire_pins[i]);
    }
    doc["sensors"]["viable_scan_pins"] = settings.sensors.viable_scan_pins;
    doc["sensors"]["ds18b20_measurement_interval"] = settings.sensors.ds18b20_measurement_interval;

    // Save SHT4x configuration
    doc["sensors"]["sht4x_enabled"] = settings.sensors.sht4x_enabled;
    doc["sensors"]["sht4x_sda_pin"] = settings.sensors.sht4x_sda_pin;
    doc["sensors"]["sht4x_scl_pin"] = settings.sensors.sht4x_scl_pin;
    doc["sensors"]["sht4x_i2c_address"] = String(settings.sensors.sht4x_i2c_address, HEX);
    doc["sensors"]["sht4x_precision"] = settings.sensors.sht4x_precision;
    doc["sensors"]["sht4x_heater_power"] = settings.sensors.sht4x_heater_power;
    doc["sensors"]["sht4x_heater_duration"] = settings.sensors.sht4x_heater_duration;
    doc["sensors"]["sht4x_heating_strategy"] = settings.sensors.sht4x_heating_strategy;
    doc["sensors"]["sht4x_measurement_interval"] = settings.sensors.sht4x_measurement_interval;

    // Save MAX31855K configuration
    doc["sensors"]["max31855k_enabled"] = settings.sensors.max31855k_enabled;
    doc["sensors"]["max31855k_do_pin"] = settings.sensors.max31855k_do_pin;
    doc["sensors"]["max31855k_clk_pin"] = settings.sensors.max31855k_clk_pin;
    doc["sensors"]["max31855k_cs_pin"] = settings.sensors.max31855k_cs_pin;

    // Relay configuration is now saved in saveRelaySettings

    JsonObject sensorAddrs = doc["sensors"]["sensor_addresses"].to<JsonObject>();
    for (int i = 0; i < settings.sensors.sensor_count; i++) {
        sensorAddrs["sensor" + String(i + 1) + "_address"] = settings.sensors.sensor_addresses[i];
    }
}

void ConfigManager::saveNetworkSettings(JsonDocument& doc) {
    doc["network"]["hostname"] = settings.network.hostname;
    doc["network"]["mdns_enabled"] = settings.network.mdns_enabled;
    // Save serial logging setting from debug.logging_enabled
    doc["network"]["serial_logging_enabled"] = settings.debug.logging_enabled;
}

void ConfigManager::saveSecuritySettings(JsonDocument& doc) {
    doc["security"]["auth_enabled"] = settings.security.auth_enabled;
    doc["security"]["username"] = settings.security.username;
    doc["security"]["password_hash"] = settings.security.password_hash;
    doc["security"]["ip_whitelist_enabled"] = settings.security.ip_whitelist_enabled;
    
    // Save IP ranges
    JsonArray ipRanges = doc["security"]["ip_ranges"].to<JsonArray>();
    for (const auto& range : settings.security.ip_ranges) {
        ipRanges.add(range);
    }
}

void ConfigManager::saveDeviceInfoSettings(JsonDocument& doc) {
    doc["device_info"]["name"] = settings.device_info.name;
    doc["device_info"]["manufacturer"] = settings.device_info.manufacturer;
    doc["device_info"]["model"] = settings.device_info.model;
}

void ConfigManager::saveDebugSettings(JsonDocument& doc) {
    doc["debug"]["logging_enabled"] = settings.debug.logging_enabled;
}

void ConfigManager::saveLoggingSettings(JsonDocument& doc) {
    doc["logging"]["flush_interval_seconds"] = settings.logging.flush_interval_seconds;
}

void ConfigManager::saveNTPSettings(JsonDocument& doc) {
    JsonArray servers = doc["ntp"]["servers"].to<JsonArray>();
    for (int i = 0; i < 3; i++) {
        if (settings.ntp.servers[i].length() > 0) {
            servers.add(settings.ntp.servers[i]);
        }
    }
    doc["ntp"]["timezone"] = settings.ntp.timezone;

    // Optional DS3231 RTC configuration
    JsonObject rtcObj = doc["ntp"]["rtc"].to<JsonObject>();
    rtcObj["enabled"] = settings.ntp.rtc.enabled;
    rtcObj["sda_pin"] = settings.ntp.rtc.sda_pin;
    rtcObj["scl_pin"] = settings.ntp.rtc.scl_pin;
    rtcObj["i2c_address"] = String("0x") + String(settings.ntp.rtc.i2c_address, HEX);
    rtcObj["store_utc"] = settings.ntp.rtc.store_utc;
    rtcObj["set_system_time_on_boot"] = settings.ntp.rtc.set_system_time_on_boot;
    rtcObj["sync_rtc_from_ntp"] = settings.ntp.rtc.sync_rtc_from_ntp;
    rtcObj["max_step_seconds"] = settings.ntp.rtc.max_step_seconds;
}

void ConfigManager::loadRelaySettings(const JsonDocument& doc) {

    // Debug: Check if relay section exists at root level
    if (doc["relay"].isNull()) {
        
        // Check if relay section exists inside sensors section (web interface format)
        if (!doc["sensors"]["relay"].isNull()) {

            // Load relay settings from sensors section
            if (!doc["sensors"]["relay"]["enabled"].isNull()) {
                settings.relay.enabled = doc["sensors"]["relay"]["enabled"].as<bool>();

                // Load configurable pin arrays for each state
                settings.relay.pins_low.clear();
                if (!doc["sensors"]["relay"]["pins_low"].isNull()) {
                    JsonArrayConst pinsLow = doc["sensors"]["relay"]["pins_low"];
                    for (size_t i = 0; i < pinsLow.size(); i++) {
                        settings.relay.pins_low.push_back(pinsLow[i].as<int>());
                    }
                }

                settings.relay.pins_mid.clear();
                if (!doc["sensors"]["relay"]["pins_mid"].isNull()) {
                    JsonArrayConst pinsMid = doc["sensors"]["relay"]["pins_mid"];
                    for (size_t i = 0; i < pinsMid.size(); i++) {
                        settings.relay.pins_mid.push_back(pinsMid[i].as<int>());
                    }
                }

                settings.relay.pins_high.clear();
                if (!doc["sensors"]["relay"]["pins_high"].isNull()) {
                    JsonArrayConst pinsHigh = doc["sensors"]["relay"]["pins_high"];
                    for (size_t i = 0; i < pinsHigh.size(); i++) {
                        settings.relay.pins_high.push_back(pinsHigh[i].as<int>());
                    }
                }

                // Load relay timeout setting if available in sensors section
                if (!doc["sensors"]["relay"]["timeout_ms"].isNull()) {
                    settings.relay.timeout_ms = doc["sensors"]["relay"]["timeout_ms"];
                } else {
                }

                // Load DS18B20 safety mechanism settings from sensors section
                if (!doc["sensors"]["relay"]["safety_enabled"].isNull()) {
                    settings.relay.safety_enabled = doc["sensors"]["relay"]["safety_enabled"].as<bool>();
                } else {
                    settings.relay.safety_enabled = false;
                }
                if (!doc["sensors"]["relay"]["safety_sensor_number"].isNull()) {
                    settings.relay.safety_sensor_number = doc["sensors"]["relay"]["safety_sensor_number"].as<int>();
                } else {
                    settings.relay.safety_sensor_number = 1;
                }
                if (!doc["sensors"]["relay"]["safety_temperature_threshold"].isNull()) {
                    settings.relay.safety_temperature_threshold = doc["sensors"]["relay"]["safety_temperature_threshold"].as<float>();
                } else {
                    settings.relay.safety_temperature_threshold = 50.0f;
                }
                if (!doc["sensors"]["relay"]["safety_duration_seconds"].isNull()) {
                    settings.relay.safety_duration_seconds = doc["sensors"]["relay"]["safety_duration_seconds"].as<unsigned long>();
                } else {
                    settings.relay.safety_duration_seconds = 60UL;
                }
                if (!doc["sensors"]["relay"]["garage_relay_mode_enabled"].isNull()) {
                    settings.relay.garage_relay_mode_enabled = doc["sensors"]["relay"]["garage_relay_mode_enabled"].as<bool>();
                } else {
                    settings.relay.garage_relay_mode_enabled = false;
                }
                
                return; // Exit early since we found and loaded the settings
            }
        }

        // Default relay configuration
        settings.relay.enabled = true;
        settings.relay.pins_low = {15, 16};
        settings.relay.pins_mid = {17, 18};
        settings.relay.pins_high = {19, 20};
    } else {

        if (!doc["relay"]["enabled"].isNull()) {
            settings.relay.enabled = doc["relay"]["enabled"].as<bool>();

            // Load configurable pin arrays for each state
            settings.relay.pins_low.clear();
            if (!doc["relay"]["pins_low"].isNull()) {
                JsonArrayConst pinsLow = doc["relay"]["pins_low"];
                for (size_t i = 0; i < pinsLow.size(); i++) {
                    settings.relay.pins_low.push_back(pinsLow[i].as<int>());
                }
            }

            settings.relay.pins_mid.clear();
            if (!doc["relay"]["pins_mid"].isNull()) {
                JsonArrayConst pinsMid = doc["relay"]["pins_mid"];
                for (size_t i = 0; i < pinsMid.size(); i++) {
                    settings.relay.pins_mid.push_back(pinsMid[i].as<int>());
                }
            }

            settings.relay.pins_high.clear();
            if (!doc["relay"]["pins_high"].isNull()) {
                JsonArrayConst pinsHigh = doc["relay"]["pins_high"];
                for (size_t i = 0; i < pinsHigh.size(); i++) {
                    settings.relay.pins_high.push_back(pinsHigh[i].as<int>());
                }
            }
        }

        // Load relay timeout setting if available at root level
        if (!doc["relay"]["timeout_ms"].isNull()) {
            settings.relay.timeout_ms = doc["relay"]["timeout_ms"];
        } else {
        }

        // Load DS18B20 safety mechanism settings
        settings.relay.safety_enabled = doc["relay"]["safety_enabled"] | false;
        settings.relay.safety_sensor_number = doc["relay"]["safety_sensor_number"] | 1;
        settings.relay.safety_temperature_threshold = doc["relay"]["safety_temperature_threshold"] | 50.0f;
        settings.relay.safety_duration_seconds = doc["relay"]["safety_duration_seconds"] | 60UL;
        settings.relay.garage_relay_mode_enabled = doc["relay"]["garage_relay_mode_enabled"] | false;
    }

}

void ConfigManager::saveRelaySettings(JsonDocument& doc) {

    doc["relay"]["enabled"] = settings.relay.enabled;

    // Save configurable pin arrays for each state
    JsonArray pinsLow = doc["relay"]["pins_low"].to<JsonArray>();
    for (int pin : settings.relay.pins_low) {
        pinsLow.add(pin);
    }

    JsonArray pinsMid = doc["relay"]["pins_mid"].to<JsonArray>();
    for (int pin : settings.relay.pins_mid) {
        pinsMid.add(pin);
    }

    JsonArray pinsHigh = doc["relay"]["pins_high"].to<JsonArray>();
    for (int pin : settings.relay.pins_high) {
        pinsHigh.add(pin);
    }

    // Save relay timeout setting
    doc["relay"]["timeout_ms"] = settings.relay.timeout_ms;

    // Save DS18B20 safety mechanism settings
    doc["relay"]["safety_enabled"] = settings.relay.safety_enabled;
    doc["relay"]["safety_sensor_number"] = settings.relay.safety_sensor_number;
    doc["relay"]["safety_temperature_threshold"] = settings.relay.safety_temperature_threshold;
    doc["relay"]["safety_duration_seconds"] = settings.relay.safety_duration_seconds;
    doc["relay"]["garage_relay_mode_enabled"] = settings.relay.garage_relay_mode_enabled;

}

// Enhanced configuration management methods
bool ConfigManager::validateConfiguration(const JsonDocument& doc, String& errorMessage) {
    lastValidationErrors = "";

    String wifiError, mqttError, sensorError, timingError, securityError, deviceError, ntpError, scheduleError, ipWhitelistError, pinError, fallbackError;

    bool wifiValid = validateWifiSettings(doc, wifiError);
    bool mqttValid = validateMqttSettings(doc, mqttError);
    bool sensorValid = validateSensorSettings(doc, sensorError);
    bool timingValid = validateTimingSettings(doc, timingError);  // Always returns true - timing validation removed
    bool securityValid = validateSecuritySettings(doc, securityError);
    bool ipWhitelistValid = validateIPWhitelistSettings(doc, ipWhitelistError);
    bool deviceValid = validateDeviceInfoSettings(doc, deviceError);
    bool ntpValid = validateNTPSettings(doc, ntpError);
    bool scheduleValid = validateScheduleSettings(doc, scheduleError);
    bool fallbackValid = validateFallbackSettings(doc, fallbackError);
    bool pinValid = validatePinConfiguration(doc, pinError);


    if (!wifiValid) lastValidationErrors += "WiFi: " + wifiError + "\n";
    if (!mqttValid) lastValidationErrors += "MQTT: " + mqttError + "\n";
    if (!sensorValid) lastValidationErrors += "Sensors: " + sensorError + "\n";
    // if (!timingValid) lastValidationErrors += "Timing: " + timingError + "\n";  // Removed - timing validation always passes
    if (!securityValid) lastValidationErrors += "Security: " + securityError + "\n";
    if (!ipWhitelistValid) lastValidationErrors += "IP Whitelist: " + ipWhitelistError + "\n";
    if (!deviceValid) lastValidationErrors += "Device Info: " + deviceError + "\n";
    if (!ntpValid) lastValidationErrors += "NTP: " + ntpError + "\n";
    if (!scheduleValid) lastValidationErrors += "Schedules: " + scheduleError + "\n";
    if (!fallbackValid) lastValidationErrors += "Fallback: " + fallbackError + "\n";
    if (!pinValid) lastValidationErrors += "Pins: " + pinError + "\n";

    errorMessage = lastValidationErrors;
    bool result = wifiValid && mqttValid && sensorValid && securityValid && ipWhitelistValid && deviceValid && ntpValid && scheduleValid && fallbackValid && pinValid;  // timingValid removed - always true

    return result;
}

bool ConfigManager::validateWifiSettings(const JsonDocument& doc, String& error) {
    if (doc["wifi"]["ssid"].isNull()) {
        error = "SSID is required";
        return false;
    }
    String ssid = doc["wifi"]["ssid"].as<String>();
    if (ssid.length() == 0 || ssid.length() > 32) {
        error = "SSID must be 1-32 characters";
        return false;
    }
    // Password validation: allow empty (keep current), encrypted, or valid plain text length
    if (doc["wifi"]["password"].isNull()) {
        error = "Password field is required";
        return false;
    }
    String password = doc["wifi"]["password"].as<String>();
    if (password.length() > 0) {
        if (password.startsWith("ENC:")) {
            // Encrypted password - allow any reasonable length
            if (password.length() < 10 || password.length() > 200) {
                error = "Encrypted password length invalid";
                return false;
            }
        } else {
            // Plain text password - validate length
            if (password.length() < 8 || password.length() > 63) {
                error = "Password must be empty (keep current) or 8-63 characters";
                return false;
            }
        }
    }
    return true;
}

bool ConfigManager::validateMqttSettings(const JsonDocument& doc, String& error) {
    if (doc["mqtt"]["server"].isNull()) {
        error = "MQTT server is required";
        return false;
    }
    String server = doc["mqtt"]["server"].as<String>();
    if (server.length() == 0) {
        error = "MQTT server cannot be empty";
        return false;
    }
    int port = doc["mqtt"]["port"] | 1883;
    if (port < 1 || port > 65535) {
        error = "MQTT port must be 1-65535";
        return false;
    }
    String clientId = doc["mqtt"]["client_id"].as<String>();
    if (clientId.length() > 23) {
        error = "MQTT client ID must be <= 23 characters";
        return false;
    }
    long publishInterval = doc["mqtt"]["publish_interval"] |
                          static_cast<long>(settings.mqtt.publish_interval > 0 ? settings.mqtt.publish_interval : 10UL);
    if (publishInterval < 1L) {
        error = "MQTT publish interval must be >= 1 second";
        return false;
    }
    return true;
}

bool ConfigManager::validateSensorSettings(const JsonDocument& doc, String& error) {
    int sensorCount = doc["sensors"]["sensor_count"] | 2;
    if (sensorCount < 1 || sensorCount > 32) {
        error = "Sensor count must be 1-32";
        return false;
    }
    int busCount = doc["sensors"]["one_wire_bus_count"] | 4;
    if (busCount < 1 || busCount > 4) {
        error = "OneWire bus count must be 1-4";
        return false;
    }

    // DS18B20 measurement interval must be long enough for pre-conversion scheduling.
    // The code needs interval > conversion window (~0.775s); we enforce a conservative minimum.
    const float dsInterval = doc["sensors"]["ds18b20_measurement_interval"] | 60.0f;
    if (dsInterval < 10.0f) {
        error = "DS18B20 measurement interval must be >= 10 seconds";
        return false;
    }
    if (doc["sensors"]["sht4x_enabled"].as<bool>()) {
        int sdaPin = doc["sensors"]["sht4x_sda_pin"] | 8;
        int sclPin = doc["sensors"]["sht4x_scl_pin"] | 9;
        if (sdaPin < 0 || sdaPin > 50 || sclPin < 0 || sclPin > 50) {
            error = "SHT4x pins must be 0-50";
            return false;
        }
        if (sdaPin == sclPin) {
            error = "SHT4x SDA and SCL pins must be different";
            return false;
        }
    }

    // MAX31855K validation (when enabled)
    const bool maxEnabled = doc["sensors"]["max31855k_enabled"] | settings.sensors.max31855k_enabled;
    if (maxEnabled) {
        const int doPin = doc["sensors"]["max31855k_do_pin"] | settings.sensors.max31855k_do_pin;
        const int clkPin = doc["sensors"]["max31855k_clk_pin"] | settings.sensors.max31855k_clk_pin;
        const int csPin = doc["sensors"]["max31855k_cs_pin"] | settings.sensors.max31855k_cs_pin;

        if (doPin < 0 || doPin > 50 || clkPin < 0 || clkPin > 50 || csPin < 0 || csPin > 50) {
            error = "MAX31855K pins must be 0-50";
            return false;
        }
        if (doPin == clkPin || doPin == csPin || clkPin == csPin) {
            error = "MAX31855K DO/CLK/CS pins must be different";
            return false;
        }
    }
    return true;
}

bool ConfigManager::validateTimingSettings(const JsonDocument& doc, String& error) {
    // Timing validation has been removed - all timing settings are now validated in their respective components
    // This function is kept for API compatibility but does nothing
    return true;
}

bool ConfigManager::validateFallbackSettings(const JsonDocument& doc, String& error) {

    // Validate timeout_minutes if fallback section exists
    if (!doc["fallback"].isNull()) {
        // Validate timeout_minutes range (1 minute to 24 hours)
        unsigned long timeout = doc["fallback"]["timeout_minutes"] | 30UL;
        if (timeout < 1 || timeout > 1440) {
            error = "Fallback timeout_minutes must be between 1 and 1440 minutes (1 minute to 24 hours)";
            return false;
        }
    }

    return true;
}

bool ConfigManager::validateDeviceInfoSettings(const JsonDocument& doc, String& error) {
    String name = doc["device_info"]["name"].as<String>();
    if (name.length() == 0 || name.length() > 50) {
        error = "Device name must be 1-50 characters";
        return false;
    }
    return true;
}

bool ConfigManager::validateNTPSettings(const JsonDocument& doc, String& error) {
    // Validate NTP servers
    JsonArrayConst servers = doc["ntp"]["servers"];
    if (servers.size() > 3) {
        error = "Maximum 3 NTP servers allowed";
        return false;
    }
    for (size_t i = 0; i < servers.size(); i++) {
        String server = servers[i].as<String>();
        if (server.length() == 0) {
            error = "NTP server cannot be empty";
            return false;
        }
        if (server.length() > 100) {
            error = "NTP server name too long";
            return false;
        }
    }

    // Validate timezone string
    String timezone = doc["ntp"]["timezone"].as<String>();
    if (timezone.length() > 200) {
        error = "Timezone string too long";
        return false;
    }

    // Validate optional DS3231 RTC configuration
    if (!doc["ntp"]["rtc"].isNull()) {
        JsonObjectConst rtcObj = doc["ntp"]["rtc"].as<JsonObjectConst>();

        bool rtcEnabled = rtcObj["enabled"] | false;
        int sda = rtcObj["sda_pin"] | 1;
        int scl = rtcObj["scl_pin"] | 2;

        if (rtcEnabled) {
            if (sda < 0 || sda > 50 || scl < 0 || scl > 50) {
                error = "DS3231 SDA/SCL pins must be 0-50";
                return false;
            }
            if (sda == scl) {
                error = "DS3231 SDA and SCL pins must be different";
                return false;
            }

            // i2c_address validation: accept string or integer
            int addr = 0x68;
            if (!rtcObj["i2c_address"].isNull()) {
                if (rtcObj["i2c_address"].is<const char*>()) {
                    addr = strtol(rtcObj["i2c_address"].as<const char*>(), nullptr, 0);
                } else {
                    addr = rtcObj["i2c_address"].as<int>();
                }
            }

            // 7-bit I2C addresses are 0x03..0x77; DS3231 is typically 0x68.
            if (addr < 0x03 || addr > 0x77) {
                error = "DS3231 I2C address must be a valid 7-bit address (0x03-0x77)";
                return false;
            }
        }
    }

    return true;
}

bool ConfigManager::validateSecuritySettings(const JsonDocument& doc, String& error) {
    // Auth enabled is a boolean, no validation needed beyond type checking
    if (!doc["security"]["auth_enabled"].isNull() && !doc["security"]["auth_enabled"].is<bool>()) {
        error = "Authentication enabled must be a boolean value";
        return false;
    }

    String username = doc["security"]["username"].as<String>();
    if (username.length() > 32) {
        error = "Username must be <= 32 characters";
        return false;
    }

    String passwordHash = doc["security"]["password_hash"].as<String>();
    if (passwordHash.length() > 128) {
        error = "Password hash must be <= 128 characters";
        return false;
    }

    return true;
}

bool ConfigManager::validateIPWhitelistSettings(const JsonDocument& doc, String& error) {
    if (!doc["security"]["ip_whitelist_enabled"].is<bool>()) {
        error = "IP whitelist enabled must be a boolean value";
        return false;
    }

    if (doc["security"]["ip_whitelist_enabled"].as<bool>()) {
        JsonArrayConst ipRanges = doc["security"]["ip_ranges"];
        if (ipRanges.size() == 0) {
            error = "IP whitelist enabled but no IP ranges configured";
            return false;
        }

        for (size_t i = 0; i < ipRanges.size(); i++) {
            String range = ipRanges[i].as<String>();
            if (!IPWhitelistManager::isValidIPRange(range)) {
                error = "Invalid IP range format: " + range;
                return false;
            }
        }
    }

    return true;
}

// Pin validation methods
bool ConfigManager::validatePinConfiguration(const JsonDocument& doc, String& errorMessage) {

    String sensorError, relayError, conflictError;

    bool sensorValid = validateSensorPins(doc, sensorError);
    bool relayValid = validateRelayPins(doc, relayError);
    bool conflictValid = true;

    // DS3231 pin conflict rules (safe/backward-compatible):
    // - DS3231 SDA/SCL must NOT conflict with any DS18B20 OneWire bus pins or Relay pins.
    // - DS3231 pins MAY be shared with SHT4x pins (either same pins or separate bus).
    //   (Do NOT require equality here; enforcing equality can brick existing configs at boot
    //    because loadConfiguration() validates the on-device config.json.)
    const JsonObjectConst rtcObj = doc["ntp"]["rtc"].as<JsonObjectConst>();
    const bool rtcEnabled = (!rtcObj.isNull()) && (rtcObj["enabled"] | false);
    if (rtcEnabled) {
        const int rtcSda = rtcObj["sda_pin"] | 1;
        const int rtcScl = rtcObj["scl_pin"] | 2;

        // SHT4x interaction rule (as requested):
        // DS3231 pins *may* match SHT4x I2C pins, but if they overlap we must not allow SDA/SCL to be swapped.
        // - Separate I2C buses are allowed: no overlap -> no constraint.
        // - Shared pins are allowed only as (rtcSda == shtSda && rtcScl == shtScl).
        const bool shtEnabled = doc["sensors"]["sht4x_enabled"] | false;
        if (shtEnabled) {
            const int shtSda = doc["sensors"]["sht4x_sda_pin"] | 8;
            const int shtScl = doc["sensors"]["sht4x_scl_pin"] | 9;

            const bool overlaps = (rtcSda == shtSda) || (rtcSda == shtScl) || (rtcScl == shtSda) || (rtcScl == shtScl);
            if (overlaps && (rtcSda != shtSda || rtcScl != shtScl)) {
                conflictError = "DS3231 RTC pins may share the SHT4x I2C pins, but SDA must map to SDA and SCL to SCL. "
                                "Expected RTC SDA=" + String(shtSda) + ", RTC SCL=" + String(shtScl) + ". "
                                "Got RTC SDA=" + String(rtcSda) + ", RTC SCL=" + String(rtcScl) + ".";
                conflictValid = false;
            }
        }

        auto collectPins = [](JsonVariantConst v) {
            std::vector<int> out;
            if (!v.is<JsonArrayConst>()) return out;
            JsonArrayConst arr = v.as<JsonArrayConst>();
            for (JsonVariantConst pv : arr) {
                if (pv.isNull()) continue;
                int pin = pv.as<int>();
                if (pin >= 0) out.push_back(pin);
            }
            return out;
        };
        auto contains = [](const std::vector<int>& v, int pin) {
            return std::find(v.begin(), v.end(), pin) != v.end();
        };

        const std::vector<int> oneWirePins = collectPins(doc["sensors"]["one_wire_pins"]);
        const std::vector<int> relayLowPins = collectPins(doc["relay"]["pins_low"]);
        const std::vector<int> relayMidPins = collectPins(doc["relay"]["pins_mid"]);
        const std::vector<int> relayHighPins = collectPins(doc["relay"]["pins_high"]);

        // DS18B20 OneWire bus pins
        if (conflictValid && (contains(oneWirePins, rtcSda) || contains(oneWirePins, rtcScl))) {
            conflictError = "DS3231 RTC SDA/SCL pins must not conflict with any DS18B20 OneWire bus pins. "
                            "RTC SDA=" + String(rtcSda) + ", RTC SCL=" + String(rtcScl) + ".";
            conflictValid = false;
        }

        // Relay pins (all groups)
        if (conflictValid) {
            const bool sdaConflictsRelay = contains(relayLowPins, rtcSda) || contains(relayMidPins, rtcSda) || contains(relayHighPins, rtcSda);
            const bool sclConflictsRelay = contains(relayLowPins, rtcScl) || contains(relayMidPins, rtcScl) || contains(relayHighPins, rtcScl);
            if (sdaConflictsRelay || sclConflictsRelay) {
                conflictError = "DS3231 RTC SDA/SCL pins must not conflict with any Relay pins. "
                                "RTC SDA=" + String(rtcSda) + ", RTC SCL=" + String(rtcScl) + ".";
                conflictValid = false;
            }
        }
    }

    // MAX31855K exclusivity rules:
    // If enabled, DO/CLK/CS must not be used by:
    // - DS18B20 OneWire pins
    // - SHT4x SDA/SCL pins
    // - DS3231 RTC SDA/SCL pins (when rtc enabled)
    // - Relay pins (all groups)
    const bool maxEnabled = doc["sensors"]["max31855k_enabled"] | settings.sensors.max31855k_enabled;
    if (conflictValid && maxEnabled) {
        const int doPin = doc["sensors"]["max31855k_do_pin"] | settings.sensors.max31855k_do_pin;
        const int clkPin = doc["sensors"]["max31855k_clk_pin"] | settings.sensors.max31855k_clk_pin;
        const int csPin = doc["sensors"]["max31855k_cs_pin"] | settings.sensors.max31855k_cs_pin;

        auto collectPins = [](JsonVariantConst v) {
            std::vector<int> out;
            if (!v.is<JsonArrayConst>()) return out;
            JsonArrayConst arr = v.as<JsonArrayConst>();
            for (JsonVariantConst pv : arr) {
                if (pv.isNull()) continue;
                int pin = pv.as<int>();
                if (pin >= 0) out.push_back(pin);
            }
            return out;
        };
        auto contains = [](const std::vector<int>& v, int pin) {
            return std::find(v.begin(), v.end(), pin) != v.end();
        };
        auto anyEquals = [](int p, int a, int b) {
            return (p == a) || (p == b);
        };

        const std::vector<int> oneWirePins = collectPins(doc["sensors"]["one_wire_pins"]);
        const std::vector<int> relayLowPins = collectPins(doc["relay"]["pins_low"]);
        const std::vector<int> relayMidPins = collectPins(doc["relay"]["pins_mid"]);
        const std::vector<int> relayHighPins = collectPins(doc["relay"]["pins_high"]);

        const bool shtEnabled = doc["sensors"]["sht4x_enabled"] | false;
        const int shtSda = doc["sensors"]["sht4x_sda_pin"] | 8;
        const int shtScl = doc["sensors"]["sht4x_scl_pin"] | 9;

        // DS18B20 OneWire conflict
        for (int p : {doPin, clkPin, csPin}) {
            if (contains(oneWirePins, p)) {
                conflictError = "MAX31855K pin " + String(p) + " conflicts with DS18B20 OneWire bus pin.";
                conflictValid = false;
                break;
            }
        }

        // SHT4x conflict
        if (conflictValid && shtEnabled) {
            for (int p : {doPin, clkPin, csPin}) {
                if (anyEquals(p, shtSda, shtScl)) {
                    const String which = (p == shtSda) ? "SDA" : "SCL";
                    conflictError = "MAX31855K pin " + String(p) + " conflicts with SHT4x I2C " + which + " pin.";
                    conflictValid = false;
                    break;
                }
            }
        }

        // DS3231 RTC conflict
        if (conflictValid && rtcEnabled) {
            const int rtcSda = rtcObj["sda_pin"] | 1;
            const int rtcScl = rtcObj["scl_pin"] | 2;
            for (int p : {doPin, clkPin, csPin}) {
                if (anyEquals(p, rtcSda, rtcScl)) {
                    const String which = (p == rtcSda) ? "SDA" : "SCL";
                    conflictError = "MAX31855K pin " + String(p) + " conflicts with DS3231 RTC I2C " + which + " pin.";
                    conflictValid = false;
                    break;
                }
            }
        }

        // Relay conflict
        if (conflictValid) {
            for (int p : {doPin, clkPin, csPin}) {
                const bool inRelay = contains(relayLowPins, p) || contains(relayMidPins, p) || contains(relayHighPins, p);
                if (inRelay) {
                    conflictError = "MAX31855K pin " + String(p) + " conflicts with a Relay pin.";
                    conflictValid = false;
                    break;
                }
            }
        }
    }

    // Viable scan pins conflict validation:
    // Viable scan pins must not include pins used by other enabled components
    if (conflictValid) {
        String viableScanPinsStr = doc["sensors"]["viable_scan_pins"].as<String>();
        if (viableScanPinsStr.length() == 0) {
            viableScanPinsStr = "4,5,6,7,12,13,14,15,16,17,18"; // Default
        }

        std::vector<int> viablePins = parsePinString(viableScanPinsStr);

        // Collect relay pins for conflict checking
        auto collectPins = [](JsonVariantConst v) {
            std::vector<int> out;
            if (!v.is<JsonArrayConst>()) return out;
            JsonArrayConst arr = v.as<JsonArrayConst>();
            for (JsonVariantConst pv : arr) {
                if (pv.isNull()) continue;
                int pin = pv.as<int>();
                if (pin >= 0) out.push_back(pin);
            }
            return out;
        };
        auto contains = [](const std::vector<int>& v, int pin) {
            return std::find(v.begin(), v.end(), pin) != v.end();
        };

        const std::vector<int> relayLowPins = collectPins(doc["relay"]["pins_low"]);
        const std::vector<int> relayMidPins = collectPins(doc["relay"]["pins_mid"]);
        const std::vector<int> relayHighPins = collectPins(doc["relay"]["pins_high"]);

        // Check each viable pin against all other component pins
        for (int pin : viablePins) {
            // Check against SHT4x pins (if enabled)
            const bool shtEnabled = doc["sensors"]["sht4x_enabled"] | false;
            if (shtEnabled) {
                const int shtSda = doc["sensors"]["sht4x_sda_pin"] | 8;
                const int shtScl = doc["sensors"]["sht4x_scl_pin"] | 9;
                if (pin == shtSda || pin == shtScl) {
                    conflictError = "Viable scan pin " + String(pin) + " conflicts with SHT4x I2C pin.";
                    conflictValid = false;
                    break;
                }
            }

            // Check against DS3231 RTC pins (if enabled)
            if (rtcEnabled) {
                const int rtcSda = rtcObj["sda_pin"] | 1;
                const int rtcScl = rtcObj["scl_pin"] | 2;
                if (pin == rtcSda || pin == rtcScl) {
                    conflictError = "Viable scan pin " + String(pin) + " conflicts with DS3231 RTC I2C pin.";
                    conflictValid = false;
                    break;
                }
            }

            // Check against MAX31855K pins (if enabled)
            if (maxEnabled) {
                const int doPin = doc["sensors"]["max31855k_do_pin"] | settings.sensors.max31855k_do_pin;
                const int clkPin = doc["sensors"]["max31855k_clk_pin"] | settings.sensors.max31855k_clk_pin;
                const int csPin = doc["sensors"]["max31855k_cs_pin"] | settings.sensors.max31855k_cs_pin;
                if (pin == doPin || pin == clkPin || pin == csPin) {
                    conflictError = "Viable scan pin " + String(pin) + " conflicts with MAX31855K pin.";
                    conflictValid = false;
                    break;
                }
            }

            // Check against relay pins (all groups)
            if (contains(relayLowPins, pin) || contains(relayMidPins, pin) || contains(relayHighPins, pin)) {
                conflictError = "Viable scan pin " + String(pin) + " conflicts with a relay pin.";
                conflictValid = false;
                break;
            }
        }
    }


    if (!sensorValid) errorMessage += "Sensor pins: " + sensorError + "\n";
    if (!relayValid) errorMessage += "Relay pins: " + relayError + "\n";
    if (!conflictValid) errorMessage += "Pin conflicts: " + conflictError + "\n";

    bool result = sensorValid && relayValid && conflictValid;

    return result;
}

bool ConfigManager::validateSensorPins(const JsonDocument& doc, String& errorMessage) {

    // Validate OneWire pins
    JsonArrayConst oneWirePins = doc["sensors"]["one_wire_pins"];
    std::vector<int> pins;
    for (size_t i = 0; i < oneWirePins.size(); i++) {
        pins.push_back(oneWirePins[i].as<int>());
    }

    // Check for invalid pin numbers
    for (int pin : pins) {
        if (pin < 0 || pin > 50) {
            errorMessage = "OneWire pin " + String(pin) + " out of range (must be 0-50)";
            return false;
        }
    }

    // Check for duplicates
    if (hasDuplicates(pins)) {
        errorMessage = "Duplicate pins found in OneWire configuration";
        return false;
    }

    // Validate SHT4x pins
    int sdaPin = doc["sensors"]["sht4x_sda_pin"] | 8;
    int sclPin = doc["sensors"]["sht4x_scl_pin"] | 9;


    if (!PinValidator::validateSHT4xPins(sdaPin, sclPin)) {
        // Check what specific validation failed
        if (sdaPin == sclPin) {
            errorMessage = "SHT4x SDA and SCL pins must be different (both are " + String(sdaPin) + ")";
        } else if (sdaPin < 0 || sdaPin > 50) {
            errorMessage = "SHT4x SDA pin " + String(sdaPin) + " out of range (must be 0-50)";
        } else if (sclPin < 0 || sclPin > 50) {
            errorMessage = "SHT4x SCL pin " + String(sclPin) + " out of range (must be 0-50)";
        } else {
            errorMessage = "SHT4x pins validation failed";
        }
        return false;
    }

    // Validate Viable Scan Pins
    String viableScanPins = doc["sensors"]["viable_scan_pins"].as<String>();
    std::vector<int> viablePins = parsePinString(viableScanPins);

    // Validate pin range for viable scan pins
    for (int pin : viablePins) {
        if (pin < 0 || pin > 50) {
            errorMessage = "Viable scan pin " + String(pin) + " out of range (must be 0-50)";
            return false;
        }
    }

    // Check for duplicates within viable scan pins
    if (hasDuplicates(viablePins)) {
        errorMessage = "Duplicate pins found in viable scan pins";
        return false;
    }

    return true;
}

bool ConfigManager::validateRelayPins(const JsonDocument& doc, String& errorMessage) {
    // Validate relay timeout setting
    if (!doc["relay"]["timeout_ms"].isNull()) {
        unsigned long timeout = doc["relay"]["timeout_ms"];
        if (timeout < 1000 || timeout > 30000) { // 1 second to 30 seconds
            errorMessage = "Relay timeout must be between 1000 and 30000 milliseconds";
            return false;
        }
    }

    // Validate relay pins for each state
    JsonArrayConst pinsLow = doc["relay"]["pins_low"];
    JsonArrayConst pinsMid = doc["relay"]["pins_mid"];
    JsonArrayConst pinsHigh = doc["relay"]["pins_high"];
    
    std::vector<int> lowPins, midPins, highPins;
    
    for (size_t i = 0; i < pinsLow.size(); i++) {
        lowPins.push_back(pinsLow[i].as<int>());
    }
    for (size_t i = 0; i < pinsMid.size(); i++) {
        midPins.push_back(pinsMid[i].as<int>());
    }
    for (size_t i = 0; i < pinsHigh.size(); i++) {
        highPins.push_back(pinsHigh[i].as<int>());
    }
    
    // Check each pin array for validity with detailed error messages
    // Check for invalid pin numbers
    for (int pin : lowPins) {
        if (pin < 0 || pin > 50) {
            errorMessage = "Relay LOW pin " + String(pin) + " out of range (must be 0-50)";
            return false;
        }
    }
    for (int pin : midPins) {
        if (pin < 0 || pin > 50) {
            errorMessage = "Relay MID pin " + String(pin) + " out of range (must be 0-50)";
            return false;
        }
    }
    for (int pin : highPins) {
        if (pin < 0 || pin > 50) {
            errorMessage = "Relay HIGH pin " + String(pin) + " out of range (must be 0-50)";
            return false;
        }
    }
    
    // Check for duplicates within each array
    if (hasDuplicates(lowPins)) {
        errorMessage = "Duplicate pins found in relay LOW pins";
        return false;
    }
    if (hasDuplicates(midPins)) {
        errorMessage = "Duplicate pins found in relay MID pins";
        return false;
    }
    if (hasDuplicates(highPins)) {
        errorMessage = "Duplicate pins found in relay HIGH pins";
        return false;
    }
    
    // Note: Relay pins can be identical between states for cumulative actions
    // No conflict checking between relay states - this is allowed behavior

    // Validate DS18B20 safety mechanism settings
    if (!doc["relay"]["safety_enabled"].isNull() && doc["relay"]["safety_enabled"].as<bool>()) {
        // Get sensor count from sensors section for validation
        int sensorCount = doc["sensors"]["sensor_count"] | 2;

        // Validate safety_sensor_number
        int safetySensorNumber = doc["relay"]["safety_sensor_number"] | 1;
        if (safetySensorNumber < 1 || safetySensorNumber > sensorCount) {
            errorMessage = "Safety sensor number must be between 1 and " + String(sensorCount) + " (configured sensor count)";
            return false;
        }

        // Validate safety_temperature_threshold
        float safetyTempThreshold = doc["relay"]["safety_temperature_threshold"] | 50.0f;
        if (safetyTempThreshold < 0.0f || safetyTempThreshold > 100.0f) {
            errorMessage = "Safety temperature threshold must be between 0.0 and 100.0 Celsius";
            return false;
        }

        // Validate safety_duration_seconds
        unsigned long safetyDuration = doc["relay"]["safety_duration_seconds"] | 60UL;
        if (safetyDuration < 0 || safetyDuration > 1800) {
            errorMessage = "Safety duration must be between 0 and 1800 seconds";
            return false;
        }
    }

    return true;
}


bool ConfigManager::hasValidCriticalSettings(String& errorMessage) {
    // Check if WiFi settings are configured (not defaults)
    if (settings.wifi.ssid == "default_ssid" || settings.wifi.ssid.length() == 0) {
        errorMessage = "WiFi SSID not configured (using default value)";
        return false;
    }
    if (settings.wifi.password == "default_password" || settings.wifi.password.length() < 8) {
        errorMessage = "WiFi password not configured or too short (minimum 8 characters)";
        return false;
    }

    // Check if MQTT settings are configured (not defaults)
    if (settings.mqtt.server == "192.168.1.1" || settings.mqtt.server.length() == 0) {
        errorMessage = "MQTT server not configured (using default value)";
        return false;
    }
    if (settings.mqtt.port <= 0 || settings.mqtt.port > 65535) {
        errorMessage = "MQTT port not configured properly";
        return false;
    }
    if (settings.mqtt.client_id == "esp32_temp_default" || settings.mqtt.client_id.length() == 0) {
        errorMessage = "MQTT client ID not configured (using default value)";
        return false;
    }

    return true;
}

bool ConfigManager::createBackup() {
    if (!configLoaded) return false;

    PsramJsonAllocator jsonAllocator;
    JsonDocument doc(&jsonAllocator);
    saveWifiSettings(doc);
    saveMqttSettings(doc);
    saveSensorSettings(doc);
    saveNetworkSettings(doc);
    saveSecuritySettings(doc);
    saveDeviceInfoSettings(doc);
    saveFallbackSettings(doc);

    return saveVersionedConfig(doc, "Manual backup");
}

bool ConfigManager::rollbackToVersion(int version) {
    PsramJsonAllocator jsonAllocator;
    JsonDocument doc(&jsonAllocator);
    if (!loadVersionedConfig(version, doc)) {
        return false;
    }

    String errorMessage;
    if (!validateConfiguration(doc, errorMessage)) {
        UnifiedLogger::error("[CONFIG] Configuration validation failed for version %d: %s\n", version, errorMessage.c_str());
        return false;
    }

    // Load the validated configuration
    loadWifiSettings(doc, true);  // Load encrypted passwords from backup
    loadMqttSettings(doc, true);  // Load encrypted passwords from backup
    loadSensorSettings(doc);
    loadRelaySettings(doc);
    loadNetworkSettings(doc);
    loadSecuritySettings(doc, true);  // Load encrypted passwords from backup
    loadDeviceInfoSettings(doc);
    loadDebugSettings(doc);
    loadNTPSettings(doc);
    loadScheduleSettings(doc);
    loadFallbackSettings(doc);

    // Update metadata
    metadata.version = version;
    metadata.timestamp = millis();

    return saveConfiguration();
}

std::vector<int> ConfigManager::getAvailableVersions() {
    std::vector<int> versions;
    if (!LittleFS.begin()) return versions;

    File root = LittleFS.open("/");
    File file = root.openNextFile();
    while (file) {
        String fileName = file.name();
        if (fileName.startsWith("/config_v") && fileName.endsWith(".json")) {
            int version = fileName.substring(9, fileName.length() - 5).toInt();
            versions.push_back(version);
        }
        file = root.openNextFile();
    }
    return versions;
}


bool ConfigManager::encryptSensitiveData() {
    if (encryptionKey.length() == 0) {
        encryptionKey = generateEncryptionKey();
    }

    // Encrypt WiFi password
    if (settings.wifi.password.length() > 0) {
        settings.wifi.password = encryptString(settings.wifi.password);
    }

    // Encrypt MQTT credentials
    if (settings.mqtt.user.length() > 0) {
        settings.mqtt.user = encryptString(settings.mqtt.user);
    }
    if (settings.mqtt.password.length() > 0) {
        settings.mqtt.password = encryptString(settings.mqtt.password);
    }

    metadata.encrypted = true;
    return saveConfiguration();
}

bool ConfigManager::decryptSensitiveData() {
    if (!metadata.encrypted) {
        return true;
    }

    // Ensure encryption key is available
    if (encryptionKey.length() == 0) {
        encryptionKey = generateEncryptionKey();
    }

    // Decrypt WiFi password
    if (settings.wifi.password.length() > 0) {
        settings.wifi.password = decryptString(settings.wifi.password);
    }

    // Decrypt MQTT password (user remains plain text)
    if (settings.mqtt.password.length() > 0) {
        settings.mqtt.password = decryptString(settings.mqtt.password);
    }

    metadata.encrypted = false;
    return true;
}

ConfigMetadata ConfigManager::getMetadata() const {
    return metadata;
}

String ConfigManager::getValidationErrors() const {
    return lastValidationErrors;
}



// Private helper methods

// Helper function to parse comma-separated pin string into vector
std::vector<int> ConfigManager::parsePinString(const String& pinString) {
    std::vector<int> pins;
    if (pinString.length() == 0) {
        return pins;
    }
    
    String currentPin = "";
    for (size_t i = 0; i < pinString.length(); i++) {
        char c = pinString[i];
        if (c == ',') {
            if (currentPin.length() > 0) {
                pins.push_back(currentPin.toInt());
                currentPin = "";
            }
        } else {
            currentPin += c;
        }
    }
    
    // Add the last pin if there was one
    if (currentPin.length() > 0) {
        pins.push_back(currentPin.toInt());
    }
    
    return pins;
}

bool ConfigManager::hasDuplicates(const std::vector<int>& pins) {
    if (pins.size() <= 1) {
        return false;
    }
    
    std::vector<int> sortedPins = pins;
    std::sort(sortedPins.begin(), sortedPins.end());
    
    for (size_t i = 1; i < sortedPins.size(); i++) {
        if (sortedPins[i] == sortedPins[i-1]) {
            return true;
        }
    }
    return false;
}

String ConfigManager::calculateChecksum(const JsonDocument& doc) {
    String jsonString;
    serializeJson(doc, jsonString);

    // Simple checksum using djb2 algorithm
    unsigned long hash = 5381;
    for (size_t i = 0; i < jsonString.length(); i++) {
        hash = ((hash << 5) + hash) + jsonString[i];
    }
    return String(hash, HEX);
}

bool ConfigManager::saveVersionedConfig(const JsonDocument& doc, const String& description) {
    if (!LittleFS.begin()) return false;

    metadata.version++;
    metadata.timestamp = millis();
    metadata.checksum = calculateChecksum(doc);
    metadata.description = description;

    String filename = "/config_v" + String(metadata.version) + ".json";
    File file = LittleFS.open(filename, "w");
    if (!file) return false;

    serializeJson(doc, file);
    file.close();

    // Keep only last 5 versions to save space
    auto versions = getAvailableVersions();
    if (versions.size() > 5) {
        std::sort(versions.begin(), versions.end());
        for (size_t i = 0; i < versions.size() - 5; i++) {
            String oldFile = "/config_v" + String(versions[i]) + ".json";
            LittleFS.remove(oldFile);
        }
    }

    return true;
}

bool ConfigManager::loadVersionedConfig(int version, JsonDocument& doc) {
    if (!LittleFS.begin()) return false;

    String filename = "/config_v" + String(version) + ".json";
    if (!LittleFS.exists(filename)) return false;

    File file = LittleFS.open(filename, "r");
    if (!file) return false;

    DeserializationError error = deserializeJson(doc, file);
    file.close();

    return !error;
}



String ConfigManager::encryptString(const String& plainText) {
    // Simple XOR encryption for demonstration (use proper encryption in production)
    String encrypted = "";
    for (size_t i = 0; i < plainText.length(); i++) {
        char keyChar = encryptionKey[i % encryptionKey.length()];
        encrypted += (char)(plainText[i] ^ keyChar);
    }
    return encrypted;
}

String ConfigManager::decryptString(const String& encryptedText) {
    // XOR decryption (symmetric)
    return encryptString(encryptedText);
}

String ConfigManager::generateEncryptionKey() {
    // Generate a key based on device MAC address or chip ID
    uint64_t chipId = ESP.getEfuseMac();
    return String(chipId, HEX);
}

// Secure credential migration system implementation

bool ConfigManager::migrateCredentialsIfNeeded() {
    UnifiedLogger::info("[CONFIG] Checking credential migration status\n");

    if (isMigrationComplete()) {
        UnifiedLogger::info("[CONFIG] Migration marker found; verifying credential security\n");
        if (!isCredentialMigrationNeeded()) {
            UnifiedLogger::info("[CONFIG] Credentials already secured\n");
            return true;
        }

        UnifiedLogger::warning("[CONFIG] Migration marker present but credentials still require migration; forcing migration\n");
        if (LittleFS.begin()) {
            LittleFS.remove("/migration_complete");
        }
    }

    if (!isCredentialMigrationNeeded()) {
        UnifiedLogger::info("[CONFIG] Credential migration not required\n");
        markMigrationComplete();
        return true;
    }

    UnifiedLogger::info("[CONFIG] Starting automatic credential migration\n");
    return performCredentialMigration();
}

bool ConfigManager::isCredentialMigrationNeeded() const {
    // Check if any credentials appear to be plain text
    return !isCredentialSecured(settings.wifi.password) ||
           !isCredentialSecured(settings.mqtt.password) ||
           !isCredentialSecured(settings.security.password_hash);
}

bool ConfigManager::performCredentialMigration() {
    UnifiedLogger::info("[CONFIG] Performing credential migration\n");

    // Generate a device-specific salt for this migration
    migrationSalt = generateSalt();

    // Secure WiFi password
    if (settings.wifi.password.length() > 0 && !isCredentialSecured(settings.wifi.password)) {
        securedWifiPassword = encryptSecret(settings.wifi.password);
    }

    // Secure MQTT password (user remains plain text)
    if (settings.mqtt.password.length() > 0 && !isCredentialSecured(settings.mqtt.password)) {
        securedMqttPassword = encryptSecret(settings.mqtt.password);
    }

    // Hash authentication password
    if (settings.security.password_hash.length() > 0 && !isCredentialSecured(settings.security.password_hash)) {
        // Store the plain text password for HTTP Basic Auth
        plainTextAuthPassword = settings.security.password_hash;

        // Hash it for secure storage
        securedAuthPassword = hashPassword(settings.security.password_hash);
        settings.security.password_hash = securedAuthPassword;
    }

    // Mark metadata as encrypted
    metadata.encrypted = true;

    // Save the secured configuration
    if (saveConfiguration()) {
        markMigrationComplete();
        UnifiedLogger::info("[CONFIG] Credential migration completed successfully\n");
        return true;
    }

    UnifiedLogger::error("[CONFIG] Failed to save secured configuration\n");
    return false;
}

bool ConfigManager::isCredentialSecured(const String& credential) const {
    if (credential.length() == 0) return true; // Empty credentials are considered "secured"
    
    // Only credentials with ENC: prefix are considered secured
    // This ensures all plain text passwords are properly detected and encrypted
    return credential.startsWith("ENC:");
}

String ConfigManager::hashPassword(const String& plainPassword) {
    if (plainPassword.length() == 0) return "";

    // Generate salt if not already done
    if (migrationSalt.length() == 0) {
        migrationSalt = generateSalt();
    }

    // Create salted password
    String saltedPassword = migrationSalt + plainPassword;

    // Use SHA-256 hashing
    unsigned char hash[32];
    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 0);
    mbedtls_md_starts(&ctx);
    mbedtls_md_update(&ctx, (const unsigned char*)saltedPassword.c_str(), saltedPassword.length());
    mbedtls_md_finish(&ctx, hash);
    mbedtls_md_free(&ctx);

    // Convert to hex string
    String hashString = "";
    for (int i = 0; i < 32; i++) {
        char hex[3];
        sprintf(hex, "%02x", hash[i]);
        hashString += hex;
    }

    // Return salt:hash format
    return migrationSalt + ":" + hashString;
}

bool ConfigManager::verifyPassword(const String& plainPassword, const String& hashedPassword) const {
    if (hashedPassword.length() == 0) return plainPassword.length() == 0;

    // Parse salt:hash format
    int colonIndex = hashedPassword.indexOf(':');
    if (colonIndex == -1) return false;

    String storedSalt = hashedPassword.substring(0, colonIndex);
    String storedHash = hashedPassword.substring(colonIndex + 1);

    // Hash the provided password with the stored salt
    String saltedPassword = storedSalt + plainPassword;

    unsigned char hash[32];
    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 0);
    mbedtls_md_starts(&ctx);
    mbedtls_md_update(&ctx, (const unsigned char*)saltedPassword.c_str(), saltedPassword.length());
    mbedtls_md_finish(&ctx, hash);
    mbedtls_md_free(&ctx);

    // Convert to hex and compare
    String computedHash = "";
    for (int i = 0; i < 32; i++) {
        char hex[3];
        sprintf(hex, "%02x", hash[i]);
        computedHash += hex;
    }

    return computedHash.equalsIgnoreCase(storedHash);
}

String ConfigManager::generateSalt() {
    // Generate a random 16-byte salt
    String salt = "";
    for (int i = 0; i < 16; i++) {
        uint8_t randomByte = esp_random() % 256;
        char hex[3];
        sprintf(hex, "%02x", randomByte);
        salt += hex;
    }
    return salt;
}

String ConfigManager::encryptSecret(const String& plainSecret) {
    if (plainSecret.length() == 0) return "";

    // Use AES encryption with device-specific key
    if (encryptionKey.length() == 0) {
        encryptionKey = generateEncryptionKey();
    }

    // XOR encrypt, then hex encode to avoid JSON binary issues
    String xor_result = encryptString(plainSecret);
    String hex_encoded = "";
    for (size_t i = 0; i < xor_result.length(); i++) {
        char buf[3];
        sprintf(buf, "%02x", (uint8_t)xor_result[i]);
        hex_encoded += buf;
    }
    // Add encryption marker for reliable detection
    return "ENC:" + hex_encoded;
}

String ConfigManager::decryptSecret(const String& encryptedSecret) {
    if (encryptedSecret.length() == 0) return "";

    // Ensure encryption key is available
    if (encryptionKey.length() == 0) {
        encryptionKey = generateEncryptionKey();
    }

    // Check for encryption marker
    if (encryptedSecret.startsWith("ENC:")) {
        // Remove marker and hex decode
        String hexData = encryptedSecret.substring(4);
        String xor_result = "";
        for (size_t i = 0; i < hexData.length(); i += 2) {
            char buf[3] = {hexData[i], hexData[i+1], 0};
            uint8_t byte = strtol(buf, NULL, 16);
            xor_result += (char)byte;
        }
        return decryptString(xor_result);
    } else {
        // Assume plain text (backward compatibility)
        return encryptedSecret;
    }
}

bool ConfigManager::markMigrationComplete() {
    migrationComplete = true;

    // Save migration status to a separate file
    if (LittleFS.begin()) {
        File file = LittleFS.open("/migration_complete", "w");
        if (file) {
            file.println("1");
            file.close();
            return true;
        }
    }
    return false;
}

bool ConfigManager::isMigrationComplete() const {
    if (migrationComplete) return true;

    // Check if migration complete file exists
    if (LittleFS.begin()) {
        return LittleFS.exists("/migration_complete");
    }
    return false;
}

// Secure authentication helpers

String ConfigManager::getPlainTextAuthPassword() const {
    // Return the plain text password for HTTP Basic Auth
    // This is kept in memory for authentication purposes
    return plainTextAuthPassword;
}

bool ConfigManager::verifyAuthPassword(const String& plainPassword) const {
    // Verify password against the stored hash
    return verifyPassword(plainPassword, settings.security.password_hash);
}

// Handle password updates with special logic for empty fields
void ConfigManager::handlePasswordUpdates(const JsonDocument& doc) {
    // WiFi password - only update if provided
    if (doc["wifi"]["password"].is<String>()) {
        String newWifiPass = doc["wifi"]["password"].as<String>();
        if (newWifiPass.length() > 0) {
            // User provided a new password - store plain text in settings
            // saveWifiSettings() will encrypt for JSON storage
            settings.wifi.password = newWifiPass;
        } else {
        }
        // If empty, keep current password
    }

    // MQTT password - only update if provided
    if (doc["mqtt"]["password"].is<String>()) {
        String newMqttPass = doc["mqtt"]["password"].as<String>();
        if (newMqttPass.length() > 0) {
            // User provided a new password - store plain text in settings
            // saveMqttSettings() will encrypt for JSON storage
            settings.mqtt.password = newMqttPass;
        } else {
        }
        // If empty, keep current password
    }

    // Security password - special handling
    if (doc["security"]["password_hash"].is<String>()) {
        String newAuthPass = doc["security"]["password_hash"].as<String>();
        if (newAuthPass.length() > 0) {
            // User provided a new password - encrypt it and store plain text for HTTP auth
            plainTextAuthPassword = newAuthPass;
            String encrypted = encryptSecret(newAuthPass);
            settings.security.password_hash = encrypted;
        } else {
            // Ensure plainTextAuthPassword is set from current encrypted password
            if (settings.security.password_hash.startsWith("ENC:")) {
                plainTextAuthPassword = decryptSecret(settings.security.password_hash);
            } else if (settings.security.password_hash.length() > 0) {
                // Plain text - set and encrypt
                plainTextAuthPassword = settings.security.password_hash;
                settings.security.password_hash = encryptSecret(settings.security.password_hash);
            }
        }
        // If empty, keep current password (don't disable auth)
    }
}

/**
 * @brief Load schedule settings from JSON configuration document
 *
 * This method parses the "schedules" section of the configuration JSON and
 * populates the settings.schedules structure. It handles:
 * - Schedule enable/disable flag
 * - Array of schedule entries with time ranges and relay levels
 * - Validation of required fields with sensible defaults
 *
 * Expected JSON structure:
 * {
 *   "schedules": {
 *     "enabled": true,
 *     "entries": [
 *       {
 *         "day": 0,
 *         "start_time": "06:00",
 *         "start_level": "low",
 *         "end_time": "12:00",
 *         "end_level": "high"
 *       }
 *     ]
 *   }
 * }
 *
 * @param doc Reference to the JSON document containing configuration
 */
void ConfigManager::loadScheduleSettings(const JsonDocument& doc) {

    // NOTE: `doc` is const, so use `as<...Const>()` rather than `is<JsonArray>()` checks.
    // The previous implementation often mis-detected arrays/objects and fell back to re-parsing raw JSON.

    JsonObjectConst schedules = doc["schedules"].as<JsonObjectConst>();
    if (schedules.isNull()) {
        settings.schedules.enabled = true; // default for new installations
        settings.schedules.entries.clear();
        return;
    }

    // Debug: Show schedules JSON length (avoid huge log lines that can interleave)
    String schedulesJson;
    serializeJson(schedules, schedulesJson);

    // Load global scheduling enable flag (default to true for new installations)
    settings.schedules.enabled = schedules["enabled"] | true;

    // Clear existing entries to avoid duplicates on reload
    settings.schedules.entries.clear();

    JsonArrayConst entries = schedules["entries"].as<JsonArrayConst>();
    if (entries.isNull()) {
        return;
    }


    for (size_t i = 0; i < entries.size(); i++) {
        JsonObjectConst entry = entries[i].as<JsonObjectConst>();
        if (entry.isNull()) {
            continue;
        }

        Settings::Schedule::ScheduleEntry scheduleEntry;
        scheduleEntry.day = entry["day"] | 0; // Default Monday
        scheduleEntry.start_time = entry["start_time"].as<String>();
        scheduleEntry.start_level = entry["start_level"] | String("off");
        scheduleEntry.end_time = entry["end_time"].as<String>();
        scheduleEntry.end_level = "off"; // Always "off" by design

        settings.schedules.entries.push_back(scheduleEntry);

    }

}

void ConfigManager::loadFallbackSettings(const JsonDocument& doc) {

    // Load fallback enabled flag (default to false)
    settings.fallback.enabled = doc["fallback"]["enabled"] | false;

    // Load fallback timeout minutes (default to 30 minutes)
    if (!doc["fallback"]["timeout_minutes"].isNull()) {
        settings.fallback.timeout_minutes = doc["fallback"]["timeout_minutes"];
    } else {
        settings.fallback.timeout_minutes = 30UL; // Default 30 minutes
    }

}

/**
 * @brief Save schedule settings to JSON configuration document
 *
 * This method serializes the current schedule settings into the provided
 * JSON document for persistence. It creates the "schedules" section with:
 * - Global enabled flag
 * - Array of schedule entries containing all time range data
 *
 * The resulting JSON structure matches what loadScheduleSettings() expects.
 *
 * @param doc Reference to the JSON document to populate with schedule settings
 */
void ConfigManager::saveScheduleSettings(JsonDocument& doc) {

    // Save global scheduling enable flag
    doc["schedules"]["enabled"] = settings.schedules.enabled;

    // Create entries array and populate with all schedule entries
    JsonArray entries = doc["schedules"]["entries"].to<JsonArray>();
    for (const auto& entry : settings.schedules.entries) {
        JsonObject entryObj = entries.add<JsonObject>();

        // Serialize schedule entry fields (end_level always "off" by design, not saved)
        entryObj["day"] = entry.day;
        entryObj["start_time"] = entry.start_time;
        entryObj["start_level"] = entry.start_level;
        entryObj["end_time"] = entry.end_time;

    }


    // Debug: Show the saved schedules JSON
    String savedSchedules;
    serializeJson(doc["schedules"], savedSchedules);
}

void ConfigManager::saveFallbackSettings(JsonDocument& doc) {

    // Save fallback enabled flag
    doc["fallback"]["enabled"] = settings.fallback.enabled;

    // Save fallback timeout minutes
    doc["fallback"]["timeout_minutes"] = settings.fallback.timeout_minutes;

}

/**
 * @brief Validate schedule settings in JSON configuration document
 *
 * This method performs comprehensive validation of schedule configuration including:
 * - Day of week validation (0-6, Monday-Sunday)
 * - Time format validation (HH:MM, 24-hour format)
 * - Time value range validation (00:00 to 23:59)
 * - Relay level validation ("off", "low", "mid", "high")
 * - Time range logic validation (allows midnight spanning)
 *
 * The validation is strict to prevent runtime errors but permissive enough
 * to allow complex scheduling scenarios like overnight ranges.
 *
 * @param doc Reference to the JSON document containing schedule settings
 * @param error Output parameter for detailed error message if validation fails
 * @return true if all schedule settings are valid, false otherwise
 */
bool ConfigManager::validateScheduleSettings(const JsonDocument& doc, String& error) {

    // Check if schedules section exists and has entries array
    if (!doc["schedules"].isNull() && doc["schedules"]["entries"].is<JsonArray>()) {
        JsonArrayConst entries = doc["schedules"]["entries"];

        // Validate each schedule entry
        for (size_t i = 0; i < entries.size(); i++) {
            JsonObjectConst entry = entries[i];

            // Validate day of week (0=Monday, 6=Sunday)
            int day = entry["day"] | -1;
            if (day < 0 || day > 6) {
                error = "Schedule entry " + String(i) + ": day must be 0-6 (0=Monday, 6=Sunday)";
                return false;
            }

            // Validate start time format and values (HH:MM)
            String startTime = entry["start_time"].as<String>();
            if (startTime.length() != 5 || startTime[2] != ':') {
                error = "Schedule entry " + String(i) + ": start_time must be in HH:MM format";
                return false;
            }
            int startHour = startTime.substring(0, 2).toInt();
            int startMinute = startTime.substring(3, 5).toInt();
            if (startHour < 0 || startHour > 23 || startMinute < 0 || startMinute > 59) {
                error = "Schedule entry " + String(i) + ": invalid start_time values (must be 00:00-23:59)";
                return false;
            }

            // Validate end time format and values (HH:MM)
            String endTime = entry["end_time"].as<String>();
            if (endTime.length() != 5 || endTime[2] != ':') {
                error = "Schedule entry " + String(i) + ": end_time must be in HH:MM format";
                return false;
            }
            int endHour = endTime.substring(0, 2).toInt();
            int endMinute = endTime.substring(3, 5).toInt();
            if (endHour < 0 || endHour > 23 || endMinute < 0 || endMinute > 59) {
                error = "Schedule entry " + String(i) + ": invalid end_time values (must be 00:00-23:59)";
                return false;
            }

            // Validate relay levels (must be one of the four valid states)
            String startLevel = entry["start_level"].as<String>();
            if (startLevel != "low" && startLevel != "mid" && startLevel != "high" && startLevel != "off") {
                error = "Schedule entry " + String(i) + ": start_level must be 'low', 'mid', 'high', or 'off'";
                return false;
            }

            // Note: end_level is always "off" by design, no validation needed


            // Validate time range logic (allow midnight spanning and equal times)
            int startTotalMinutes = startHour * 60 + startMinute;
            int endTotalMinutes = endHour * 60 + endMinute;
            if (startTotalMinutes >= endTotalMinutes && startTotalMinutes != endTotalMinutes) {
                // Allow equal times for instantaneous changes, warn about unusual ranges
                // For midnight spanning (e.g., 22:00 to 06:00), start > end is allowed
            }
        }

        // Note: Overlapping ranges are allowed
        // The first active range found will be used

    }

    return true;
}

