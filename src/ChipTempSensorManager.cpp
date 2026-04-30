#include "ChipTempSensorManager.h"
#include <Arduino.h>
#include "UnifiedLogger.h"

ChipTempSensorManager::ChipTempSensorManager()
    : temperature(NAN),
      initialized(false) {
#if defined(ARDUINO_ARCH_ESP32)
    stateMutex = xSemaphoreCreateRecursiveMutex();
#endif
}

ChipTempSensorManager::~ChipTempSensorManager() {
    // ESPCPUTemp library handles cleanup automatically
#if defined(ARDUINO_ARCH_ESP32)
    if (stateMutex) {
        vSemaphoreDelete(stateMutex);
        stateMutex = nullptr;
    }
#endif
}

void ChipTempSensorManager::lockState() const {
#if defined(ARDUINO_ARCH_ESP32)
    if (stateMutex) {
        xSemaphoreTakeRecursive(stateMutex, portMAX_DELAY);
    }
#endif
}

void ChipTempSensorManager::unlockState() const {
#if defined(ARDUINO_ARCH_ESP32)
    if (stateMutex) {
        xSemaphoreGiveRecursive(stateMutex);
    }
#endif
}

bool ChipTempSensorManager::begin() {
    StateLockGuard guard(*this);
    // ESPCPUTemp library doesn't require explicit initialization
    initialized = true;
    UnifiedLogger::info("[CHIPTEMP] Sensor initialized successfully\n");
    return true;
}

bool ChipTempSensorManager::updateTemperature() {
    StateLockGuard guard(*this);
    if (!initialized) {
        return false;
    }

    float temp = temperatureRead();  // Arduino built-in function

    // temperatureRead() returns temperature in Celsius
    // Valid range for ESP32 chip temperature is typically 20-80C
    if (!isnan(temp) && temp > -50.0f && temp < 150.0f) {  // Reasonable temperature range
        temperature = round(temp * 10) / 10.0f;
        return true;
    }

    temperature = NAN;
    return false;
}

float ChipTempSensorManager::getTemperature() {
    StateLockGuard guard(*this);
    return temperature;
}

bool ChipTempSensorManager::isInitialized() {
    StateLockGuard guard(*this);
    return initialized;
}

bool ChipTempSensorManager::isSensorOperational() {
    StateLockGuard guard(*this);
    // Sensor is operational if it has valid data
    return !isnan(temperature);
}

