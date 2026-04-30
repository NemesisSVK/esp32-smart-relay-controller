#include "MAX31855KSensorManager.h"

#include "ErrorHandler.h"

#include <math.h>

namespace {
    // Conservative rate-limit interval for repeated identical error conditions.
    constexpr unsigned long REPORT_INTERVAL_MS = 60UL * 1000UL;

    // MAX31855 frame format sanity checks:
    // - Bit 17 is reserved (should be 0)
    // - Bit 3 is reserved (should be 0)
    constexpr uint32_t RESERVED_BIT_17 = (1UL << 17);
    constexpr uint32_t RESERVED_BIT_3 = (1UL << 3);

    // Fault detail bits (D2..D0) in the 32-bit frame.
    constexpr uint8_t FAULT_OPEN = 0x01;
    constexpr uint8_t FAULT_SHORT_GND = 0x02;
    constexpr uint8_t FAULT_SHORT_VCC = 0x04;

    // Outlier rejection / confirmation:
    // If a new reading differs from the last good value by more than this threshold,
    // it must be confirmed by additional immediate reads (median + consensus).
    constexpr float OUTLIER_JUMP_C = 30.0f;
    constexpr float CONFIRM_EPSILON_C = 2.0f;
    constexpr uint8_t OUTLIER_EXTRA_READS = 3;   // number of extra reads when confirming
    constexpr uint8_t SAMPLE_READ_RETRIES = 3;   // retries for corrupted frames / SPI read failures
    constexpr uint8_t OUTLIER_CONSECUTIVE_REQUIRED = 2; // require persistence across update cycles

    static float parseThermocoupleC(uint32_t frame) {
        // Bits 31..18: signed 14-bit thermocouple temperature, LSB=0.25C
        int32_t v;
        if (frame & 0x80000000UL) {
            v = static_cast<int32_t>(0xFFFFC000UL | ((frame >> 18) & 0x00003FFFUL));
        } else {
            v = static_cast<int32_t>(frame >> 18);
        }
        return static_cast<float>(v) * 0.25f;
    }

    static float parseInternalJunctionC(uint32_t frame) {
        // Bits 15..4: signed 12-bit internal temperature, LSB=0.0625C
        uint32_t v = frame >> 4;

        float internal = static_cast<float>(v & 0x7FFUL);
        if (v & 0x800UL) {
            int16_t tmp = static_cast<int16_t>(0xF800 | (v & 0x7FFUL));
            internal = static_cast<float>(tmp);
        }
        internal *= 0.0625f;
        return internal;
    }

    static bool reservedBitsOk(uint32_t frame) {
        return ((frame & RESERVED_BIT_17) == 0) && ((frame & RESERVED_BIT_3) == 0);
    }

    static float fabsf_compat(float x) {
        return static_cast<float>(fabs(static_cast<double>(x)));
    }
}

struct MAX31855KSensorManager::Sample_ {
    uint32_t frame;
    float tcC;
    float internalC;
    uint8_t errorBits;
    bool fault;
    bool faultOC;
    bool faultSCG;
    bool faultSCV;
};

MAX31855KSensorManager::MAX31855KSensorManager(int doPin, int clkPin, int csPin)
    : doPin_(doPin),
      clkPin_(clkPin),
      csPin_(csPin),
      spiDev_(static_cast<int8_t>(csPin), static_cast<int8_t>(clkPin), static_cast<int8_t>(doPin), -1, 1000000),
      begun_(false),
      rawFrame_(0),
      thermocoupleC_(NAN),
      internalJunctionC_(NAN),
      lastGoodThermocoupleC_(NAN),
      pendingOutlierThermocoupleC_(NAN),
      pendingOutlierCount_(0),
      pendingOutlierSinceMs_(0),
      lastReadOk_(false),
      fault_(false),
      faultOC_(false),
      faultSCG_(false),
      faultSCV_(false),
      glitchRejected_(false),
      readCount_(0),
      faultCount_(0),
      lastOkMillis_(0),
      lastReadMillis_(0),
      prevReadOk_(false),
      prevFault_(false),
      lastReadFailedReportMs_(0),
      lastFaultReportMs_(0),
      statusString_("NO_DATA") {
#if defined(ARDUINO_ARCH_ESP32)
    stateMutex = xSemaphoreCreateRecursiveMutex();
#endif
}

MAX31855KSensorManager::~MAX31855KSensorManager() {
#if defined(ARDUINO_ARCH_ESP32)
    if (stateMutex) {
        vSemaphoreDelete(stateMutex);
        stateMutex = nullptr;
    }
#endif
}

void MAX31855KSensorManager::lockState() const {
#if defined(ARDUINO_ARCH_ESP32)
    if (stateMutex) {
        xSemaphoreTakeRecursive(stateMutex, portMAX_DELAY);
    }
#endif
}

