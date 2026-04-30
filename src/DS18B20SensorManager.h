#ifndef DS18B20_SENSOR_MANAGER_H
#define DS18B20_SENSOR_MANAGER_H

#include <OneWire.h>
#include <DallasTemperature.h>
#include <ArduinoJson.h>
#if defined(ARDUINO_ARCH_ESP32)
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#endif

// Forward declaration to avoid circular dependency
class MainController;

class DS18B20SensorManager {
private:
    OneWire** oneWirePtrs;
    DallasTemperature** sensorsPtrs;
    DeviceAddress sensorAddresses[32];
    float temperatures[32];
    bool sensorParasiticPower[32];  // Store parasitic power status for each sensor
    bool hasValidData[32];  // Tracks if latest reading is considered valid for each sensor
    int sensorCount;
    int* oneWirePins;
    int busIndex[32];  // which bus each sensor is on
    int numBuses;
    String viableScanPins;  // Configurable list of pins for full scan

    // Timing state for pre-conversion
    unsigned long conversionStartTime = 0;
    bool conversionInProgress = false;
    unsigned long lastReadoutTime = 0;
    bool dataValid = false;

    // Readout tracking / startup handling
    unsigned long managerStartTimeMs = 0;
    unsigned long completedReadouts = 0;
    bool hasEverNonDefaultTemp[32]; // Tracks if sensor ever returned a non-85.0 temperature
    bool pendingReinitNonDefaultTemp[32]; // After a -127 failure, require one non-85.0 reading before trusting 85.0 again

    // Configuration
    unsigned long sensorReadInterval = 1000; // Configurable sensor read interval

    // Constants
    static constexpr unsigned long CONVERSION_TIME_MS = 750;
    static constexpr unsigned long SAFETY_MARGIN_MS = 25;
    static constexpr unsigned long TOTAL_WAIT_TIME_MS = CONVERSION_TIME_MS + SAFETY_MARGIN_MS;
    static constexpr unsigned long POWER_ON_85_SUPPRESS_WINDOW_MS = 120000; // 2 minutes

    // Read reliability
    static constexpr int READ_RETRIES = 2; // number of retries after the initial read
    static constexpr unsigned long READ_RETRY_DELAY_MS = 15;

    // Non-blocking scan job state (single-owner, loop-driven)
    static constexpr int MAX_SCAN_PINS = 20;
    bool scanJobRequested = false;
    bool scanJobActive = false;
    bool scanJobFullMode = false;
    bool scanResultReady = false;
    bool lastScanResultFullMode = false;
    unsigned long scanJobStartMs = 0;
    unsigned long scanConversionStartMs = 0;
    unsigned long lastScanCompletedMs = 0;
    int scanPins[MAX_SCAN_PINS];
    int scanBuses = 0;
    int scanTotalCount = 0;
    DeviceAddress scanTempAddrs[32];
    int scanSensorBus[32];
    String scanErrorMsg;
    String lastScanResultJson;

    // Performance monitoring
    MainController* mainController = nullptr;

#if defined(ARDUINO_ARCH_ESP32)
    mutable SemaphoreHandle_t stateMutex = nullptr;
#endif

    void lockState() const;
    void unlockState() const;
    class StateLockGuard {
    public:
        explicit StateLockGuard(const DS18B20SensorManager& ownerRef) : owner(ownerRef) {
            owner.lockState();
        }
        ~StateLockGuard() {
            owner.unlockState();
        }
    private:
        const DS18B20SensorManager& owner;
    };

    void parseHexAddress(String hexStr, DeviceAddress &addr);

    // Helper methods for pre-conversion timing
    void startTemperatureConversion();
    void readTemperaturesAfterConversion();

public:
    DS18B20SensorManager(const int pins[], int count, const String& viableScanPins);
    ~DS18B20SensorManager();

    // Core functionality
    void begin();
    void updateTemperatures();
    float getTemperature(int sensorIndex);
    int getSensorCount();

    // Configuration
    void setSensorAddresses(const String addresses[], int count);
    void setSensorCount(int count);

    // Diagnostics
    bool requestScan(bool fullScan);
    void processScanJob();
    bool isScanBusy() const;
    bool isScanInProgress() const;
    bool isScanQueued() const;
    bool hasScanResultReady() const;
    bool isLastScanResultFullMode() const;
    unsigned long getLastScanCompletedMs() const;
    String consumeScanResult();
    void clearScanResult();
    bool isParasitePowerMode();
    bool readPowerSupply(int sensorIndex);

    // Health monitoring
    bool isSensorOperational(int sensorIndex);

    // Parasitic power detection
    bool getParasiticPowerStatus(int sensorIndex);

    // Device enumeration (for MQTT discovery)
    bool getAddress(DeviceAddress &addr, int index);
    String getSensorAddressHex(int sensorIndex);

    // Data validity checking
    bool isDataValid() const;
    bool hasCompletedAtLeastOneReadout() const;
    unsigned long getCompletedReadoutCount() const;

    // Returns true when the current temperature is likely still the DS18B20 power-on default (85C)
    // and should be suppressed from publishing until sensor state stabilizes.
    // This includes reconnect recovery after read failures/disconnects.
    bool isLikelyPowerOnDefault85C(int sensorIndex) const;

    // Sensor status


    // Configuration
    void setSensorReadInterval(unsigned long interval);

    // Performance monitoring
    void setMainController(MainController* controller) { mainController = controller; }
};

#endif
