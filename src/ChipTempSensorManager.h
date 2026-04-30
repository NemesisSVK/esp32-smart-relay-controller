#ifndef CHIP_TEMP_SENSOR_MANAGER_H
#define CHIP_TEMP_SENSOR_MANAGER_H

// ESP32-S3 temperature sensor implementation using Arduino temperatureRead()
#if defined(ARDUINO_ARCH_ESP32)
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#endif

class ChipTempSensorManager {
private:
    float temperature;
    bool initialized;
#if defined(ARDUINO_ARCH_ESP32)
    mutable SemaphoreHandle_t stateMutex = nullptr;
#endif

    void lockState() const;
    void unlockState() const;
    class StateLockGuard {
    public:
        explicit StateLockGuard(const ChipTempSensorManager& ownerRef) : owner(ownerRef) {
            owner.lockState();
        }
        ~StateLockGuard() {
            owner.unlockState();
        }
    private:
        const ChipTempSensorManager& owner;
    };

public:
    ChipTempSensorManager();
    ~ChipTempSensorManager();

    // Core functionality
    bool begin();
    bool updateTemperature();
    float getTemperature();
    bool isInitialized();

    // Health monitoring
    bool isSensorOperational();
};

#endif // CHIP_TEMP_SENSOR_MANAGER_H