void MAX31855KSensorManager::unlockState() const {
#if defined(ARDUINO_ARCH_ESP32)
    if (stateMutex) {
        xSemaphoreGiveRecursive(stateMutex);
    }
#endif
}

bool MAX31855KSensorManager::begin() {
    StateLockGuard guard(*this);
    // Reset state
    begun_ = false;
    rawFrame_ = 0;
    thermocoupleC_ = NAN;
    internalJunctionC_ = NAN;
    lastGoodThermocoupleC_ = NAN;
    pendingOutlierThermocoupleC_ = NAN;
    pendingOutlierCount_ = 0;
    pendingOutlierSinceMs_ = 0;
    lastReadOk_ = false;
    fault_ = false;
    faultOC_ = false;
    faultSCG_ = false;
    faultSCV_ = false;
    glitchRejected_ = false;
    updateStatusString_();

    // Try to initialize immediately, but allow runtime retries if it fails.
    begun_ = spiDev_.begin();

    return begun_;
}

bool MAX31855KSensorManager::update() {
    StateLockGuard guard(*this);
    const unsigned long now = millis();

    prevReadOk_ = lastReadOk_;
    prevFault_ = fault_;

    lastReadMillis_ = now;
    readCount_++;
    glitchRejected_ = false;

    // Ensure begin() succeeded. If it didn't, keep retrying.
    if (!begun_) {
        begun_ = spiDev_.begin();
        if (!begun_) {
            lastReadOk_ = false;
            thermocoupleC_ = NAN;
            internalJunctionC_ = NAN;
            fault_ = false;
            faultOC_ = faultSCG_ = faultSCV_ = false;
            updateStatusString_();
            maybeReportReadFailed_(now, "begin() failed");
            return false;
        }
    }

    Sample_ first;
    String sampleFailReason;
    if (!readSampleWithRetries_(first, sampleFailReason, SAMPLE_READ_RETRIES)) {
        lastReadOk_ = false;
        thermocoupleC_ = NAN;
        internalJunctionC_ = NAN;
        fault_ = false;
        faultOC_ = faultSCG_ = faultSCV_ = false;
        updateStatusString_();
        maybeReportReadFailed_(now, sampleFailReason.c_str());
        return false;
    }

    // Update raw frame and internal temperature from the successfully read frame.
    rawFrame_ = first.frame;
    internalJunctionC_ = first.internalC;

    // Fault handling: if any fault is present, report and publish NaN for thermocouple temperature.
    if (first.fault) {
        fault_ = true;
        faultOC_ = first.faultOC;
        faultSCG_ = first.faultSCG;
        faultSCV_ = first.faultSCV;

        thermocoupleC_ = NAN;
        lastReadOk_ = true; // communication was OK; sensor reports a fault condition
        updateStatusString_();
        faultCount_++;
        maybeReportFault_(now, first.errorBits);
        return true;
    }

    // No fault bits: candidate thermocouple temperature is available.
    const float candidateTc = first.tcC;
    const bool needsConfirm = isnan(lastGoodThermocoupleC_) ||
                              (fabsf_compat(candidateTc - lastGoodThermocoupleC_) > OUTLIER_JUMP_C);

    if (!needsConfirm) {
        pendingOutlierThermocoupleC_ = NAN;
        pendingOutlierCount_ = 0;
        pendingOutlierSinceMs_ = 0;

        // Normal OK reading
        fault_ = false;
        faultOC_ = faultSCG_ = faultSCV_ = false;
        thermocoupleC_ = candidateTc;
        lastGoodThermocoupleC_ = candidateTc;
        lastReadOk_ = true;
        updateStatusString_();
        lastOkMillis_ = now;
        return true;
    }

    // Outlier / first-reading confirmation: take additional immediate samples, then accept median if there is consensus.
    struct Candidate {
        float tcC;
        uint32_t frame;
        float internalC;
    };

    Candidate candidates[1 + OUTLIER_EXTRA_READS];
    uint8_t count = 0;
    candidates[count++] = {candidateTc, first.frame, first.internalC};

    for (uint8_t i = 0; i < OUTLIER_EXTRA_READS; i++) {
        Sample_ s;
        String reason;
        // For confirmation reads, do a small retry loop too (corrupted frames should be discarded).
        if (!readSampleWithRetries_(s, reason, 2)) {
            continue;
        }
        if (s.fault) {
            continue;
        }
        candidates[count++] = {s.tcC, s.frame, s.internalC};
    }

    if (count < 2) {
        lastReadOk_ = false;
        glitchRejected_ = true;
        // Keep last good value (if any) to avoid graph spikes; signal glitch via status.
        thermocoupleC_ = lastGoodThermocoupleC_;
        fault_ = false;
        faultOC_ = faultSCG_ = faultSCV_ = false;
        updateStatusString_();
        maybeReportReadFailed_(now, "outlier confirmation: insufficient samples");
        return false;
    }

    // Sort candidates by temperature (small N, simple sort).
    for (uint8_t i = 0; i < count; i++) {
        for (uint8_t j = i + 1; j < count; j++) {
            if (candidates[j].tcC < candidates[i].tcC) {
                Candidate tmp = candidates[i];
                candidates[i] = candidates[j];
                candidates[j] = tmp;
            }
        }
    }

    const uint8_t medianIndex = count / 2;
    const float medianTc = candidates[medianIndex].tcC;

    uint8_t agree = 0;
    for (uint8_t i = 0; i < count; i++) {
        if (fabsf_compat(candidates[i].tcC - medianTc) <= CONFIRM_EPSILON_C) {
            agree++;
        }
    }

    if (agree < 2) {
        lastReadOk_ = false;
        glitchRejected_ = true;
        // Keep last good value (if any) to avoid graph spikes; signal glitch via status.
        thermocoupleC_ = lastGoodThermocoupleC_;
        fault_ = false;
        faultOC_ = faultSCG_ = faultSCV_ = false;
        updateStatusString_();
        maybeReportReadFailed_(now, "outlier rejected (no consensus)");
        return false;
    }

    // Confirmed median sample. If this is an outlier relative to the last good value, require it to persist across
    // consecutive update cycles before accepting it. This prevents one-cycle spikes even if the transient is
    // consistent across a few immediate reads.
    if (!isnan(lastGoodThermocoupleC_) && (fabsf_compat(medianTc - lastGoodThermocoupleC_) > OUTLIER_JUMP_C)) {
        if (!isnan(pendingOutlierThermocoupleC_) &&
            (fabsf_compat(medianTc - pendingOutlierThermocoupleC_) <= CONFIRM_EPSILON_C)) {
            pendingOutlierCount_++;
        } else {
            pendingOutlierThermocoupleC_ = medianTc;
            pendingOutlierCount_ = 1;
            pendingOutlierSinceMs_ = now;
        }

        if (pendingOutlierCount_ < OUTLIER_CONSECUTIVE_REQUIRED) {
            lastReadOk_ = false;
            glitchRejected_ = true;
            thermocoupleC_ = lastGoodThermocoupleC_;
            fault_ = false;
            faultOC_ = faultSCG_ = faultSCV_ = false;
            rawFrame_ = candidates[medianIndex].frame;
            internalJunctionC_ = candidates[medianIndex].internalC;
            updateStatusString_();
            maybeReportReadFailed_(now, "outlier pending (needs consecutive confirmation)");
            return false;
        }

        // Outlier persisted: accept it.
        pendingOutlierThermocoupleC_ = NAN;
        pendingOutlierCount_ = 0;
        pendingOutlierSinceMs_ = 0;
    } else {
        pendingOutlierThermocoupleC_ = NAN;
        pendingOutlierCount_ = 0;
        pendingOutlierSinceMs_ = 0;
    }

    // Accept confirmed sample.
    fault_ = false;
    faultOC_ = faultSCG_ = faultSCV_ = false;
    thermocoupleC_ = medianTc;
    lastGoodThermocoupleC_ = medianTc;
    rawFrame_ = candidates[medianIndex].frame;
    internalJunctionC_ = candidates[medianIndex].internalC;
    lastReadOk_ = true;
    updateStatusString_();
    lastOkMillis_ = now;
    return true;
}

