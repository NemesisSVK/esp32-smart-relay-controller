#ifndef LOGGINGPERFORMANCETEST_H
#define LOGGINGPERFORMANCETEST_H

#include <Arduino.h>
#include "MainController.h"
#include "RelayManager.h"
#include "ErrorHandler.h"
#include "PerformanceStats.h"

// Test configuration
#define HIGH_FREQUENCY_TEST_DURATION_MS 10000  // 10 seconds
#define RELAY_TOGGLE_RATE_HZ 15                // 15 toggles per second
#define ERROR_GENERATION_RATE_HZ 10            // 10 errors per second

// Performance monitoring structure for tests
struct TestPerformanceMetrics {
    unsigned long testStartTime;
    unsigned long testEndTime;
    unsigned long relayOperationsCount;
    unsigned long errorOperationsCount;
    unsigned long initialRelayLoggerCalls;
    unsigned long finalRelayLoggerCalls;
    unsigned long initialErrorHandlerCalls;
    unsigned long finalErrorHandlerCalls;
    PerformanceStats relayLoggerStatsBefore;
    PerformanceStats relayLoggerStatsAfter;
    PerformanceStats errorHandlerStatsBefore;
    PerformanceStats errorHandlerStatsAfter;
};

class LoggingPerformanceTest {
public:
    // Initialize test with required managers
    static void setManagers(MainController* mc, RelayManager* rm);

    // Main test execution
    static bool runHighFrequencyLoggingTest();

    // Individual test components
    static bool testRapidRelayToggling(unsigned long durationMs, int toggleRateHz);
    static bool testFrequentErrorGeneration(unsigned long durationMs, int errorRateHz);
    static bool testCombinedHighFrequencyScenario(unsigned long durationMs, int relayRateHz, int errorRateHz);

    // Performance monitoring
    static void capturePerformanceMetricsBefore(TestPerformanceMetrics& metrics);
    static void capturePerformanceMetricsAfter(TestPerformanceMetrics& metrics);
    static void printPerformanceResults(const TestPerformanceMetrics& metrics);

    // Data integrity verification
    static bool verifyDataIntegrity();
    static bool verifyPeriodicFlush();

    // Utility methods
    static void resetTestState();
    static String generateTestReport();

private:
    static MainController* mainController;
    static RelayManager* relayManager;

    // Test state tracking
    static unsigned long testRelayOperations;
    static unsigned long testErrorOperations;
    static bool testInProgress;

    // Helper methods
    static void simulateRelayToggle();
    static void simulateErrorGeneration();
    static unsigned long calculateExpectedOperations(unsigned long durationMs, int rateHz);
};

#endif // LOGGINGPERFORMANCETEST_H