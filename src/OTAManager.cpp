#include "OTAManager.h"

#include <ArduinoJson.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <ArduinoOTA.h>
#include "UnifiedLogger.h"
#include "PsramJsonAllocator.h"

OTAManager::OTAManager()
    : configValid(false),
      armed(false),
      armedUntilMs(0),
      started(false),
      inProgress(false),
      progressPercent(0),
      lastCommand(-1),
      lastLoggedProgressDecile(-1) {}

void OTAManager::setHostname(const String& newHostname) {
    hostname = newHostname;
}

void OTAManager::refreshConfigStatus() {
    String err;
    (void)loadConfig(err);
}

bool OTAManager::loadConfig(String& errorMessage) {
    configValid = false;
    configError = "";

    if (!LittleFS.begin()) {
        errorMessage = "LittleFS not mounted";
        configError = errorMessage;
        UnifiedLogger::warning("[OTA] Config load failed: %s\n", errorMessage.c_str());
        return false;
    }

    if (!LittleFS.exists("/ota.json")) {
        errorMessage = "/ota.json not found";
        configError = errorMessage;
        UnifiedLogger::warning("[OTA] Config load failed: %s\n", errorMessage.c_str());
        return false;
    }

    File f = LittleFS.open("/ota.json", "r");
    if (!f) {
        errorMessage = "Failed to open /ota.json";
        configError = errorMessage;
        UnifiedLogger::error("[OTA] Config load failed: %s\n", errorMessage.c_str());
        return false;
    }

    PsramJsonAllocator jsonAllocator;
    JsonDocument doc(&jsonAllocator);
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (err) {
        errorMessage = String("Invalid /ota.json: ") + err.c_str();
        configError = errorMessage;
        UnifiedLogger::error("[OTA] Config load failed: %s\n", errorMessage.c_str());
        return false;
    }

    JsonObjectConst otaObj = doc["ota"];
    if (otaObj.isNull()) {
        errorMessage = "/ota.json missing 'ota' object";
        configError = errorMessage;
        UnifiedLogger::error("[OTA] Config load failed: %s\n", errorMessage.c_str());
        return false;
    }

    String password = otaObj["password"] | "";
    int port = otaObj["port"] | 3232;
    int windowSeconds = otaObj["window_seconds"] | 600;

    password.trim();
    if (password.length() == 0) {
        errorMessage = "ota.password must be non-empty";
        configError = errorMessage;
        UnifiedLogger::warning("[OTA] Config validation failed: %s\n", errorMessage.c_str());
        return false;
    }
    if (port < 1 || port > 65535) {
        errorMessage = "ota.port must be 1..65535";
        configError = errorMessage;
        UnifiedLogger::warning("[OTA] Config validation failed: %s\n", errorMessage.c_str());
        return false;
    }
    if (windowSeconds < 10 || windowSeconds > 3600) {
        errorMessage = "ota.window_seconds must be 10..3600";
        configError = errorMessage;
        UnifiedLogger::warning("[OTA] Config validation failed: %s\n", errorMessage.c_str());
        return false;
    }

    cfg.password = password;
    cfg.port = port;
    cfg.windowSeconds = windowSeconds;

    configValid = true;
    configError = "";
    UnifiedLogger::info("[OTA] Config loaded (port=%d, window=%ds)\n", cfg.port, cfg.windowSeconds);
    return true;
}

