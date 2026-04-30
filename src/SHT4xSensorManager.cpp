#include "SHT4xSensorManager.h"
#include "ErrorHandler.h"
#include "MainController.h"
#include "UnifiedLogger.h"

SHT4xSensorManager::SHT4xSensorManager(int sdaPin, int sclPin, uint8_t address) :
    mainController(nullptr),
    sht4xSensor(nullptr), temperature(NAN), humidity(NAN),
      sensorInitialized(false),
      i2cAddress(address), sdaPin(sdaPin), sclPin(sclPin), i2cBus(0),
      heatingStrategy(HEATING_STRATEGY_NONE), measurementInterval(60.0),
      heaterPower(0), heaterDuration(0),
      lastMeasurementTime(0), heatingScheduledTime(0), heatingActive(false),
      heatingStartTime(0), heatingWaitDurationMs(0), alternatingShouldHeat(false),
      hasNewMeasurementFlag(false), lastPublishedTimestamp(0) {
#if defined(ARDUINO_ARCH_ESP32)
    stateMutex = xSemaphoreCreateRecursiveMutex();
#endif
    // Initialize current measurement with default values
    currentMeasurement.temperature = NAN;
    currentMeasurement.humidity = NAN;
    currentMeasurement.quality = QUALITY_STALE;
    currentMeasurement.timestamp = 0;
    currentMeasurement.heaterWasActive = false;
}

SHT4xSensorManager::~SHT4xSensorManager() {
    if (sht4xSensor) {
        delete sht4xSensor;
        sht4xSensor = nullptr;
    }
#if defined(ARDUINO_ARCH_ESP32)
    if (stateMutex) {
        vSemaphoreDelete(stateMutex);
        stateMutex = nullptr;
    }
#endif
}

void SHT4xSensorManager::lockState() const {
#if defined(ARDUINO_ARCH_ESP32)
    if (stateMutex) {
        xSemaphoreTakeRecursive(stateMutex, portMAX_DELAY);
    }
#endif
}

void SHT4xSensorManager::unlockState() const {
#if defined(ARDUINO_ARCH_ESP32)
    if (stateMutex) {
        xSemaphoreGiveRecursive(stateMutex);
    }
#endif
}

bool SHT4xSensorManager::begin() {
    StateLockGuard guard(*this);
    // Initialize I2C bus
    i2cBus.begin(sdaPin, sclPin);
    
    // Create sensor instance
    sht4xSensor = new Adafruit_SHT4x();
    
    // Initialize sensor with default high precision and no heater
    // Note: SHT4x always uses I2C address 0x44, so we don't pass address to begin()
    if (!sht4xSensor->begin(&i2cBus)) {
        UnifiedLogger::error("[SHT4X] Failed to initialize sensor\n");
        ErrorHandler::reportError(ErrorCode::SHT4X_INITIALIZATION_FAILED, ErrorSeverity::ERROR,
                                "SHT4x sensor initialization failed on I2C bus (SDA: " + String(sdaPin) + ", SCL: " + String(sclPin) + ")");
        delete sht4xSensor;
        sht4xSensor = nullptr;
        return false;
    }
    
    // Configure sensor settings
    sht4xSensor->setPrecision(SHT4X_HIGH_PRECISION);
    sht4xSensor->setHeater(SHT4X_NO_HEATER);
    
    sensorInitialized = true;
    
    // Log sensor information
    uint32_t serial = sht4xSensor->readSerial();
    UnifiedLogger::info("[SHT4X] Initialized, address=0x%02X, serial=0x%08lX\n",
                        i2cAddress,
                        static_cast<unsigned long>(serial));
    
    return true;
}

void SHT4xSensorManager::updateReadings() {
    StateLockGuard guard(*this);
    if (!sensorInitialized || !sht4xSensor) {
        return;
    }

    // Start timing for actual sensor reading operation
    unsigned long startTime = millis();

    sensors_event_t humidityEvent, tempEvent;
    bool success = sht4xSensor->getEvent(&humidityEvent, &tempEvent);

    // End timing and report to performance monitoring
    unsigned long endTime = millis();
    if (mainController) {
        unsigned long duration = endTime - startTime;
        mainController->updateComponentStatsPublic(1, duration); // SHT4X_INDEX = 1
    }

    if (success) {
        temperature = round(tempEvent.temperature * 10) / 10.0;
        humidity = round(humidityEvent.relative_humidity * 10) / 10.0;
        
        
        // When heating is disabled (heaterPower = 0 and heatingStrategy = 0),
        // we're following global timing and should set the new measurement flag
        if (heaterPower == 0 && heatingStrategy == HEATING_STRATEGY_NONE) {
            // Mark this measurement as valid
            currentMeasurement.temperature = temperature;
            currentMeasurement.humidity = humidity;
            currentMeasurement.quality = QUALITY_VALID;
            currentMeasurement.timestamp = millis();
            currentMeasurement.heaterWasActive = false;
            
            // Set the new measurement flag
            hasNewMeasurementFlag = true;
            
        } else if (heaterPower > 0 && heatingStrategy == HEATING_STRATEGY_NONE) {
            // Heating is enabled but no heating strategy is configured
            // This means we should take measurements on global timing and publish them
            currentMeasurement.temperature = temperature;
            currentMeasurement.humidity = humidity;
            currentMeasurement.quality = QUALITY_VALID;
            currentMeasurement.timestamp = millis();
            currentMeasurement.heaterWasActive = false;
            
            // Set the new measurement flag
            hasNewMeasurementFlag = true;
            
        }
    } else {
        temperature = NAN;
        humidity = NAN;

        // Report failure as warning
        ErrorHandler::reportError(ErrorCode::SHT4X_READ_FAILED, ErrorSeverity::WARNING,
                                "SHT4x sensor read failure - no valid data received");
    }
}

