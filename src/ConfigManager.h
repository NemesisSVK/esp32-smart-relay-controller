#ifndef CONFIGMANAGER_H
#define CONFIGMANAGER_H

#include <ArduinoJson.h>
#include <LittleFS.h>
#include <vector>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "IPWhitelistManager.h"

// Configuration validation error codes
enum ConfigValidationError {
    CONFIG_VALIDATION_SUCCESS = 0,
    CONFIG_VALIDATION_MISSING_REQUIRED = 1,
    CONFIG_VALIDATION_INVALID_TYPE = 2,
    CONFIG_VALIDATION_OUT_OF_RANGE = 3,
    CONFIG_VALIDATION_INVALID_FORMAT = 4,
    CONFIG_VALIDATION_ENCRYPTION_FAILED = 5,
    CONFIG_VALIDATION_DECRYPTION_FAILED = 6
};


// Settings struct definition
struct Settings {
    struct Wifi {
        String ssid;
        String password;
    } wifi;
    struct Mqtt {
        String server;
        int port;
        String user;
        String password;
        String client_id;
        unsigned long publish_interval; // MQTT publish interval in seconds
        struct Topics {
            String temp[32];
            String sht4x; // Single topic name for SHT4x sensor
            String max31855k; // Single topic name for MAX31855K sensor
        } topics;
    } mqtt;
    struct Sensors {
        bool ds18b20_enabled;    // Global DS18B20 enable/disable
        int sensor_count;
        int one_wire_bus_count;
        int one_wire_pins[4];
        String sensor_addresses[32];
        String viable_scan_pins; // Comma-separated list of pins for full scan
        
        // DS18B20 sensor configuration
        float ds18b20_measurement_interval; // Measurement interval in seconds (default: 60.0)

        // SHT4x sensor configuration
        bool sht4x_enabled;
        int sht4x_sda_pin;
        int sht4x_scl_pin;
        uint8_t sht4x_i2c_address;
        int sht4x_precision;          // 0=low, 1=medium, 2=high
        int sht4x_heater_power;       // 0=none, 1=low (20mW), 2=medium (110mW), 3=high (200mW)
        int sht4x_heater_duration;    // 0=short (0.1s), 1=long (1s)
        int sht4x_heating_strategy;   // 0=none, 1=alternating
        float sht4x_measurement_interval; // seconds between measurements for heating strategy

        // MAX31855K thermocouple sensor configuration
        bool max31855k_enabled;
        int max31855k_do_pin;
        int max31855k_clk_pin;
        int max31855k_cs_pin;

    } sensors;
    struct Relay {
        bool enabled;
        std::vector<int> pins_low;    // Configurable pins for LOW state
        std::vector<int> pins_mid;    // Configurable pins for MID state
        std::vector<int> pins_high;   // Configurable pins for HIGH state
        unsigned long timeout_ms;    // Safety timeout between relay state changes (1000-30000ms)
        // DS18B20 safety mechanism fields
        bool safety_enabled;              // Enable/disable safety mechanism (default: false)
        int safety_sensor_number;         // Sensor number to monitor (1-based, default: 1)
        float safety_temperature_threshold; // Temperature threshold in Celsius (default: 50.0)
        unsigned long safety_duration_seconds; // Duration in seconds before safety action (default: 60)
        bool garage_relay_mode_enabled;   // Enable/disable garage relay mode (default: false)
    } relay;
    // Timing section removed - all timing settings are now in their respective components
    struct Network {
        String hostname;
        bool mdns_enabled;
    } network;
    struct Security {
        bool auth_enabled;
        String username;
        String password_hash;
        bool ip_whitelist_enabled;
        std::vector<String> ip_ranges;
    } security;
    struct DeviceInfo {
        String name;
        String manufacturer;
        String model;
    } device_info;
    struct Debug {
        bool logging_enabled;  // Global debug logging enable/disable
    } debug;
    struct Logging {
        int flush_interval_seconds;  // Interval for flushing log buffers in seconds
    } logging;
    struct NTP {
        String servers[3];
        String timezone;

        // Optional DS3231 RTC support (used as time source when WiFi/NTP are unavailable)
        struct RTC {
            bool enabled;
            int sda_pin;
            int scl_pin;
            uint8_t i2c_address;

            // If true, DS3231 stores UTC. If false, DS3231 stores local time according to ntp.timezone.
            bool store_utc;

            // Phase 2/3 behavior flags (wired in Phase 1 config + UI)
            bool set_system_time_on_boot;
            bool sync_rtc_from_ntp;
            unsigned long sync_interval_seconds;
            unsigned long max_step_seconds;
        } rtc;
    } ntp;
    struct Schedule {
        bool enabled;     // Enable/disable scheduling system
        struct ScheduleEntry {
            int day;           // 0=Monday, 1=Tuesday, ..., 6=Sunday
            String start_time; // HH:MM format - start of range
            String start_level; // "low", "mid", "high", "off" - level at start
            String end_time;   // HH:MM format - end of range
            String end_level;  // "low", "mid", "high", "off" - level at end
        };
        std::vector<ScheduleEntry> entries;
    } schedules;
    struct Fallback {
        bool enabled;
        unsigned long timeout_minutes;
    } fallback;
};

// Configuration metadata
struct ConfigMetadata {
    int version;
    String checksum;
    unsigned long timestamp;
    String description;
    bool encrypted;
};

class ConfigManager {
public:
    ConfigManager();
    ~ConfigManager();

    // Configuration management
    bool loadConfiguration();
    bool saveConfiguration();
    void setDefaultSettings();
    bool isConfigLoaded() const;

