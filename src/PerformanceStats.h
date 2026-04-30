#ifndef PERFORMANCESTATS_H
#define PERFORMANCESTATS_H

#include <Arduino.h>

// Performance monitoring structure - shared between MainController and WebServerManager
struct PerformanceStats {
    unsigned long minTime = UINT32_MAX;   // Minimum execution time (ms)
    unsigned long maxTime = 0;           // Maximum execution time (ms)
    unsigned long totalTime = 0;         // Accumulated time (ms)
    unsigned long callCount = 0;         // Internal counter (not displayed)

    // Calculate average in milliseconds
    unsigned long getAverage() const {
        return callCount > 0 ? totalTime / callCount : 0;
    }

    // Reset statistics
    void reset() {
        minTime = UINT32_MAX;
        maxTime = 0;
        totalTime = 0;
        callCount = 0;
    }
};

// File logger performance stats - monitor file I/O operations that could cause delays
struct FileLoggerStats {
    PerformanceStats saveTime;      // File save operations (most frequent)
};

// Specific file logger performance structures
struct RelayLoggerStats {
    FileLoggerStats base;  // Inherit common file logger stats
};


#endif // PERFORMANCESTATS_H