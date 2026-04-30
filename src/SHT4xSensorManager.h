#ifndef SHT4X_SENSOR_MANAGER_H
#define SHT4X_SENSOR_MANAGER_H

#include <Wire.h>
#include <Adafruit_SHT4x.h>
#if defined(ARDUINO_ARCH_ESP32)
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#endif

// Forward declaration to avoid circular dependency
class MainController;

class SHT4xSensorManager {
private:
    MainController* mainController;
    Adafruit_SHT4x* sht4xSensor;
    float temperature;
    float humidity;
    bool sensorInitialized;
    
    // Default configuration
    static const uint8_t DEFAULT_I2C_ADDRESS = 0x44;
    static const int DEFAULT_SDA_PIN = 8;
    static const int DEFAULT_SCL_PIN = 9;

    
    // Current configuration
    uint8_t i2cAddress;
    int sdaPin;
    int sclPin;
    TwoWire i2cBus;
    
    // Heating strategy variables
    int heatingStrategy; // 0=none, 1=alternating
    float measurementInterval; // seconds between measurements
    int heaterPower; // 0-9, 0=disabled
    int heaterDuration; // seconds, 0=disabled
    unsigned long lastMeasurementTime;
    unsigned long heatingScheduledTime;
    bool heatingActive;
    unsigned long heatingStartTime;
    unsigned long heatingWaitDurationMs;
    bool alternatingShouldHeat;
    
    // Measurement tracking
    bool hasNewMeasurementFlag;
    unsigned long lastPublishedTimestamp;
    
    

public:
    // Heating strategy constants
    static const int HEATING_STRATEGY_NONE = 0;
    static const int HEATING_STRATEGY_ALTERNATING = 1;
    
    // Data quality tracking (public for external monitoring)
    enum MeasurementQuality {
        QUALITY_VALID = 0,    // Normal measurement, no heating
        QUALITY_HEATED = 1,   // Heating was active during measurement
        QUALITY_STALE = 2,    // Data is old/expired
        QUALITY_ERROR = 3     // Sensor error occurred
    };
    
    struct SHT4xMeasurement {
        float temperature;
        float humidity;
        MeasurementQuality quality;
        unsigned long timestamp;
        bool heaterWasActive;
    };

    // Constructor with default pins
    explicit SHT4xSensorManager(int sdaPin = DEFAULT_SDA_PIN,
                               int sclPin = DEFAULT_SCL_PIN,
                               uint8_t address = DEFAULT_I2C_ADDRESS);
    
    // Destructor
    ~SHT4xSensorManager();
    
    // Core functionality
    bool begin();
    void updateReadings();
    float getTemperature();
    float getHumidity();
    bool isSensorOperational();
    
    
    // Sensor information
    bool isInitialized();
    uint32_t getSensorSerialNumber();
    
    // Heating strategy methods
    void setHeatingStrategy(int strategy, float intervalSeconds = 60.0);
    void setHeaterConfiguration(int power, int duration);
    
    // Private heating strategy methods (used internally)
    void updateReadingsWithHeatingStrategy();
    void handleAlternatingHeatingStrategy(unsigned long now);
    void performHeatedMeasurement();
    void performNonHeatedMeasurement();
    
    // Access to sensor object for advanced configuration
    Adafruit_SHT4x* getSensor();

    // Performance monitoring integration
    void setMainController(MainController* controller);

    // Debugging and monitoring
    bool hasNewMeasurement() const;
    void clearNewMeasurementFlag();
    void logMeasurementQuality() const;

private:
    // Current measurement tracking (must be after public SHT4xMeasurement definition)
    SHT4xMeasurement currentMeasurement;

#if defined(ARDUINO_ARCH_ESP32)
    mutable SemaphoreHandle_t stateMutex = nullptr;
#endif

    void lockState() const;
    void unlockState() const;
    class StateLockGuard {
    public:
        explicit StateLockGuard(const SHT4xSensorManager& ownerRef) : owner(ownerRef) {
            owner.lockState();
        }
        ~StateLockGuard() {
            owner.unlockState();
        }
    private:
        const SHT4xSensorManager& owner;
    };
};

#endif
