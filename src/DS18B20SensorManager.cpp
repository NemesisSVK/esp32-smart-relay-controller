#include "DS18B20SensorManager.h"
#include "ErrorHandler.h"
#include "MainController.h"
#include "UnifiedLogger.h"
#include <math.h>

// Function to parse hex string to DeviceAddress
void DS18B20SensorManager::parseHexAddress(String hexStr, DeviceAddress &addr) {
    if (hexStr.length() != 16) {
        memset(addr, 0, 8);
        return;
    }
    for (int i = 0; i < 8; i++) {
        String byteStr = hexStr.substring(i * 2, i * 2 + 2);
        addr[i] = (uint8_t)strtol(byteStr.c_str(), NULL, 16);
    }
}

DS18B20SensorManager::DS18B20SensorManager(const int pins[], int count, const String& viableScanPins) : sensorCount(0), numBuses(min(count, 4)), viableScanPins(viableScanPins) {
    // Allocate arrays dynamically based on actual bus count
    oneWirePins = new int[numBuses];
    oneWirePtrs = new OneWire*[numBuses];
    sensorsPtrs = new DallasTemperature*[numBuses];
    
    for (int i = 0; i < numBuses; i++) {
        oneWirePins[i] = pins[i];
        oneWirePtrs[i] = nullptr;
        sensorsPtrs[i] = nullptr;
    }
    memset(temperatures, 0, sizeof(temperatures));
    memset(busIndex, 0, sizeof(busIndex));
    memset(sensorParasiticPower, false, sizeof(sensorParasiticPower));
    memset(hasValidData, false, sizeof(hasValidData));
    memset(hasEverNonDefaultTemp, false, sizeof(hasEverNonDefaultTemp));
    memset(pendingReinitNonDefaultTemp, false, sizeof(pendingReinitNonDefaultTemp));
    memset(scanPins, 0, sizeof(scanPins));
    memset(scanSensorBus, 0, sizeof(scanSensorBus));
    memset(scanTempAddrs, 0, sizeof(scanTempAddrs));
#if defined(ARDUINO_ARCH_ESP32)
    stateMutex = xSemaphoreCreateRecursiveMutex();
#endif
}

DS18B20SensorManager::~DS18B20SensorManager() {
    for (int i = 0; i < numBuses; i++) {
        if (sensorsPtrs[i]) delete sensorsPtrs[i];
        if (oneWirePtrs[i]) delete oneWirePtrs[i];
    }
    delete[] oneWirePins;
    delete[] oneWirePtrs;
    delete[] sensorsPtrs;
#if defined(ARDUINO_ARCH_ESP32)
    if (stateMutex) {
        vSemaphoreDelete(stateMutex);
        stateMutex = nullptr;
    }
#endif
}

void DS18B20SensorManager::lockState() const {
#if defined(ARDUINO_ARCH_ESP32)
    if (stateMutex) {
        xSemaphoreTakeRecursive(stateMutex, portMAX_DELAY);
    }
#endif
}

void DS18B20SensorManager::unlockState() const {
#if defined(ARDUINO_ARCH_ESP32)
    if (stateMutex) {
        xSemaphoreGiveRecursive(stateMutex);
    }
#endif
}

void DS18B20SensorManager::begin() {
    StateLockGuard guard(*this);
    if (managerStartTimeMs == 0) {
        managerStartTimeMs = millis();
    }
    for (int i = 0; i < numBuses; i++) {
        if (!oneWirePtrs[i]) {
            oneWirePtrs[i] = new OneWire(oneWirePins[i]);
            sensorsPtrs[i] = new DallasTemperature(oneWirePtrs[i]);
        }
        sensorsPtrs[i]->begin();
        // Make DS18B20 conversions asynchronous. Otherwise requestTemperatures() can block
        // for the full conversion time (~750ms at 12-bit), defeating the pre-conversion design.
        sensorsPtrs[i]->setWaitForConversion(false);
    }
}

