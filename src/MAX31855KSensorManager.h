#ifndef MAX31855K_SENSOR_MANAGER_H
#define MAX31855K_SENSOR_MANAGER_H

#include <Adafruit_SPIDevice.h>
#include <Arduino.h>
#include <stdint.h>
#if defined(ARDUINO_ARCH_ESP32)
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#endif

// Single-channel MAX31855K reader using a single 32-bit frame per update.
// Software SPI is used (no MOSI line required):
// - DO   (MISO-like): input
// - CLK  : output
// - CS   : output
class MAX31855KSensorManager {
public:
    MAX31855KSensorManager(int doPin, int clkPin, int csPin);
    ~MAX31855KSensorManager();

    // Initializes the Adafruit driver. If init fails, update() will keep retrying.
    bool begin();

    // Reads temperatures and updates fault status.
    // Returns true if communication succeeded (even if a fault is present).
    bool update();

    // Getters
    float getThermocoupleC() const;
    float getInternalJunctionC() const;

    // Raw 32-bit frame is not exposed by Adafruit's API.
    // Kept for backwards-compatibility: always returns 0.
    uint32_t getRawFrame() const;

    bool hasFault() const;
    bool faultOpenCircuit() const;
    bool faultShortToGnd() const;
    bool faultShortToVcc() const;
    bool isSensorOperational() const; // last read OK AND no fault
    const char* getStatusString() const; // "OK" / "FAULT" / "NO_DATA" / "GLITCH"

    // Diagnostics
    uint32_t getReadCount() const;
    uint32_t getFaultCount() const;
    unsigned long getLastOkMillis() const;
    unsigned long getLastReadMillis() const;

private:
    int doPin_;
    int clkPin_;
    int csPin_;

    Adafruit_SPIDevice spiDev_;
    bool begun_;

    uint32_t rawFrame_;
    float thermocoupleC_;
    float internalJunctionC_;
    float lastGoodThermocoupleC_;
    float pendingOutlierThermocoupleC_;
    uint8_t pendingOutlierCount_;
    unsigned long pendingOutlierSinceMs_;

    bool lastReadOk_;
    bool fault_;
    bool faultOC_;
    bool faultSCG_;
    bool faultSCV_;
    bool glitchRejected_;

    uint32_t readCount_;
    uint32_t faultCount_;
    unsigned long lastOkMillis_;
    unsigned long lastReadMillis_;

    // Error reporting state (anti-spam)
    bool prevReadOk_;
    bool prevFault_;
    unsigned long lastReadFailedReportMs_;
    unsigned long lastFaultReportMs_;

    void updateStatusString_();
    void maybeReportReadFailed_(unsigned long now, const char* reason);
    void maybeReportFault_(unsigned long now, uint8_t errorBits);

    const char* statusString_;

    struct Sample_;
    bool readSampleWithRetries_(Sample_& out, String& lastReason, uint8_t attempts);
    bool readSampleOnce_(Sample_& out, String& reason);

#if defined(ARDUINO_ARCH_ESP32)
    mutable SemaphoreHandle_t stateMutex = nullptr;
#endif

    void lockState() const;
    void unlockState() const;
    class StateLockGuard {
    public:
        explicit StateLockGuard(const MAX31855KSensorManager& ownerRef) : owner(ownerRef) {
            owner.lockState();
        }
        ~StateLockGuard() {
            owner.unlockState();
        }
    private:
        const MAX31855KSensorManager& owner;
    };
};

#endif // MAX31855K_SENSOR_MANAGER_H