float MAX31855KSensorManager::getThermocoupleC() const {
    StateLockGuard guard(*this);
    return thermocoupleC_;
}

float MAX31855KSensorManager::getInternalJunctionC() const {
    StateLockGuard guard(*this);
    return internalJunctionC_;
}

uint32_t MAX31855KSensorManager::getRawFrame() const {
    StateLockGuard guard(*this);
    return rawFrame_;
}

bool MAX31855KSensorManager::hasFault() const {
    StateLockGuard guard(*this);
    return fault_;
}

bool MAX31855KSensorManager::faultOpenCircuit() const {
    StateLockGuard guard(*this);
    return faultOC_;
}

bool MAX31855KSensorManager::faultShortToGnd() const {
    StateLockGuard guard(*this);
    return faultSCG_;
}

bool MAX31855KSensorManager::faultShortToVcc() const {
    StateLockGuard guard(*this);
    return faultSCV_;
}

bool MAX31855KSensorManager::isSensorOperational() const {
    StateLockGuard guard(*this);
    return lastReadOk_ && !fault_;
}

const char* MAX31855KSensorManager::getStatusString() const {
    StateLockGuard guard(*this);
    return statusString_;
}

uint32_t MAX31855KSensorManager::getReadCount() const {
    StateLockGuard guard(*this);
    return readCount_;
}

