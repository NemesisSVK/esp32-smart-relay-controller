#include "DS3231Manager.h"

// time_t / localtime_r / mktime
#include <time.h>

DS3231Manager::DS3231Manager() :
    rtcWire(1),
    initialized(false),
    present(false),
    lostPower(false),
    storeUtc(false),
    sdaPin(-1),
    sclPin(-1),
    i2cAddress(0x68) {
}

bool DS3231Manager::begin(int sdaPinIn, int sclPinIn, uint8_t address, bool storeUtcIn) {
    sdaPin = sdaPinIn;
    sclPin = sclPinIn;
    i2cAddress = address;
    storeUtc = storeUtcIn;

    initialized = false;
    present = false;
    lostPower = false;

    // Initialize dedicated I2C bus.
    // Note: On ESP32, TwoWire::begin(sda, scl) configures the pins.
    rtcWire.begin(sdaPin, sclPin);

    // Probe the configured address first.
    if (!probeI2C(i2cAddress)) {
        present = false;
        return false;
    }

    present = true;

    // Initialize the DS3231 driver.
    // RTClib defaults to 0x68 internally. We still store/probe address for diagnostics/config validation.
    if (!rtc.begin(&rtcWire)) {
        present = false;
        return false;
    }

    lostPower = rtc.lostPower();
    initialized = true;
    return true;
}

bool DS3231Manager::probeI2C(uint8_t address) {
    rtcWire.beginTransmission(address);
    uint8_t err = rtcWire.endTransmission();
    return err == 0;
}

bool DS3231Manager::readNow(DateTime& outDateTime) const {
    if (!initialized || !present) {
        return false;
    }

    outDateTime = rtc.now();
    return true;
}

DateTime DS3231Manager::nowOrDefault() const {
    if (!initialized || !present) {
        return DateTime(2000, 1, 1, 0, 0, 0);
    }
    return rtc.now();
}

bool DS3231Manager::setDateTime(const DateTime& dt) {
    if (!initialized || !present) {
        return false;
    }

    rtc.adjust(dt);
    lostPower = rtc.lostPower();
    return true;
}

bool DS3231Manager::clearLostPowerFlag() {
    if (!initialized || !present) {
        return false;
    }

    // DS3231 status register (0x0F), bit 7 = OSF (Oscillator Stop Flag)
    // RTClib exposes lostPower() but does not always clear OSF on adjust().
    // Clearing OSF after a trusted time set prevents permanent lostPower=true.
    rtcWire.beginTransmission(i2cAddress);
    rtcWire.write((uint8_t)0x0F);
    if (rtcWire.endTransmission(false) != 0) { // repeated start
        return false;
    }

    uint8_t n = rtcWire.requestFrom((int)i2cAddress, 1);
    if (n != 1) {
        return false;
    }

    uint8_t status = rtcWire.read();
    status &= ~(1 << 7); // clear OSF

    rtcWire.beginTransmission(i2cAddress);
    rtcWire.write((uint8_t)0x0F);
    rtcWire.write(status);
    if (rtcWire.endTransmission() != 0) {
        return false;
    }

    // Refresh cached value
    lostPower = rtc.lostPower();
    return true;
}

time_t DS3231Manager::toEpoch(const DateTime& dt) const {
    if (storeUtc) {
        // RTClib DateTime is a civil time representation; unixtime() returns seconds since 1970-01-01 UTC.
        return static_cast<time_t>(dt.unixtime());
    }

    // Interpret dt as LOCAL time and convert to epoch using the current TZ rules.
    // IMPORTANT: ensure TZ is configured via setenv(TZ)/tzset() or configTzTime() before calling.
    struct tm tmLocal;
    tmLocal.tm_year = dt.year() - 1900;
    tmLocal.tm_mon = dt.month() - 1;
    tmLocal.tm_mday = dt.day();
    tmLocal.tm_hour = dt.hour();
    tmLocal.tm_min = dt.minute();
    tmLocal.tm_sec = dt.second();
    tmLocal.tm_isdst = -1; // let libc determine DST
    tmLocal.tm_wday = 0;
    tmLocal.tm_yday = 0;

    return mktime(&tmLocal);
}

bool DS3231Manager::setFromEpoch(time_t epoch) {
    if (!initialized || !present) {
        return false;
    }

    if (epoch <= 0) {
        return false;
    }

    if (storeUtc) {
        rtc.adjust(DateTime(static_cast<uint32_t>(epoch)));
        lostPower = rtc.lostPower();
        return true;
    }

    // Convert epoch to LOCAL time, then write that civil time into the RTC.
    // IMPORTANT: ensure TZ is configured.
    struct tm tmLocal;
    if (!localtime_r(&epoch, &tmLocal)) {
        return false;
    }

    DateTime dt(
        tmLocal.tm_year + 1900,
        tmLocal.tm_mon + 1,
        tmLocal.tm_mday,
        tmLocal.tm_hour,
        tmLocal.tm_min,
        tmLocal.tm_sec
    );

    rtc.adjust(dt);
    lostPower = rtc.lostPower();
    return true;
}

bool DS3231Manager::isDateTimePlausible(const DateTime& dt) {
    // Basic sanity bounds. Adjust if your product needs a different acceptable range.
    if (dt.year() < 2020 || dt.year() > 2100) return false;
    if (dt.month() < 1 || dt.month() > 12) return false;
    if (dt.day() < 1 || dt.day() > 31) return false;
    if (dt.hour() > 23) return false;
    if (dt.minute() > 59) return false;
    if (dt.second() > 59) return false;
    return true;
}

