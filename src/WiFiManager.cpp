#include "WiFiManager.h"
#include <Arduino.h>
#include "ErrorHandler.h"
#include "UnifiedLogger.h"

// Initialize static members
unsigned long WiFiManager::last_wifi_check = 0;
int WiFiManager::wifi_retry_count = 0;
const unsigned long WiFiManager::wifi_check_interval = 30000; // 30 seconds

WiFiManager::WiFiManager() {
    // Constructor - initialize WiFi if needed
}

void WiFiManager::setupWiFi(const String& ssid, const String& password) {
    // Store credentials for reconnection
    wifi_ssid = ssid;
    wifi_password = password;

    delay(10);
    UnifiedLogger::info("Initializing WiFi connection to %s\n", ssid.c_str());

    // Stability tweak (optional): disabling WiFi modem sleep can reduce intermittent TCP/MQTT dropouts,
    // but may increase chip temperature/power consumption. Disabled for now during investigation.
    // WiFi.setSleep(false);

    // Start WiFi connection (non-blocking)
    WiFi.begin(ssid.c_str(), password.c_str());

    // Check initial status
    delay(100); // Small delay to let WiFi start
    wl_status_t status = WiFi.status();
    UnifiedLogger::info("Initial WiFi status: %d\n", status);

    // Note: Actual connection will be checked in checkWiFiConnection() called from loop
    UnifiedLogger::info("WiFi initialization complete - connection will be established in main loop\n");
}

void WiFiManager::checkWiFiConnection() {
    static bool wasConnected = true; // Assume connected at startup

    if (WiFi.status() != WL_CONNECTED) {
        wasConnected = false; // Mark as disconnected
        unsigned long now = millis();

        // If we have an in-progress attempt, check for timeout (non-blocking)
        if (connecting) {
            if (now - connectAttemptStart >= connect_attempt_timeout_ms) {
                UnifiedLogger::warning("WiFi reconnection attempt timed out (status: %d)\n", (int)WiFi.status());
                connecting = false;
                ErrorHandler::reportError(ErrorCode::WIFI_CONNECTION_LOST, ErrorSeverity::WARNING,
                                        "WiFi connection lost - retrying");
            }
            return;
        }

        // Only start a new attempt every 30 seconds to avoid spam
        if (now - last_wifi_check > wifi_check_interval || last_wifi_check == 0) {
            last_wifi_check = now;

            // Keep trying indefinitely.
            wifi_retry_count++;

            UnifiedLogger::warning("WiFi disconnected, attempting to reconnect...\n");
            UnifiedLogger::info("Attempting WiFi connection to SSID: '%s'\n", wifi_ssid.c_str());

            // Reset WiFi state quickly without blocking the main loop
            if (WiFi.status() != WL_IDLE_STATUS) {
                WiFi.disconnect(true);
            }

            WiFi.begin(wifi_ssid.c_str(), wifi_password.c_str());
            connecting = true;
            connectAttemptStart = now;
        }
    } else {
        // WiFi is connected, reset retry counter
        if (!wasConnected) {
            UnifiedLogger::info("[WIFI] Connection state changed to CONNECTED\n");
            wifi_retry_count = 0;
            connecting = false;
            wasConnected = true;
        }
    }
}

bool WiFiManager::isConnected() const {
    return WiFi.status() == WL_CONNECTED;
}