void SHT4xSensorManager::updateReadingsWithHeatingStrategy() {
    StateLockGuard guard(*this);
    if (!sensorInitialized || !sht4xSensor) {
        return;
    }

    // Check if heating is actually configured (power > 0)
    // Note: heaterDuration is only meaningful when heaterPower > 0
    // heaterPower = 0 means heating is disabled, any other value means heating is enabled
    bool heatingActuallyConfigured = (heaterPower > 0);

    if (heatingStrategy > HEATING_STRATEGY_NONE && !heatingActuallyConfigured) {
        // Heating strategy is set but heating is disabled (power = 0) - use normal readings with global timing
        updateReadings();
        return;
    }

    unsigned long now = millis();

    switch (heatingStrategy) {
        case HEATING_STRATEGY_NONE:
            // Normal operation - just take measurement
            updateReadings();
            break;
              
        case HEATING_STRATEGY_ALTERNATING:
            handleAlternatingHeatingStrategy(now);
            break;
               
        default:
            UnifiedLogger::warning("[SHT4X] Unknown heating strategy: %d\n", heatingStrategy);
            updateReadings();
            break;
   }
}

void SHT4xSensorManager::handleAlternatingHeatingStrategy(unsigned long now) {
    StateLockGuard guard(*this);
    if (heatingActive) {
        // Continue an in-progress heated cycle without blocking loop().
        performHeatedMeasurement();
        return;
    }
    
    // Check if it's time for a new measurement
    if (lastMeasurementTime == 0 || now - lastMeasurementTime >= measurementInterval * 1000) {
        alternatingShouldHeat = !alternatingShouldHeat; // Toggle heating state
        lastMeasurementTime = now;
        
        if (alternatingShouldHeat) {
            performHeatedMeasurement();
        } else {
            performNonHeatedMeasurement();
        }
    }
}


void SHT4xSensorManager::performHeatedMeasurement() {
    StateLockGuard guard(*this);
    Adafruit_SHT4x* sensor = getSensor();

    // Check if heating is actually enabled (power > 0)
    // Note: heaterDuration is only meaningful when heaterPower > 0
    // heaterPower = 0 means heating is disabled, any other value means heating is enabled
    if (heaterPower <= 0) {
        if (heatingActive) {
            // Heater settings changed mid-cycle; cleanly stop the in-progress cycle.
            if (sensor) {
                sensor->setHeater(SHT4X_NO_HEATER);
            }
            heatingActive = false;
            heatingStartTime = 0;
            heatingScheduledTime = 0;
            heatingWaitDurationMs = 0;
        }

        // Heating is disabled, just take normal measurement
        performNonHeatedMeasurement();
        return;
    }
    
    // Activate heater for this measurement
    if (sensor) {
        // Use configured heater power and duration
        // Map heaterPower (0-3) directly to SHT4x heater levels
        sht4x_heater_t heaterLevel = SHT4X_NO_HEATER;
        int delayMs = 0;
        
        switch (heaterPower) {
            case 1:
                heaterLevel = SHT4X_LOW_HEATER_100MS;
                delayMs = 100;
                break;
            case 2:
                heaterLevel = SHT4X_MED_HEATER_100MS;
                delayMs = 100;
                break;
            case 3:
                heaterLevel = SHT4X_HIGH_HEATER_100MS;
                delayMs = 100;
                break;
            default:
                // 0 or any other value = no heating
                heaterLevel = SHT4X_NO_HEATER;
                delayMs = 0;
                break;
        }
        
        if (!heatingActive) {
            // Start non-blocking heated cycle.
            int actualDelayMs = heaterDuration * 1000;
            heatingWaitDurationMs = static_cast<unsigned long>(max(delayMs, actualDelayMs));
            heatingStartTime = millis();
            heatingScheduledTime = heatingStartTime + heatingWaitDurationMs;
            heatingActive = true;

            sensor->setHeater(heaterLevel);

            return;
        }

        // Non-blocking wait: only complete once heater wait window has elapsed.
        if (millis() - heatingStartTime < heatingWaitDurationMs) {
            return;
        }

        // Take measurement with heating active.
        updateReadings();
        
        // Mark this measurement as heated.
        currentMeasurement.temperature = temperature;
        currentMeasurement.humidity = humidity;
        currentMeasurement.quality = QUALITY_HEATED;
        currentMeasurement.timestamp = millis();
        currentMeasurement.heaterWasActive = true;
        
        // Reset heater and state machine.
        sensor->setHeater(SHT4X_NO_HEATER);
        heatingActive = false;
        heatingScheduledTime = 0;
        heatingWaitDurationMs = 0;
        
        logMeasurementQuality();
    } else if (heatingActive) {
        // Sensor disappeared during an in-progress cycle; clear pending state.
        heatingActive = false;
        heatingStartTime = 0;
        heatingScheduledTime = 0;
        heatingWaitDurationMs = 0;
    }
}