void DS18B20SensorManager::updateTemperatures() {
    StateLockGuard guard(*this);
    if (scanJobRequested || scanJobActive) {
        // Pause regular DS18B20 loop activity while scan jobs are queued/running.
        return;
    }

    unsigned long currentTime = millis();

    // Handle initial call - start with pre-conversion immediately
    if (lastReadoutTime == 0) {
        startTemperatureConversion();
        lastReadoutTime = currentTime - (sensorReadInterval - TOTAL_WAIT_TIME_MS);
        return;
    }

    unsigned long timeSinceLastReadout = currentTime - lastReadoutTime;

    // Phase 1: Pre-start conversion 775ms before readout
    if (!conversionInProgress &&
        timeSinceLastReadout >= (sensorReadInterval - TOTAL_WAIT_TIME_MS)) {
        startTemperatureConversion();
    }

    // Phase 2: Read results at exact readout interval
    if (timeSinceLastReadout >= sensorReadInterval) {
        if (conversionInProgress &&
            currentTime - conversionStartTime >= TOTAL_WAIT_TIME_MS) {
            // Conversion should be complete
            readTemperaturesAfterConversion();
            conversionInProgress = false;
            dataValid = true;
            lastReadoutTime = currentTime;
        } else if (!conversionInProgress) {
            // Pre-conversion was missed - skip this cycle and try again
            UnifiedLogger::warning("[DS18B20] Pre-conversion missed, skipping this readout cycle\n");
            // Reset timing to try again in next cycle
            lastReadoutTime = currentTime - (sensorReadInterval - TOTAL_WAIT_TIME_MS);
        } else {
            // Conversion still in progress (shouldn't happen with proper timing)
            // Removed serial spam - conversion timing is monitored internally
        }
    }
}

void DS18B20SensorManager::startTemperatureConversion() {
    StateLockGuard guard(*this);
    unsigned long startTime = millis();

    // Start conversion timing at the moment we initiate conversion.
    // With setWaitForConversion(false), requestTemperatures() returns quickly.
    conversionStartTime = millis();

    // Initiate conversion on all buses
    for (int b = 0; b < numBuses; b++) {
        sensorsPtrs[b]->requestTemperatures();
    }
    conversionInProgress = true;
    dataValid = false;
    unsigned long endTime = millis();

    // Report timing for actual sensor operation
    if (mainController) {
        unsigned long duration = endTime - startTime;
        mainController->updateComponentStatsPublic(DS18B20_INDEX, duration);
    }
}

void DS18B20SensorManager::readTemperaturesAfterConversion() {
    StateLockGuard guard(*this);
    unsigned long startTime = millis();
    // Read temperatures immediately (conversion should be complete)
    for (int i = 0; i < sensorCount; i++) {
        int bus = busIndex[i];
        float temp = -127.0;
        if (bus >= 0 && bus < numBuses) {
            temp = sensorsPtrs[bus]->getTempC(sensorAddresses[i]);

            if (temp == -127.0f) {
                // Retry a couple times; transient bus errors can produce occasional -127 reads.
                for (int attempt = 0; attempt < READ_RETRIES; attempt++) {
                    delay(READ_RETRY_DELAY_MS);
                    temp = sensorsPtrs[bus]->getTempC(sensorAddresses[i]);
                    if (temp != -127.0f) {
                        break;
                    }
                }

                // Basic error reporting (after retries)
                if (temp == -127.0f) {
                    ErrorHandler::reportError(ErrorCode::DS18B20_READ_FAILED, ErrorSeverity::WARNING,
                                              "DS18B20 sensor " + String(i + 1) + " read failure (temperature = -127C)");
                }
            }
        }
        if (temp != -127.0) temp = round(temp * 10) / 10.0;
        temperatures[i] = temp;
        const bool isDefault85C = (temp != -127.0f) && (fabsf(temp - 85.0f) <= 0.05f);
        const bool suppressAsReconnectDefault = pendingReinitNonDefaultTemp[i] && isDefault85C;
        const bool suppressAsPostInitDefault = hasEverNonDefaultTemp[i] && isDefault85C;

        if (temp == -127.0f) {
            hasValidData[i] = false;
            // After read failures/disconnects, DS18 can briefly output default 85C.
            // Require one non-default value before accepting 85C again.
            pendingReinitNonDefaultTemp[i] = true;
        } else if (suppressAsReconnectDefault || suppressAsPostInitDefault) {
            hasValidData[i] = false;
        } else {
            hasValidData[i] = true;
        }

        // Track whether we've ever seen a non-default value for this sensor.
        // DS18B20 power-on default is 85.0C; treat it as "default" early after boot.
        if (temp != -127.0 && fabsf(temp - 85.0f) > 0.05f) {
            hasEverNonDefaultTemp[i] = true;
            pendingReinitNonDefaultTemp[i] = false;
        }
    }
    completedReadouts++;
    unsigned long endTime = millis();

    // Report timing for actual sensor operation
    if (mainController) {
        unsigned long duration = endTime - startTime;
        mainController->updateComponentStatsPublic(DS18B20_INDEX, duration);
    }
}