    // Enhanced configuration management
    bool validateConfiguration(const JsonDocument& doc, String& errorMessage);
    bool createBackup();
    bool rollbackToVersion(int version);
    std::vector<int> getAvailableVersions();
    bool encryptSensitiveData();
    bool decryptSensitiveData();

    // Secure credential migration system
    bool migrateCredentialsIfNeeded();
    bool isCredentialMigrationNeeded() const;
    bool performCredentialMigration();
    bool isCredentialSecured(const String& credential) const;
    String hashPassword(const String& plainPassword);
    bool verifyPassword(const String& plainPassword, const String& hashedPassword) const;
    String generateSalt();
    String encryptSecret(const String& plainSecret);
    String decryptSecret(const String& encryptedSecret);
    bool markMigrationComplete();
    bool isMigrationComplete() const;

    // Critical settings validation
    bool hasValidCriticalSettings(String& errorMessage);

    // Settings access
    Settings& getSettings();
    const Settings& getSettings() const;
    Settings getSettingsSnapshot() const;
    uint32_t getSettingsRevision() const;

    // Secure authentication helpers
    String getPlainTextAuthPassword() const;
    bool verifyAuthPassword(const String& plainPassword) const;

    // API response generation
    String getSettingsJson();
    bool updateSettingsFromJson(const String& jsonBody);

    // Configuration metadata
    ConfigMetadata getMetadata() const;
    String getValidationErrors() const;


private:
    Settings settings;
    bool configLoaded;
    ConfigMetadata metadata;
    String lastValidationErrors;
    mutable SemaphoreHandle_t settingsMutex;
    uint32_t settingsRevision;

    // Secure credential migration system
    bool migrationComplete;
    String migrationSalt;
    String securedWifiPassword;
    String securedMqttPassword;
    String securedAuthPassword;
    String plainTextAuthPassword; // Decrypted version for HTTP auth

    // Encryption key (should be derived from device-specific data in production)
    static String encryptionKey;

    void lockSettings() const;
    void unlockSettings() const;
    class SettingsLockGuard {
    public:
        explicit SettingsLockGuard(const ConfigManager& ownerRef) : owner(ownerRef) {
            owner.lockSettings();
        }
        ~SettingsLockGuard() {
            owner.unlockSettings();
        }
    private:
        const ConfigManager& owner;
    };

    // Helper methods
    void loadWifiSettings(const JsonDocument& doc, bool loadPasswords = true);
    void loadMqttSettings(const JsonDocument& doc, bool loadPasswords = true);
    void loadSensorSettings(const JsonDocument& doc);
    void loadRelaySettings(const JsonDocument& doc);
    void loadTimingSettings(const JsonDocument& doc);
    void loadNetworkSettings(const JsonDocument& doc);
    void loadSecuritySettings(const JsonDocument& doc, bool loadPasswords = true);
    void loadDeviceInfoSettings(const JsonDocument& doc);
    void loadDebugSettings(const JsonDocument& doc);
    void loadLoggingSettings(const JsonDocument& doc);
    void loadNTPSettings(const JsonDocument& doc);
    void loadScheduleSettings(const JsonDocument& doc);
    void loadFallbackSettings(const JsonDocument& doc);

    void saveWifiSettings(JsonDocument& doc);
    void saveMqttSettings(JsonDocument& doc);
    void saveSensorSettings(JsonDocument& doc);
    void saveRelaySettings(JsonDocument& doc);
    void saveTimingSettings(JsonDocument& doc);
    void saveNetworkSettings(JsonDocument& doc);
    void saveSecuritySettings(JsonDocument& doc);
    void saveDeviceInfoSettings(JsonDocument& doc);
    void saveDebugSettings(JsonDocument& doc);
    void saveLoggingSettings(JsonDocument& doc);
    void saveNTPSettings(JsonDocument& doc);
    void saveScheduleSettings(JsonDocument& doc);
    void saveFallbackSettings(JsonDocument& doc);

    // Enhanced helper methods
    bool validateWifiSettings(const JsonDocument& doc, String& error);
    bool validateMqttSettings(const JsonDocument& doc, String& error);
    bool validateSensorSettings(const JsonDocument& doc, String& error);
    bool validateTimingSettings(const JsonDocument& doc, String& error);
    bool validateSecuritySettings(const JsonDocument& doc, String& error);
    bool validateDeviceInfoSettings(const JsonDocument& doc, String& error);
    bool validateNTPSettings(const JsonDocument& doc, String& error);
    bool validateIPWhitelistSettings(const JsonDocument& doc, String& error);
    bool validateScheduleSettings(const JsonDocument& doc, String& error);
    bool validateFallbackSettings(const JsonDocument& doc, String& error);
    
    // Pin validation methods
    bool validatePinConfiguration(const JsonDocument& doc, String& errorMessage);
    bool validateSensorPins(const JsonDocument& doc, String& errorMessage);
    bool validateRelayPins(const JsonDocument& doc, String& errorMessage);

    // Helper methods
    std::vector<int> parsePinString(const String& pinString);
    bool hasDuplicates(const std::vector<int>& pins);

    String calculateChecksum(const JsonDocument& doc);
    bool saveVersionedConfig(const JsonDocument& doc, const String& description);
    bool loadVersionedConfig(int version, ArduinoJson::JsonDocument& doc);
    // Encryption methods
    String encryptString(const String& plainText);
    String decryptString(const String& encryptedText);
    String generateEncryptionKey();

    // Password update handling
    void handlePasswordUpdates(const JsonDocument& doc);
};

#endif // CONFIGMANAGER_H