uint32_t MAX31855KSensorManager::getFaultCount() const {
    StateLockGuard guard(*this);
    return faultCount_;
}

unsigned long MAX31855KSensorManager::getLastOkMillis() const {
    StateLockGuard guard(*this);
    return lastOkMillis_;
}

unsigned long MAX31855KSensorManager::getLastReadMillis() const {
    StateLockGuard guard(*this);
    return lastReadMillis_;
}

void MAX31855KSensorManager::updateStatusString_() {
    StateLockGuard guard(*this);
    if (!lastReadOk_) {
        statusString_ = glitchRejected_ ? "GLITCH" : "NO_DATA";
        return;
    }

    statusString_ = fault_ ? "FAULT" : "OK";
}

void MAX31855KSensorManager::maybeReportReadFailed_(unsigned long now, const char* reason) {
    const bool becameBad = (prevReadOk_ && !lastReadOk_);
    const bool firstEver = (!prevReadOk_ && lastReadFailedReportMs_ == 0);
    const bool intervalElapsed = (lastReadFailedReportMs_ == 0) || (now - lastReadFailedReportMs_ >= REPORT_INTERVAL_MS);

    if (becameBad || firstEver || intervalElapsed) {
        lastReadFailedReportMs_ = now;
        ErrorHandler::reportError(
            ErrorCode::MAX31855K_READ_FAILED,
            ErrorSeverity::WARNING,
            "MAX31855K read failed (" + String(reason) + ") on pins DO=" + String(doPin_) + ", CLK=" + String(clkPin_) + ", CS=" + String(csPin_)
        );
    }
}

void MAX31855KSensorManager::maybeReportFault_(unsigned long now, uint8_t errorBits) {
    const bool becameFault = (!prevFault_ && fault_);
    const bool intervalElapsed = (lastFaultReportMs_ == 0) || (now - lastFaultReportMs_ >= REPORT_INTERVAL_MS);

    if (becameFault || intervalElapsed) {
        lastFaultReportMs_ = now;

        String details;
        if (faultOC_) details += "OC ";
        if (faultSCG_) details += "SCG ";
        if (faultSCV_) details += "SCV ";
        if (details.isEmpty()) details = "(no detail bits)";

        ErrorHandler::reportError(
            ErrorCode::MAX31855K_FAULT,
            ErrorSeverity::WARNING,
            "MAX31855K fault: " + details + "err=0x" + String(errorBits, HEX) + " pins DO=" + String(doPin_) + ", CLK=" + String(clkPin_) + ", CS=" + String(csPin_)
        );
    }
}

bool MAX31855KSensorManager::readSampleOnce_(Sample_& out, String& reason) {
    uint8_t buf[4] = {0};
    if (!spiDev_.read(buf, 4)) {
        reason = "spi read failed";
        return false;
    }

    uint32_t frame = 0;
    frame |= static_cast<uint32_t>(buf[0]) << 24;
    frame |= static_cast<uint32_t>(buf[1]) << 16;
    frame |= static_cast<uint32_t>(buf[2]) << 8;
    frame |= static_cast<uint32_t>(buf[3]);

    if (!reservedBitsOk(frame)) {
        reason = "reserved bits set (corrupted frame)";
        return false;
    }

    out.frame = frame;
    out.internalC = parseInternalJunctionC(frame);

    const uint8_t errorBits = static_cast<uint8_t>(frame & 0x07);
    const bool faultGeneral = (frame & 0x00010000UL) != 0;

    out.errorBits = errorBits;
    out.faultOC = (errorBits & FAULT_OPEN) != 0;
    out.faultSCG = (errorBits & FAULT_SHORT_GND) != 0;
    out.faultSCV = (errorBits & FAULT_SHORT_VCC) != 0;
    out.fault = faultGeneral || (errorBits != 0);

    if (out.fault) {
        out.tcC = NAN;
        return true;
    }

    out.tcC = parseThermocoupleC(frame);
    return true;
}

bool MAX31855KSensorManager::readSampleWithRetries_(Sample_& out, String& lastReason, uint8_t attempts) {
    for (uint8_t i = 0; i < attempts; i++) {
        String reason;
        if (readSampleOnce_(out, reason)) {
            return true;
        }
        lastReason = reason;
        // Tiny pause helps if the issue is a transient edge/noise on CS/CLK.
        delayMicroseconds(50);
    }
    if (lastReason.isEmpty()) {
        lastReason = "sample read failed";
    }
    return false;
}