bool DS18B20SensorManager::isLikelyPowerOnDefault85C(int sensorIndex) const {
    StateLockGuard guard(*this);
    if (sensorIndex < 0 || sensorIndex >= sensorCount) {
        return false;
    }

    // If we haven't completed any readout yet, caller should treat data as not-ready.
    if (completedReadouts == 0) {
        return false;
    }

    const float temp = temperatures[sensorIndex];
    if (temp == -127.0f) {
        return false;
    }

    // After a valid non-default history, treat exact 85C as suspicious default/not-ready.
    // This catches partial disconnects where DS18 can float to 85C while still responding.
    if (fabsf(temp - 85.0f) <= 0.05f && hasEverNonDefaultTemp[sensorIndex] && !hasValidData[sensorIndex]) {
        return true;
    }

    if (fabsf(temp - 85.0f) <= 0.05f && pendingReinitNonDefaultTemp[sensorIndex]) {
        return true;
    }

    if (fabsf(temp - 85.0f) <= 0.05f && !hasEverNonDefaultTemp[sensorIndex]) {
        if (managerStartTimeMs == 0) {
            return true;
        }
        const unsigned long now = millis();
        return (now - managerStartTimeMs) < POWER_ON_85_SUPPRESS_WINDOW_MS;
    }

    return false;
}

// Removed blocking fallback - now using non-blocking approach only

float DS18B20SensorManager::getTemperature(int sensorIndex) {
    StateLockGuard guard(*this);
    if (sensorIndex >= 0 && sensorIndex < sensorCount) {
        return temperatures[sensorIndex];
    }
    return -127.0; // Invalid index
}

int DS18B20SensorManager::getSensorCount() {
    StateLockGuard guard(*this);
    return sensorCount;
}