void OTAManager::ensureArduinoOtaStarted() {
    if (started) {
        return;
    }
    if (WiFi.status() != WL_CONNECTED) {
        return;
    }
    if (!configValid) {
        return;
    }

    if (hostname.length() > 0) {
        ArduinoOTA.setHostname(hostname.c_str());
    }
    ArduinoOTA.setPort(static_cast<uint16_t>(cfg.port));
    ArduinoOTA.setPassword(cfg.password.c_str());
    ArduinoOTA.setRebootOnSuccess(false);

    ArduinoOTA.onStart([this]() {
        inProgress = true;
        lastError = "";
        progressPercent = 0;
        lastLoggedProgressDecile = -1;

        lastCommand = ArduinoOTA.getCommand();
        if (lastCommand == U_FLASH) {
            command = "firmware";
        } else if (lastCommand == U_SPIFFS) {
            command = "filesystem";
        } else {
            command = "";
        }

        addLogLine(String("[OTA] Start (") + (command.length() ? command : "unknown") + ")");
        UnifiedLogger::info("[OTA] Start (%s)\n", command.length() ? command.c_str() : "unknown");
    });

    ArduinoOTA.onEnd([this]() {
        inProgress = false;
        progressPercent = 100;
        addLogLine("[OTA] End");
        UnifiedLogger::info("[OTA] Transfer complete\n");

        // We intentionally reboot only after filesystem upload so PlatformIO can
        // run `upload` then `uploadfs` in one go (USB-like flow).
        if (lastCommand == U_SPIFFS) {
            addLogLine("[OTA] Rebooting after filesystem update");
            UnifiedLogger::warning("[OTA] Rebooting after filesystem update\n");
            delay(150);
            ESP.restart();
        }
    });

    ArduinoOTA.onProgress([this](unsigned int progress, unsigned int total) {
        if (total == 0) {
            return;
        }
        const int pct = static_cast<int>((progress * 100UL) / total);
        progressPercent = pct;

        // Log every 10% to keep memory/serial noise down
        const int decile = pct / 10;
        if (decile != lastLoggedProgressDecile) {
            lastLoggedProgressDecile = decile;
            addLogLine(String("[OTA] Progress ") + String(decile * 10) + "%");
        }
    });

    ArduinoOTA.onError([this](ota_error_t error) {
        inProgress = false;
        String msg = "[OTA] Error ";
        msg += String(static_cast<int>(error));

        switch (error) {
            case OTA_AUTH_ERROR: msg += " (auth)"; break;
            case OTA_BEGIN_ERROR: msg += " (begin)"; break;
            case OTA_CONNECT_ERROR: msg += " (connect)"; break;
            case OTA_RECEIVE_ERROR: msg += " (receive)"; break;
            case OTA_END_ERROR: msg += " (end)"; break;
            default: break;
        }

        lastError = msg;
        addLogLine(msg);
        UnifiedLogger::warning("%s\n", msg.c_str());
    });

    ArduinoOTA.begin();
    started = true;
    addLogLine(String("[OTA] Ready on port ") + String(cfg.port));
}

bool OTAManager::armFromWeb(String& errorMessage) {
    errorMessage = "";

    String cfgErr;
    if (!loadConfig(cfgErr)) {
        errorMessage = cfgErr;
        UnifiedLogger::warning("[OTA] Arm failed: %s\n", errorMessage.c_str());
        return false;
    }

    const unsigned long now = millis();
    armedUntilMs = now + (static_cast<unsigned long>(cfg.windowSeconds) * 1000UL);
    armed = true;

    addLogLine(String("[OTA] Armed for ") + String(cfg.windowSeconds) + "s");
    UnifiedLogger::info("[OTA] Armed for %ds\n", cfg.windowSeconds);
    ensureArduinoOtaStarted();
    return true;
}

void OTAManager::loop() {
    if (!armed) {
        return;
    }
    const unsigned long now = millis();
    // Never expire the window while an OTA transfer is in progress.
    if (!inProgress && armedUntilMs != 0 && static_cast<long>(armedUntilMs - now) <= 0) {
        armed = false;
        addLogLine("[OTA] Window expired");
        UnifiedLogger::info("[OTA] Window expired\n");
        return;
    }

    ensureArduinoOtaStarted();
    if (!started) {
        return;
    }

    ArduinoOTA.handle();
}

OtaStatusSnapshot OTAManager::snapshot() const {
    OtaStatusSnapshot s;
    s.configValid = configValid;
    s.configError = configError;
    s.port = cfg.port;
    s.windowSeconds = cfg.windowSeconds;
    s.armed = armed;
    s.inProgress = inProgress;
    s.progressPercent = progressPercent;
    s.command = command;
    s.lastError = lastError;

    if (armed) {
        const unsigned long now = millis();
        if (armedUntilMs > now) {
            s.remainingSeconds = (armedUntilMs - now) / 1000UL;
        } else {
            s.remainingSeconds = 0;
        }
    }
    return s;
}

String OTAManager::getLogText() const {
    String out;
    for (const auto& line : logLines) {
        out += line;
        out += "\n";
    }
    return out;
}

void OTAManager::addLogLine(const String& line) {
    // Keep a small ring buffer
    if (logLines.size() >= MAX_LOG_LINES) {
        logLines.erase(logLines.begin());
    }
    logLines.push_back(line);
}
