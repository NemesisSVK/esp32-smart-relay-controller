#ifndef RGB_LED_MANAGER_H
#define RGB_LED_MANAGER_H

#include <Adafruit_NeoPixel.h>

enum LedStatus {
    STATUS_OFF,       // LED off / black
    STATUS_RED,       // Critical error (WiFi disconnected)
    STATUS_YELLOW,    // MQTT disconnected (WiFi OK)
    STATUS_GREEN,     // All operational
    STATUS_MAGENTA,   // Sensor errors
    STATUS_BLUE,      // Initializing
    STATUS_CYAN,      // Scanning
    STATUS_WHITE      // All offline
};

class RGBLedManager {
private:
    Adafruit_NeoPixel* pixel;
    LedStatus currentStatus;
    bool hasShown;
    void setColor(uint32_t color);

public:
    // Delete copy operations to prevent double deletion
    RGBLedManager(const RGBLedManager&) = delete;
    RGBLedManager& operator=(const RGBLedManager&) = delete;
    
    // Make constructor explicit to prevent implicit conversions
    explicit RGBLedManager(int pin = 48, int numPixels = 1);
    ~RGBLedManager();
    
    void begin();
    void setStatus(LedStatus status);
};

#endif