void DS18B20SensorManager::setSensorAddresses(const String addresses[], int count) {
    StateLockGuard guard(*this);
    UnifiedLogger::info("[DS18B20] Starting address assignment for configured buses\n");
    sensorCount = count;
    if (sensorCount < 0) sensorCount = 0;
    if (sensorCount > 32) sensorCount = 32;
    for (int i = 0; i < sensorCount; i++) {
        parseHexAddress(addresses[i], sensorAddresses[i]);
        // Find which bus this address is on by scanning each bus
        busIndex[i] = -1;
        char hexAddr[17]; // 16 chars + null
        for (int j = 0; j < 8; j++) {
            sprintf(&hexAddr[j*2], "%02X", sensorAddresses[i][j]);
        }
        hexAddr[16] = '\0';
        for (int b = 0; b < numBuses; b++) {
            // Only scan buses that are actually configured (have valid pin numbers > 0)
            if (oneWirePins[b] <= 0) {
                continue;
            }
            OneWire tempOneWire(oneWirePins[b]);
            DallasTemperature tempSensors(&tempOneWire);
            tempSensors.begin();
            int deviceCount = tempSensors.getDeviceCount();
            for (int d = 0; d < deviceCount; d++) {
                DeviceAddress addr;
                if (tempSensors.getAddress(addr, d)) {
                    char foundHex[17];
                    for (int j = 0; j < 8; j++) {
                        sprintf(&foundHex[j*2], "%02X", addr[j]);
                    }
                    foundHex[16] = '\0';
                    bool match = true;
                    for (int j = 0; j < 8; j++) {
                        if (addr[j] != sensorAddresses[i][j]) {
                            match = false;
                            break;
                        }
                    }
                    if (match) {
                        busIndex[i] = b;
                        break;
                    }
                }
            }
            if (busIndex[i] != -1) break;
        }
        if (busIndex[i] == -1) {
            UnifiedLogger::warning("[DS18B20] Sensor %d address not found on any configured bus\n", i + 1);
        }
    }
    
    // After assigning sensor addresses, detect power mode
    for (int i = 0; i < sensorCount; i++) {
        if (busIndex[i] >= 0 && busIndex[i] < numBuses) {
            sensorParasiticPower[i] = readPowerSupply(i);
        }
    }
    
    UnifiedLogger::info("[DS18B20] Address assignment complete for %d sensors\n", sensorCount);
}

void DS18B20SensorManager::setSensorCount(int count) {
    StateLockGuard guard(*this);
    sensorCount = count;
    if (sensorCount < 0) sensorCount = 0;
    if (sensorCount > 32) sensorCount = 32;
}

bool DS18B20SensorManager::requestScan(bool fullScan) {
    StateLockGuard guard(*this);
    if (scanJobRequested || scanJobActive) {
        return false;
    }

    scanJobRequested = true;
    scanJobFullMode = fullScan;
    return true;
}

