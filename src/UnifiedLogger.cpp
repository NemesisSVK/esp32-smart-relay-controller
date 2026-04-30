#include "UnifiedLogger.h"
#include "DebugLogBuffer.h"
#include <cstdio>
#include <cstring>

namespace {
constexpr size_t kUnifiedLogBufferSize = 512;

size_t writeDirect(const char* text) {
    if (!text) {
        return 0;
    }
    const size_t len = strlen(text);
    if (len == 0) {
        return 0;
    }

    if (DebugLogBuffer::shouldCaptureFast()) {
        DebugLogBuffer::append(text, len);
    }
    return Serial.print(text);
}

size_t writeFormatted(const char* format, va_list args) {
    if (!format) {
        return 0;
    }

    char buffer[kUnifiedLogBufferSize];
    int len = vsnprintf(buffer, sizeof(buffer), format, args);
    if (len <= 0) {
        return 0;
    }

    const size_t clippedLen = strnlen(buffer, sizeof(buffer));
    if (clippedLen > 0 && DebugLogBuffer::shouldCaptureFast()) {
        DebugLogBuffer::append(buffer, clippedLen);
    }
    Serial.print(buffer);
    return static_cast<size_t>(len);
}
} // namespace

ConfigManager* UnifiedLogger::configManagerPtr = nullptr;
bool UnifiedLogger::loggingEnabled = false;

void UnifiedLogger::begin(ConfigManager* configManager) {
    configManagerPtr = configManager;
    updateSettings();
}

void UnifiedLogger::updateSettings() {
    if (configManagerPtr && configManagerPtr->isConfigLoaded()) {
        loggingEnabled = configManagerPtr->getSettings().debug.logging_enabled;
    } else {
        loggingEnabled = false;
    }
}

bool UnifiedLogger::isEnabled() {
    return loggingEnabled;
}

const char* UnifiedLogger::levelTag(Level level) {
    switch (level) {
        case Level::Error:
            return "[ERROR] ";
        case Level::Warning:
            return "[WARNING] ";
        case Level::Info:
            return "[INFO] ";
        case Level::Verbose:
            return "[VERBOSE] ";
        default:
            return "";
    }
}

size_t UnifiedLogger::vRaw(const char* format, va_list args) {
    if (!loggingEnabled) {
        return 0;
    }
    return writeFormatted(format, args);
}

size_t UnifiedLogger::raw(const char* format, ...) {
    if (!loggingEnabled) {
        return 0;
    }

    va_list args;
    va_start(args, format);
    const size_t written = vRaw(format, args);
    va_end(args);
    return written;
}

size_t UnifiedLogger::vLog(Level level, const char* format, va_list args) {
    if (!loggingEnabled) {
        return 0;
    }

    size_t written = 0;
    const char* prefix = levelTag(level);
    if (prefix && prefix[0] != '\0') {
        written += writeDirect(prefix);
    }
    written += writeFormatted(format, args);
    return written;
}

size_t UnifiedLogger::log(Level level, const char* format, ...) {
    if (!loggingEnabled) {
        return 0;
    }

    va_list args;
    va_start(args, format);
    const size_t written = vLog(level, format, args);
    va_end(args);
    return written;
}

size_t UnifiedLogger::error(const char* format, ...) {
    if (!loggingEnabled) {
        return 0;
    }
    va_list args;
    va_start(args, format);
    const size_t written = vLog(Level::Error, format, args);
    va_end(args);
    return written;
}

size_t UnifiedLogger::warning(const char* format, ...) {
    if (!loggingEnabled) {
        return 0;
    }
    va_list args;
    va_start(args, format);
    const size_t written = vLog(Level::Warning, format, args);
    va_end(args);
    return written;
}

size_t UnifiedLogger::info(const char* format, ...) {
    if (!loggingEnabled) {
        return 0;
    }
    va_list args;
    va_start(args, format);
    const size_t written = vLog(Level::Info, format, args);
    va_end(args);
    return written;
}

size_t UnifiedLogger::verbose(const char* format, ...) {
    if (!loggingEnabled) {
        return 0;
    }
    va_list args;
    va_start(args, format);
    const size_t written = vLog(Level::Verbose, format, args);
    va_end(args);
    return written;
}
