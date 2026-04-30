#include "RelayManager.h"
#include "RelayLogger.h"
#include "ErrorHandler.h"
#include "MainController.h"
#include "driver/gpio.h"
#include <set>
#include "UnifiedLogger.h"

RelayManager::RelayManager() :
    currentMode(RELAY_OFF), previousMode(RELAY_OFF), initialized(false),
    lastRelayChangeTime(0), relayTimeoutMs(5000), safetyShutdownActive(false),
    lastCommandSource("startup"), timerActive(false), timerStartMillis(0),
    timerDurationMillis(0), garageRelayModeEnabled(false), garagePulseStartTime(0),
    garagePulseActive(false), mainController(nullptr) {  // Default to startup
#if defined(ARDUINO_ARCH_ESP32)
    stateMutex = xSemaphoreCreateRecursiveMutex();
#endif
}

RelayManager::~RelayManager() {
    // Ensure all relays are turned off when destroyed
    if (initialized) {
        setMode(RELAY_OFF);
    }
#if defined(ARDUINO_ARCH_ESP32)
    if (stateMutex) {
        vSemaphoreDelete(stateMutex);
        stateMutex = nullptr;
    }
#endif
}

void RelayManager::lockState() const {
#if defined(ARDUINO_ARCH_ESP32)
    if (stateMutex) {
        xSemaphoreTakeRecursive(stateMutex, portMAX_DELAY);
    }
#endif
}

void RelayManager::unlockState() const {
#if defined(ARDUINO_ARCH_ESP32)
    if (stateMutex) {
        xSemaphoreGiveRecursive(stateMutex);
    }
#endif
}

void RelayManager::begin(const std::vector<int>& pinsLow, const std::vector<int>& pinsMid, const std::vector<int>& pinsHigh) {
    StateLockGuard guard(*this);
    relayPinsLow = pinsLow;
    relayPinsMid = pinsMid;
    relayPinsHigh = pinsHigh;

    // Initialize all GPIO pins to INPUT_OUTPUT mode for verification capability
    initializePinsWithBidirectionalMode();

    initialized = true;
    currentMode = RELAY_OFF;
    previousMode = RELAY_OFF;

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

    UnifiedLogger::info("RelayManager initialized with bidirectional GPIO mode\n");
    UnifiedLogger::info("LOW pins: %s | MID pins: %s | HIGH pins: %s\n",
                        joinPins(relayPinsLow).c_str(),
                        joinPins(relayPinsMid).c_str(),
                        joinPins(relayPinsHigh).c_str());
}

void RelayManager::setMainController(MainController* controller) {
    StateLockGuard guard(*this);
    mainController = controller;
}

void RelayManager::initializePinsWithBidirectionalMode() {
    // Initialize all GPIO pins to INPUT_OUTPUT mode
    auto initializeBidirectional = [](const std::vector<int>& pins) {
        for (int pin : pins) {
            if (pin >= 0) {
                // Use ESP-IDF to set bidirectional mode
                gpio_set_direction((gpio_num_t)pin, GPIO_MODE_INPUT_OUTPUT);
                digitalWrite(pin, LOW); // Start LOW
            }
        }
    };

    initializeBidirectional(relayPinsLow);
    initializeBidirectional(relayPinsMid);
    initializeBidirectional(relayPinsHigh);
}

void RelayManager::setMode(RelayMode mode) {
    StateLockGuard guard(*this);
    if (!initialized) {
        UnifiedLogger::error("RelayManager: Not initialized, cannot set mode\n");
        return;
    }

    if (mode < RELAY_OFF || mode > RELAY_HIGH) {
        UnifiedLogger::error("RelayManager: Invalid mode requested\n");
        return;
    }

    currentMode = mode;
    setRelayPins();

    UnifiedLogger::info("Relay mode set to: %s\n", getModeString().c_str());
}

void RelayManager::setMode(const String& modeStr) {
    RelayMode mode = stringToMode(modeStr);
    if (mode == RELAY_OFF && modeStr != "off") {
        UnifiedLogger::error("RelayManager: Invalid mode string: %s\n", modeStr.c_str());
        return;
    }
    setMode(mode);
}


String RelayManager::getModeString() const {
    StateLockGuard guard(*this);
    return modeToString(currentMode);
}

void RelayManager::setRelayTimeout(unsigned long timeoutMs) {
    StateLockGuard guard(*this);
    // Validate timeout range (0=disabled, 1000-30000ms as per requirements)
    if (timeoutMs == 0 || (timeoutMs >= 1000 && timeoutMs <= 30000)) {
        relayTimeoutMs = timeoutMs;
        if (timeoutMs == 0) {
            UnifiedLogger::warning("RelayManager: Safety timeout disabled\n");
        } else {
            UnifiedLogger::info("RelayManager: Safety timeout set to %lu ms\n", relayTimeoutMs);
        }
    } else {
        UnifiedLogger::warning("RelayManager: Invalid timeout %lu ms, using default 5000 ms\n", timeoutMs);
        relayTimeoutMs = 5000; // Default to 5 seconds
    }
}