void DS18B20SensorManager::processScanJob() {
    StateLockGuard guard(*this);
    if (!scanJobRequested && !scanJobActive) {
        return;
    }

    // Let an in-flight regular conversion finish before taking ownership for scan.
    if (!scanJobActive && conversionInProgress) {
        const unsigned long now = millis();
        if (now - conversionStartTime >= TOTAL_WAIT_TIME_MS) {
            readTemperaturesAfterConversion();
            conversionInProgress = false;
            dataValid = true;
            lastReadoutTime = now;
        } else {
            return;
        }
    }

    if (!scanJobActive && scanJobRequested) {
        scanJobActive = true;
        scanJobRequested = false;
        scanJobStartMs = millis();
        scanConversionStartMs = 0;
        scanTotalCount = 0;
        scanBuses = 0;
        scanErrorMsg = "";
        memset(scanPins, 0, sizeof(scanPins));
        memset(scanSensorBus, 0, sizeof(scanSensorBus));
        memset(scanTempAddrs, 0, sizeof(scanTempAddrs));

        if (scanJobFullMode) {
            // Parse configurable viable scan pins from comma-separated string.
            int pinCount = 0;
            int startIdx = 0;
            for (int i = 0; i < viableScanPins.length() && pinCount < MAX_SCAN_PINS; i++) {
                if (viableScanPins.charAt(i) == ',') {
                    if (i > startIdx) {
                        String pinStr = viableScanPins.substring(startIdx, i);
                        scanPins[pinCount++] = pinStr.toInt();
                    }
                    startIdx = i + 1;
                }
            }
            if (startIdx < viableScanPins.length() && pinCount < MAX_SCAN_PINS) {
                String pinStr = viableScanPins.substring(startIdx);
                scanPins[pinCount++] = pinStr.toInt();
            }
            scanBuses = pinCount;
        } else {
            // For configured scan, only include valid configured pins.
            for (int i = 0; i < numBuses && scanBuses < MAX_SCAN_PINS; i++) {
                if (oneWirePins[i] > 0) {
                    scanPins[scanBuses++] = oneWirePins[i];
                }
            }
        }

        UnifiedLogger::info("[SCAN] Job started (%s), buses=%d\n",
                            scanJobFullMode ? "full" : "configured",
                            scanBuses);

        // Enumerate sensors on requested pins.
        for (int b = 0; b < scanBuses; b++) {
            if (scanPins[b] <= 0) {
                continue;
            }

            OneWire tempOneWire(scanPins[b]);
            DallasTemperature tempSensors(&tempOneWire);
            tempSensors.begin();
            const int deviceCount = tempSensors.getDeviceCount();

            for (int i = 0; i < deviceCount && scanTotalCount < 32; i++) {
                DeviceAddress addr;
                if (tempSensors.getAddress(addr, i)) {
                    memcpy(scanTempAddrs[scanTotalCount], addr, 8);
                    scanSensorBus[scanTotalCount] = b;
                    scanTotalCount++;
                } else {
                    char errorBuf[64];
                    sprintf(errorBuf, "Failed to get address for sensor %d on pin %d; ", i, scanPins[b]);
                    scanErrorMsg += errorBuf;
                }
            }
        }

        // Request non-blocking conversion on all valid pins.
        for (int b = 0; b < scanBuses; b++) {
            if (scanPins[b] <= 0) {
                continue;
            }
            OneWire tempOneWire(scanPins[b]);
            DallasTemperature tempSensors(&tempOneWire);
            tempSensors.begin();
            tempSensors.setWaitForConversion(false);
            tempSensors.requestTemperatures();
        }

        scanConversionStartMs = millis();
        return;
    }

    if (!scanJobActive) {
        return;
    }

    // Non-blocking conversion wait.
    if (millis() - scanConversionStartMs < TOTAL_WAIT_TIME_MS) {
        return;
    }

    JsonDocument doc;
    JsonArray addresses = doc["addresses"].to<JsonArray>();
    JsonArray temperaturesJson = doc["temperatures"].to<JsonArray>();
    JsonArray pins = doc["pins"].to<JsonArray>();

    doc["sensor_count"] = scanTotalCount;

    for (int i = 0; i < scanTotalCount; i++) {
        char hexAddr[17];
        for (int j = 0; j < 8; j++) {
            sprintf(&hexAddr[j * 2], "%02X", scanTempAddrs[i][j]);
        }
        hexAddr[16] = '\0';
        addresses.add((const char*)hexAddr);
        pins.add(scanPins[scanSensorBus[i]]);

        int bus = scanSensorBus[i];
        OneWire tempOneWire(scanPins[bus]);
        DallasTemperature tempSensors(&tempOneWire);
        tempSensors.begin();
        float temp = tempSensors.getTempC(scanTempAddrs[i]);
        if (temp == -127.0f) {
            temperaturesJson.add("NA");
        } else {
            temperaturesJson.add(temp);
        }
    }

    doc["error"] = scanErrorMsg;
    String jsonString;
    serializeJson(doc, jsonString);

    lastScanResultJson = jsonString;
    lastScanResultFullMode = scanJobFullMode;
    scanResultReady = true;
    lastScanCompletedMs = millis();

    scanJobActive = false;
    scanConversionStartMs = 0;

    UnifiedLogger::info("[SCAN] Job complete (%s), sensors=%d, duration=%lu ms\n",
                        scanJobFullMode ? "full" : "configured",
                        scanTotalCount,
                        lastScanCompletedMs - scanJobStartMs);
}

String DS18B20SensorManager::consumeScanResult() {
    StateLockGuard guard(*this);
    String result = lastScanResultJson;
    clearScanResult();
    return result;
}

void DS18B20SensorManager::clearScanResult() {
    StateLockGuard guard(*this);
    lastScanResultJson = "";
    scanResultReady = false;
}

