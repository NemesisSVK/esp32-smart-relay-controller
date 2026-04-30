#ifndef WIFIMANAGER_H
#define WIFIMANAGER_H

#include <WiFi.h>

class WiFiManager {
public:
    WiFiManager();

    // Initialize WiFi connection
    void setupWiFi(const String& ssid, const String& password);

    // Check WiFi connection and attempt reconnection if needed
    void checkWiFiConnection();

    // Check if WiFi is currently connected
    bool isConnected() const;

    // Get retry count for monitoring
    static int getRetryCount() { return wifi_retry_count; }

private:
    String wifi_ssid;
    String wifi_password;

    // Non-blocking connection attempt state
    bool connecting = false;
    unsigned long connectAttemptStart = 0;
    static const unsigned long connect_attempt_timeout_ms = 10000; // ~10s

    static unsigned long last_wifi_check;
    static int wifi_retry_count;
    static const unsigned long wifi_check_interval;
};

#endif // WIFIMANAGER_H