void RelayManager::setSafetyShutdownActive(bool active) {
    StateLockGuard guard(*this);
    safetyShutdownActive = active;
    if (active) {
        UnifiedLogger::warning("RelayManager: Safety shutdown state set to ACTIVE\n");
    } else {
        UnifiedLogger::info("RelayManager: Safety shutdown state set to INACTIVE\n");
    }
}

void RelayManager::setGarageRelayModeEnabled(bool enabled) {
    StateLockGuard guard(*this);
    garageRelayModeEnabled = enabled;
    UnifiedLogger::info("RelayManager: Garage relay mode set to %s\n", enabled ? "ENABLED" : "DISABLED");
}

void RelayManager::updateRelays() {
    StateLockGuard guard(*this);
    if (initialized) {
        // Update garage pulse timing if active
        updateGaragePulse();

        // Only set relay pins if not in active garage pulse (pulse manages its own state)
        if (!garagePulseActive) {
            setRelayPins();
        }
    }
}

bool RelayManager::isRelayChangeAllowed(RelayMode newMode) {
    StateLockGuard guard(*this);
    // Allow any change if timeout is 0 (disabled)
    if (relayTimeoutMs == 0) {
        return true;
    }

    // Allow change to OFF mode regardless of timeout (safety feature)
    if (newMode == RELAY_OFF) {
        return true;
    }

    // Allow change if this is the first time (lastRelayChangeTime is 0)
    if (lastRelayChangeTime == 0) {
        return true;
    }

    // Check if enough time has elapsed since last relay change
    unsigned long currentTime = millis();
    unsigned long timeSinceLastChange = currentTime - lastRelayChangeTime;

    if (timeSinceLastChange >= relayTimeoutMs) {
        return true;
    }

    // Timeout not elapsed yet
    return false;
}

void RelayManager::setRelayPins() {
    if (!initialized) return;

    // Helper function to set all pins in a vector to a specific state
    auto setPins = [](const std::vector<int>& pins, int state) {
        for (int pin : pins) {
            if (pin >= 0) {
                digitalWrite(pin, state);
            }
        }
    };

    // Turn off all pins first (set all configured pins to LOW)
    setPins(relayPinsLow, LOW);
    setPins(relayPinsMid, LOW);
    setPins(relayPinsHigh, LOW);

    // Set pins based on current mode (independent assignment)
    switch (currentMode) {
        case RELAY_LOW:
            setPins(relayPinsLow, HIGH);
            break;
        case RELAY_MID:
            setPins(relayPinsMid, HIGH);
            break;
        case RELAY_HIGH:
            setPins(relayPinsHigh, HIGH);
            break;
        case RELAY_OFF:
        default:
            // All pins already set to LOW
            break;
    }
}

bool RelayManager::verifyPinState(int pin, int expectedState) {
    if (pin < 0) return true; // Skip invalid pins

    int actualState = digitalRead(pin);
    return (actualState == expectedState);
}

bool RelayManager::verifyPinGroup(const std::vector<int>& pins, int expectedState) {
    bool groupSuccess = true;
    for (int pin : pins) {
        if (pin >= 0) {
            bool pinSuccess = verifyPinState(pin, expectedState);
            if (!pinSuccess) groupSuccess = false;
        }
    }
    return groupSuccess;
}