void SHT4xSensorManager::performNonHeatedMeasurement() {
    StateLockGuard guard(*this);
    // Take normal measurement without heating
    updateReadings();
    
    // Mark this measurement as valid
    currentMeasurement.temperature = temperature;
    currentMeasurement.humidity = humidity;
    currentMeasurement.quality = QUALITY_VALID;
    currentMeasurement.timestamp = millis();
    currentMeasurement.heaterWasActive = false;
    
    // Set the new measurement flag
    hasNewMeasurementFlag = true;
    
    logMeasurementQuality();
}



// Getter methods
float SHT4xSensorManager::getTemperature() {
    StateLockGuard guard(*this);
    return temperature;
}

float SHT4xSensorManager::getHumidity() {
    StateLockGuard guard(*this);
    return humidity;
}

bool SHT4xSensorManager::isSensorOperational() {
    StateLockGuard guard(*this);
    // Sensor is operational if it has valid data
    return !isnan(temperature) && !isnan(humidity);
}

bool SHT4xSensorManager::isInitialized() {
    StateLockGuard guard(*this);
    return sensorInitialized;
}

// Heating strategy methods
void SHT4xSensorManager::setHeatingStrategy(int strategy, float intervalSeconds) {
    StateLockGuard guard(*this);
    if (strategy < HEATING_STRATEGY_NONE || strategy > HEATING_STRATEGY_ALTERNATING) {
        UnifiedLogger::warning("[SHT4X] Invalid heating strategy specified: %d\n", strategy);
        return;
    }
     
    heatingStrategy = strategy;
    measurementInterval = intervalSeconds;

    UnifiedLogger::info("[SHT4X] Heating strategy set: strategy=%d interval=%.1fs\n", strategy, intervalSeconds);
     
    // Reset timing variables
    lastMeasurementTime = 0;
    heatingScheduledTime = 0;
    heatingActive = false;
    heatingStartTime = 0;
    heatingWaitDurationMs = 0;
    alternatingShouldHeat = false;

    if (sensorInitialized && sht4xSensor) {
        // Ensure no heater state leaks across strategy changes.
        sht4xSensor->setHeater(SHT4X_NO_HEATER);
    }
}

void SHT4xSensorManager::setHeaterConfiguration(int power, int duration) {
    StateLockGuard guard(*this);
    heaterPower = power;
    heaterDuration = duration;

    UnifiedLogger::info("[SHT4X] Heater configuration set: power=%d duration=%ds\n", power, duration);
}


void SHT4xSensorManager::logMeasurementQuality() const {
    StateLockGuard guard(*this);
    (void)currentMeasurement;
}

bool SHT4xSensorManager::hasNewMeasurement() const {
    StateLockGuard guard(*this);
    // Check if we have a new measurement that hasn't been published yet
    // A measurement is considered "new" if:
    // 1. It's valid (not heated, not stale, not error)
    // 2. It's newer than the last published measurement
    // 3. The new measurement flag is set
    
    if (!hasNewMeasurementFlag) {
        return false;
    }
    
    // Check if this is a valid measurement (not heated)
    if (currentMeasurement.quality != QUALITY_VALID) {
        return false;
    }
    
    // Check if this measurement is newer than the last published one
    if (currentMeasurement.timestamp <= lastPublishedTimestamp) {
        return false;
    }
    
    return true;
}

void SHT4xSensorManager::clearNewMeasurementFlag() {
    StateLockGuard guard(*this);
    // Clear the new measurement flag and update the last published timestamp
    hasNewMeasurementFlag = false;
    lastPublishedTimestamp = currentMeasurement.timestamp;
}


uint32_t SHT4xSensorManager::getSensorSerialNumber() {
    StateLockGuard guard(*this);
    if (sensorInitialized && sht4xSensor) {
        return sht4xSensor->readSerial();
    }
    return 0;
}

// Configuration methods

Adafruit_SHT4x* SHT4xSensorManager::getSensor() {
    StateLockGuard guard(*this);
    return sht4xSensor;
}

void SHT4xSensorManager::setMainController(MainController* controller) {
    StateLockGuard guard(*this);
    mainController = controller;
}
