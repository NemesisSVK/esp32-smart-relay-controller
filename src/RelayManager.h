#ifndef RELAYMANAGER_H
#define RELAYMANAGER_H

#include <Arduino.h>
#include <vector>
#if defined(ARDUINO_ARCH_ESP32)
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#endif

// Forward declaration to avoid circular dependencies
class MainController;

// Relay mode enumeration
enum RelayMode {
    RELAY_OFF = 0,
    RELAY_LOW = 1,
    RELAY_MID = 2,
    RELAY_HIGH = 3
};

class RelayManager {
public:
    RelayManager();
    ~RelayManager();

    // Initialize relay manager with configurable pin arrays for each state
    void begin(const std::vector<int>& pinsLow, const std::vector<int>& pinsMid, const std::vector<int>& pinsHigh);

    // Set relay mode (original methods)
    void setMode(RelayMode mode);
    void setMode(const String& modeStr);

    // Set relay mode with verification
    bool setModeWithVerification(RelayMode mode, const String& source, bool isSafetyOperation = false);
    bool setModeWithVerification(const String& modeStr, const String& source, bool isSafetyOperation = false);

    // Get actual pin state for verification
    bool verifyPinState(int pin, int expectedState);

    // Get current relay mode
    String getModeString() const;
    RelayMode getCurrentMode() const;

    // Get relay pins for each state
    const std::vector<int>& getLowPins() const { return relayPinsLow; }
    const std::vector<int>& getMidPins() const { return relayPinsMid; }
    const std::vector<int>& getHighPins() const { return relayPinsHigh; }

    // Set safety timeout configuration
    void setRelayTimeout(unsigned long timeoutMs);

    // Get current relay timeout
    unsigned long getRelayTimeoutMs() const;

    // Set safety shutdown state
    void setSafetyShutdownActive(bool active);

    // Set garage relay mode enabled/disabled
    void setGarageRelayModeEnabled(bool enabled);

    // Performance monitoring integration
    void setMainController(MainController* controller);

    // Update relay state based on current mode
    void updateRelays();

    // Check if relay manager is initialized
    bool isInitialized() const;

    // Check if safety shutdown is active
    bool isSafetyShutdownActive() const;

    // Get last command source
    String getLastCommandSource() const;

    // Timer functionality
    void startTimer(int minutes);
    void stopTimer();
    unsigned long getRemainingSeconds();
    unsigned long getTotalSeconds();
    bool isTimerActive();
    bool isExpired();

    // Get relay mode from string
    static RelayMode stringToMode(const String& modeStr);
    static String modeToString(RelayMode mode);

private:
    // Configurable GPIO pin assignments for each state
    std::vector<int> relayPinsLow;   // Pins for LOW state
    std::vector<int> relayPinsMid;   // Pins for MID state
    std::vector<int> relayPinsHigh;  // Pins for HIGH state

    RelayMode currentMode;
    RelayMode previousMode;  // For logging
    bool initialized;
    unsigned long lastRelayChangeTime;  // Track when last relay change occurred
    unsigned long relayTimeoutMs;      // Safety timeout between relay changes
    bool safetyShutdownActive;         // Whether safety shutdown is currently active
    String lastCommandSource;          // Track source of last relay command

    // Timer state variables
    bool timerActive;
    unsigned long timerStartMillis;
    unsigned long timerDurationMillis;

    // Garage relay mode variables
    bool garageRelayModeEnabled;
    unsigned long garagePulseStartTime;
    bool garagePulseActive;

    // Performance monitoring
    MainController* mainController;

#if defined(ARDUINO_ARCH_ESP32)
    mutable SemaphoreHandle_t stateMutex = nullptr;
#endif

    void lockState() const;
    void unlockState() const;
    class StateLockGuard {
    public:
        explicit StateLockGuard(const RelayManager& ownerRef) : owner(ownerRef) {
            owner.lockState();
        }
        ~StateLockGuard() {
            owner.unlockState();
        }
    private:
        const RelayManager& owner;
    };

    // Set individual relay pins based on mode
    void setRelayPins();

    // Initialize pins with bidirectional mode for verification
    void initializePinsWithBidirectionalMode();

    // Verify pin group states
    bool verifyPinGroup(const std::vector<int>& pins, int expectedState);

    // Check if relay change is allowed based on timeout
    bool isRelayChangeAllowed(RelayMode newMode);

    // Update garage pulse timing
    void updateGaragePulse();
};

#endif // RELAYMANAGER_H
