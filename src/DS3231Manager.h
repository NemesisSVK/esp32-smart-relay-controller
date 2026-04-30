#ifndef DS3231MANAGER_H
#define DS3231MANAGER_H

#include <Arduino.h>
#include <Wire.h>
#include <RTClib.h>

// DS3231 RTC manager
// - Uses a dedicated I2C controller (TwoWire port 1) by default
// - Supports probing presence, lost-power detection, reading and setting time
class DS3231Manager {
public:
    DS3231Manager();

    // Initialize DS3231 on the given I2C pins.
    // address is expected to be 0x68 for DS3231 modules.
    bool begin(int sdaPin, int sclPin, uint8_t address = 0x68, bool storeUtc = false);

    bool isInitialized() const { return initialized; }
    bool isPresent() const { return present; }
    bool hasLostPower() const { return lostPower; }

    // Clear the DS3231 Oscillator Stop Flag (OSF) so lostPower becomes false.
    // Call this after setting RTC time from a trusted source (e.g., NTP).
    bool clearLostPowerFlag();

    uint8_t getI2CAddress() const { return i2cAddress; }
    int getSdaPin() const { return sdaPin; }
    int getSclPin() const { return sclPin; }
    bool isStoreUtc() const { return storeUtc; }

    // Read current datetime from the RTC.
    // Returns false if RTC isn't initialized/present or read fails.
    bool readNow(DateTime& outDateTime) const;

    // Convenience: returns rtc.now() or 2000-01-01 if unavailable.
    DateTime nowOrDefault() const;

    // Set RTC datetime.
    bool setDateTime(const DateTime& dt);

    // Convert an RTC DateTime to Unix epoch seconds, based on current configuration.
    // - If storeUtc=true, DateTime is treated as UTC (dt.unixtime()).
    // - If storeUtc=false, DateTime is treated as LOCAL time and converted using mktime() (TZ must be configured).
    time_t toEpoch(const DateTime& dt) const;

    // Set RTC from Unix epoch seconds.
    // - If storeUtc=true, epoch is written as UTC.
    // - If storeUtc=false, epoch is converted to LOCAL time first (TZ must be configured).
    bool setFromEpoch(time_t epoch);

    // Simple plausibility check for RTC values.
    static bool isDateTimePlausible(const DateTime& dt);

private:
    bool probeI2C(uint8_t address);

private:
    // Dedicated I2C controller (port 1) to avoid pin conflicts with global Wire.
    // Marked mutable so we can expose const read APIs (I2C transactions are logically read-only).
    mutable TwoWire rtcWire;
    mutable RTC_DS3231 rtc;

    bool initialized;
    bool present;
    bool lostPower;
    bool storeUtc;

    int sdaPin;
    int sclPin;
    uint8_t i2cAddress;
};

#endif // DS3231MANAGER_H

