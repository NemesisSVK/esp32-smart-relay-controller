#ifndef UNIFIEDLOGGER_H
#define UNIFIEDLOGGER_H

#include <Arduino.h>
#include <cstdarg>
#include "ConfigManager.h"

// Unified runtime serial logger with severity support.
class UnifiedLogger {
public:
    enum class Level : uint8_t {
        Error = 0,
        Warning,
        Info,
        Verbose
    };

    // Initialize with config manager reference.
    static void begin(ConfigManager* configManager);

    // Refresh runtime setting from configuration.
    static void updateSettings();

    // Check whether serial logging output is enabled.
    static bool isEnabled();

    // Severity-based logging (prefixes severity tag automatically).
    static size_t log(Level level, const char* format, ...);
    static size_t vLog(Level level, const char* format, va_list args);

    static size_t error(const char* format, ...);
    static size_t warning(const char* format, ...);
    static size_t info(const char* format, ...);
    static size_t verbose(const char* format, ...);

    // Raw passthrough logging without automatic severity prefix.
    static size_t raw(const char* format, ...);
    static size_t vRaw(const char* format, va_list args);

private:
    static ConfigManager* configManagerPtr;
    static bool loggingEnabled;

    static const char* levelTag(Level level);
};

#endif // UNIFIEDLOGGER_H