bool DS18B20SensorManager::isParasitePowerMode() {
    StateLockGuard guard(*this);
    for (int b = 0; b < numBuses; b++) {
        if (sensorsPtrs[b]->isParasitePowerMode()) return true;
    }
    return false;
}

bool DS18B20SensorManager::readPowerSupply(int sensorIndex) {
    StateLockGuard guard(*this);
    if (sensorIndex >= 0 && sensorIndex < sensorCount) {
        int bus = busIndex[sensorIndex];
        if (bus >= 0 && bus < numBuses) {
            return sensorsPtrs[bus]->readPowerSupply(sensorAddresses[sensorIndex]);
        }
    }
    return false;
}

bool DS18B20SensorManager::getParasiticPowerStatus(int sensorIndex) {
    StateLockGuard guard(*this);
    if (sensorIndex >= 0 && sensorIndex < sensorCount) {
        return sensorParasiticPower[sensorIndex];
    }
    return false;
}

bool DS18B20SensorManager::getAddress(DeviceAddress &addr, int index) {
    StateLockGuard guard(*this);
    if (index >= 0 && index < sensorCount) {
        memcpy(addr, sensorAddresses[index], 8);
        return true;
    }
    return false;
}

bool DS18B20SensorManager::isSensorOperational(int sensorIndex) {
    StateLockGuard guard(*this);
    if (sensorIndex >= 0 && sensorIndex < sensorCount) {
        // Sensor is operational when its latest reading is considered valid.
        return hasValidData[sensorIndex];
    }
    return false;
}

String DS18B20SensorManager::getSensorAddressHex(int sensorIndex) {
    StateLockGuard guard(*this);
    if (sensorIndex >= 0 && sensorIndex < sensorCount) {
        char hexAddr[24]; // 8 bytes * 2 chars + 7 spaces + null
        int pos = 0;
        for (int j = 0; j < 8; j++) {
            sprintf(&hexAddr[pos], "%02X", sensorAddresses[sensorIndex][j]);
            pos += 2;
            if (j < 7) {
                hexAddr[pos++] = ' ';
            }
        }
        hexAddr[pos] = '\0';
        return String(hexAddr);
    }
    return "";
}

bool DS18B20SensorManager::isDataValid() const {
    StateLockGuard guard(*this);
    return dataValid && !conversionInProgress;
}

bool DS18B20SensorManager::isScanBusy() const {
    StateLockGuard guard(*this);
    return scanJobRequested || scanJobActive;
}

bool DS18B20SensorManager::isScanInProgress() const {
    StateLockGuard guard(*this);
    return scanJobActive;
}

bool DS18B20SensorManager::isScanQueued() const {
    StateLockGuard guard(*this);
    return scanJobRequested;
}

bool DS18B20SensorManager::hasScanResultReady() const {
    StateLockGuard guard(*this);
    return scanResultReady;
}

bool DS18B20SensorManager::isLastScanResultFullMode() const {
    StateLockGuard guard(*this);
    return lastScanResultFullMode;
}

unsigned long DS18B20SensorManager::getLastScanCompletedMs() const {
    StateLockGuard guard(*this);
    return lastScanCompletedMs;
}

bool DS18B20SensorManager::hasCompletedAtLeastOneReadout() const {
    StateLockGuard guard(*this);
    return completedReadouts > 0;
}

unsigned long DS18B20SensorManager::getCompletedReadoutCount() const {
    StateLockGuard guard(*this);
    return completedReadouts;
}

void DS18B20SensorManager::setSensorReadInterval(unsigned long interval) {
    StateLockGuard guard(*this);
    // Prevent underflow in scheduling math (interval - TOTAL_WAIT_TIME_MS).
    const unsigned long minIntervalMs = TOTAL_WAIT_TIME_MS + 1;
    sensorReadInterval = (interval < minIntervalMs) ? minIntervalMs : interval;
}