bool RelayManager::setModeWithVerification(RelayMode mode, const String& source, bool isSafetyOperation) {
    StateLockGuard guard(*this);
    if (!initialized) return false;

    // ⚠️ ADDED: Mode validation (from setModeWithLogging)
    if (mode < RELAY_OFF || mode > RELAY_HIGH) {
        UnifiedLogger::error("RelayManager: Invalid mode requested\n");
        return false;
    }

    // ⚠️ ADDED: Safety shutdown lockout check - prevent manual control during safety shutdown
    if (safetyShutdownActive && !isSafetyOperation) {
        String errorMessage = "Relay control blocked due to active safety shutdown. Temperature must drop below threshold before manual control is allowed.";
        ErrorHandler::reportError(ErrorCode::RELAY_SAFETY_SHUTDOWN,
                                ErrorSeverity::WARNING, errorMessage);

        UnifiedLogger::warning("[RELAY_SAFETY_LOCKOUT] Mode change %s denied (source: %s) - safety shutdown active\n",
                      modeToString(mode).c_str(), source.c_str());

        // Log to relay logger as denied request
        RelayLogger::logEvent(modeToString(currentMode), modeToString(mode), source + "_SAFETY_LOCKOUT", false);

        return false; // Command denied due to safety lockout
    }

    // ⚠️ ADDED: Safety timeout check - prevent rapid relay switching
    if (!isRelayChangeAllowed(mode)) {
        // Log timeout violation to error handler
        String errorMessage = "Relay state change denied due to safety timeout. Minimum interval: " + String(relayTimeoutMs) + "ms";
        ErrorHandler::reportError(ErrorCode::RELAY_TIMEOUT_VIOLATION, ErrorSeverity::WARNING, errorMessage);

        // Log to serial
        unsigned long currentTime = millis();
        unsigned long timeSinceLastChange = currentTime - lastRelayChangeTime;
        unsigned long remainingTime = relayTimeoutMs - timeSinceLastChange;

        UnifiedLogger::warning("[RELAY_TIMEOUT_DENIED] Mode change %s -> %s DENIED (source: %s) | Timeout: %lu/%lu ms remaining | Remaining: %lu ms\n",
                      modeToString(currentMode).c_str(), modeToString(mode).c_str(),
                      source.c_str(), timeSinceLastChange, relayTimeoutMs, remainingTime);

        // Log to relay logger as denied request
        RelayLogger::logEvent(modeToString(currentMode), modeToString(mode), source + "_TIMEOUT", false);

        return false; // Command denied, not failed
    }

    // Store previous mode for logging
    previousMode = currentMode;
    String prevModeStr = modeToString(previousMode);

    // Store command source
    lastCommandSource = source;

    // Start timing for actual relay operations
    unsigned long operationStartTime = micros();

    // Handle garage relay mode pulse behavior
    if (garageRelayModeEnabled && (mode == RELAY_LOW || mode == RELAY_MID || mode == RELAY_HIGH)) {
        // Garage mode: Pulse for 500ms instead of holding state
        UnifiedLogger::info("[GARAGE_MODE] Pulse start - Mode: %s, Time: %lu ms\n", modeToString(mode).c_str(), millis());

        // Set pins normally for pulse start
        currentMode = mode;
        setRelayPins();

        // Start pulse timing
        garagePulseStartTime = millis();
        garagePulseActive = true;

        // Skip normal mode setting - pulse will be managed by updateGaragePulse()
    } else {
        // Normal behavior: Set mode and hold state
        currentMode = mode;
        setRelayPins();
    }

    // Update last relay change time
    lastRelayChangeTime = millis();

    // Small delay for GPIO settling (100 microseconds)
    delayMicroseconds(100);

    // Verify all pins in the new configuration
    bool allVerified = true;
    String newModeStr = getModeString();

    // Collect all unique pins
    std::set<int> allPins;
    for (int pin : relayPinsLow) if (pin >= 0) allPins.insert(pin);
    for (int pin : relayPinsMid) if (pin >= 0) allPins.insert(pin);
    for (int pin : relayPinsHigh) if (pin >= 0) allPins.insert(pin);

    // Determine which pins should be HIGH for this mode
    std::set<int> shouldBeHigh;
    switch (mode) {
        case RELAY_OFF:
            // None
            break;
        case RELAY_LOW:
            for (int pin : relayPinsLow) if (pin >= 0) shouldBeHigh.insert(pin);
            break;
        case RELAY_MID:
            for (int pin : relayPinsMid) if (pin >= 0) shouldBeHigh.insert(pin);
            break;
        case RELAY_HIGH:
            for (int pin : relayPinsHigh) if (pin >= 0) shouldBeHigh.insert(pin);
            break;
    }
    for (int pin : allPins) {
        int expected = (shouldBeHigh.count(pin) > 0) ? HIGH : LOW;
        int actual = digitalRead(pin);
        bool pinOk = (actual == expected);
        if (!pinOk) allVerified = false;
    }

    // End timing and report to performance monitoring
    unsigned long operationDuration = (micros() - operationStartTime) / 1000; // Convert to milliseconds
    if (mainController) {
        mainController->updateComponentStatsPublic(RELAY_INDEX, operationDuration);
    }

    UnifiedLogger::info("[RELAY_CHANGE] %s -> %s (source: %s) | VERIFICATION: %s\n",
                  prevModeStr.c_str(), newModeStr.c_str(), source.c_str(),
                  allVerified ? "PASSED" : "FAILED");

    // ⚠️ ADDED: File logging (from setModeWithLogging)
    const bool isNoOpChange = (prevModeStr == newModeStr);
    const bool suppressNoOpSafetyTempLog = allVerified && isNoOpChange && (source == "safety_temperature");
    if (!suppressNoOpSafetyTempLog) {
        RelayLogger::logEvent(prevModeStr, newModeStr, source, allVerified);
    }

    return allVerified;
}

bool RelayManager::setModeWithVerification(const String& modeStr, const String& source, bool isSafetyOperation) {
    RelayMode mode = stringToMode(modeStr);
    if (mode == RELAY_OFF && modeStr != "off") {
        UnifiedLogger::error("RelayManager: Invalid mode string: %s\n", modeStr.c_str());
        return false;
    }
    return setModeWithVerification(mode, source, isSafetyOperation);
}

// Timer functionality implementation
void RelayManager::startTimer(int minutes) {
    StateLockGuard guard(*this);
    if (minutes <= 0) {
        UnifiedLogger::error("RelayManager: Invalid timer duration, must be positive minutes\n");
        return;
    }

    timerActive = true;
    timerStartMillis = millis();
    timerDurationMillis = (unsigned long)minutes * 60 * 1000; // Convert minutes to milliseconds

    UnifiedLogger::info("RelayManager: Timer started for %d minutes (%lu ms), active=%d, start=%lu, duration=%lu\n", minutes, timerDurationMillis, timerActive, timerStartMillis, timerDurationMillis);
}

void RelayManager::stopTimer() {
    StateLockGuard guard(*this);
    timerActive = false;
    timerStartMillis = 0;
    timerDurationMillis = 0;

    UnifiedLogger::info("RelayManager: Timer stopped\n");
}

unsigned long RelayManager::getRemainingSeconds() {
    StateLockGuard guard(*this);
    if (!timerActive) {
        return 0;
    }

    unsigned long elapsedMillis = millis() - timerStartMillis;
    if (elapsedMillis >= timerDurationMillis) {
        return 0; // Timer has expired
    }

    unsigned long remainingMillis = timerDurationMillis - elapsedMillis;
    return remainingMillis / 1000; // Convert to seconds
}

unsigned long RelayManager::getTotalSeconds() {
    StateLockGuard guard(*this);
    return timerDurationMillis / 1000; // Convert milliseconds to seconds
}

bool RelayManager::isTimerActive() {
    StateLockGuard guard(*this);
    return timerActive;
}

bool RelayManager::isExpired() {
    StateLockGuard guard(*this);
    if (!timerActive) {
        return false;
    }

    unsigned long currentMillis = millis();
    unsigned long elapsedMillis = currentMillis - timerStartMillis;
    return elapsedMillis >= timerDurationMillis;
}

void RelayManager::updateGaragePulse() {
    StateLockGuard guard(*this);
    if (garagePulseActive && millis() - garagePulseStartTime >= 500) {
        // Start timing for pulse end operation
        unsigned long pulseEndStartTime = micros();

        // Pulse duration elapsed - turn off all relays
        UnifiedLogger::info("[GARAGE_MODE] Pulse end - Duration: %lu ms (target: 500ms)\n", millis() - garagePulseStartTime);

        // Turn off all pins
        auto setPinsLow = [](const std::vector<int>& pins) {
            for (int pin : pins) {
                if (pin >= 0) {
                    digitalWrite(pin, LOW);
                }
            }
        };

        setPinsLow(relayPinsLow);
        setPinsLow(relayPinsMid);
        setPinsLow(relayPinsHigh);

        // Reset pulse state
        garagePulseActive = false;

        // Set current mode to OFF to reflect actual state
        currentMode = RELAY_OFF;

        // End timing and report to performance monitoring
        unsigned long pulseEndDuration = (micros() - pulseEndStartTime) / 1000; // Convert to milliseconds
        if (mainController) {
            mainController->updateComponentStatsPublic(RELAY_INDEX, pulseEndDuration);
        }
    }
}

RelayMode RelayManager::stringToMode(const String& modeStr) {
    String lowerStr = modeStr;
    lowerStr.toLowerCase();
    
    if (lowerStr == "off") return RELAY_OFF;
    if (lowerStr == "low") return RELAY_LOW;
    if (lowerStr == "mid") return RELAY_MID;
    if (lowerStr == "high") return RELAY_HIGH;
    
    return RELAY_OFF; // Default to OFF for invalid strings
}

String RelayManager::modeToString(RelayMode mode) {
    switch (mode) {
        case RELAY_OFF: return "off";
        case RELAY_LOW: return "low";
        case RELAY_MID: return "mid";
        case RELAY_HIGH: return "high";
        default: return "off";
    }
}

RelayMode RelayManager::getCurrentMode() const {
    StateLockGuard guard(*this);
    return currentMode;
}

unsigned long RelayManager::getRelayTimeoutMs() const {
    StateLockGuard guard(*this);
    return relayTimeoutMs;
}

bool RelayManager::isInitialized() const {
    StateLockGuard guard(*this);
    return initialized;
}

bool RelayManager::isSafetyShutdownActive() const {
    StateLockGuard guard(*this);
    return safetyShutdownActive;
}

String RelayManager::getLastCommandSource() const {
    StateLockGuard guard(*this);
    return lastCommandSource;
}

