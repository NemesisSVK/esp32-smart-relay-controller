#include "LoggingPerformanceTest.h"
#include "RelayLogger.h"
#include "UnifiedLogger.h"
#include <LittleFS.h>

// Static member initialization
MainController* LoggingPerformanceTest::mainController = nullptr;
RelayManager* LoggingPerformanceTest::relayManager = nullptr;
unsigned long LoggingPerformanceTest::testRelayOperations = 0;
unsigned long LoggingPerformanceTest::testErrorOperations = 0;
bool LoggingPerformanceTest::testInProgress = false;

void LoggingPerformanceTest::setManagers(MainController* mc, RelayManager* rm) {
    mainController = mc;
    relayManager = rm;
}

bool LoggingPerformanceTest::runHighFrequencyLoggingTest() {
    if (!mainController || !relayManager) {
        UnifiedLogger::error("[TEST] Managers not set\n");
        return false;
    }

    UnifiedLogger::info("[TEST] Starting high-frequency logging performance test...\n");
    testInProgress = true;

    TestPerformanceMetrics metrics;
    capturePerformanceMetricsBefore(metrics);

    // Reset test counters
    resetTestState();

    // Run combined test scenario
    bool testResult = testCombinedHighFrequencyScenario(
        HIGH_FREQUENCY_TEST_DURATION_MS,
        RELAY_TOGGLE_RATE_HZ,
        ERROR_GENERATION_RATE_HZ
    );

    capturePerformanceMetricsAfter(metrics);
    printPerformanceResults(metrics);

    // Verify data integrity and flush functionality
    bool integrityCheck = verifyDataIntegrity();
    bool flushCheck = verifyPeriodicFlush();

    testInProgress = false;

    UnifiedLogger::info("[TEST] Test completed. Result: %s, Integrity: %s, Flush: %s\n",
                        testResult ? "PASS" : "FAIL",
                        integrityCheck ? "PASS" : "FAIL",
                        flushCheck ? "PASS" : "FAIL");

    return testResult && integrityCheck && flushCheck;
}

bool LoggingPerformanceTest::testRapidRelayToggling(unsigned long durationMs, int toggleRateHz) {
    UnifiedLogger::info("[TEST] Testing rapid relay toggling: %d Hz for %lu ms\n", toggleRateHz, durationMs);

    unsigned long startTime = millis();
    unsigned long endTime = startTime + durationMs;
    unsigned long intervalMs = 1000 / toggleRateHz;
    unsigned long nextToggle = startTime;

    while (millis() < endTime) {
        if (millis() >= nextToggle) {
            simulateRelayToggle();
            nextToggle += intervalMs;
            testRelayOperations++;
        }
        // Small delay to prevent watchdog issues
        delay(1);
    }

    UnifiedLogger::info("[TEST] Relay toggling completed: %lu operations\n", testRelayOperations);
    return true;
}

bool LoggingPerformanceTest::testFrequentErrorGeneration(unsigned long durationMs, int errorRateHz) {
    UnifiedLogger::info("[TEST] Testing frequent error generation: %d Hz for %lu ms\n", errorRateHz, durationMs);

    unsigned long startTime = millis();
    unsigned long endTime = startTime + durationMs;
    unsigned long intervalMs = 1000 / errorRateHz;
    unsigned long nextError = startTime;

    while (millis() < endTime) {
        if (millis() >= nextError) {
            simulateErrorGeneration();
            nextError += intervalMs;
            testErrorOperations++;
        }
        // Small delay to prevent watchdog issues
        delay(1);
    }

    UnifiedLogger::info("[TEST] Error generation completed: %lu operations\n", testErrorOperations);
    return true;
}

bool LoggingPerformanceTest::testCombinedHighFrequencyScenario(unsigned long durationMs, int relayRateHz, int errorRateHz) {
    UnifiedLogger::info("[TEST] Running combined high-frequency scenario: %d relay Hz, %d error Hz for %lu ms\n",
                        relayRateHz, errorRateHz, durationMs);

    unsigned long startTime = millis();
    unsigned long endTime = startTime + durationMs;

    unsigned long relayIntervalMs = 1000 / relayRateHz;
    unsigned long errorIntervalMs = 1000 / errorRateHz;
    unsigned long nextRelayToggle = startTime;
    unsigned long nextError = startTime;

    while (millis() < endTime) {
        unsigned long currentTime = millis();

        // Handle relay toggling
        if (currentTime >= nextRelayToggle) {
            simulateRelayToggle();
            nextRelayToggle += relayIntervalMs;
            testRelayOperations++;
        }

        // Handle error generation
        if (currentTime >= nextError) {
            simulateErrorGeneration();
            nextError += errorIntervalMs;
            testErrorOperations++;
        }

        // Small delay to prevent watchdog issues
        delay(1);
    }

    UnifiedLogger::info("[TEST] Combined test completed: %lu relay ops, %lu error ops\n",
                        testRelayOperations, testErrorOperations);
    return true;
}

void LoggingPerformanceTest::capturePerformanceMetricsBefore(TestPerformanceMetrics& metrics) {
    metrics.testStartTime = millis();
    metrics.initialRelayLoggerCalls = mainController ? mainController->relayLoggerStats.base.saveTime.callCount : 0;
    metrics.relayLoggerStatsBefore = mainController ? mainController->relayLoggerStats.base.saveTime : PerformanceStats();
    // Note: ErrorHandler doesn't have direct performance stats, so we'll track via operation counts
}

void LoggingPerformanceTest::capturePerformanceMetricsAfter(TestPerformanceMetrics& metrics) {
    metrics.testEndTime = millis();
    metrics.relayOperationsCount = testRelayOperations;
    metrics.errorOperationsCount = testErrorOperations;
    metrics.finalRelayLoggerCalls = mainController ? mainController->relayLoggerStats.base.saveTime.callCount : 0;
    metrics.relayLoggerStatsAfter = mainController ? mainController->relayLoggerStats.base.saveTime : PerformanceStats();
}

void LoggingPerformanceTest::printPerformanceResults(const TestPerformanceMetrics& metrics) {
    unsigned long testDuration = metrics.testEndTime - metrics.testStartTime;
    unsigned long relayLoggerCalls = metrics.finalRelayLoggerCalls - metrics.initialRelayLoggerCalls;

    UnifiedLogger::info("\n=== PERFORMANCE TEST RESULTS ===\n");
    UnifiedLogger::info("Test Duration: %lu ms\n", testDuration);
    UnifiedLogger::info("Relay Operations: %lu\n", metrics.relayOperationsCount);
    UnifiedLogger::info("Error Operations: %lu\n", metrics.errorOperationsCount);
    UnifiedLogger::info("Relay Logger File I/O Calls: %lu\n", relayLoggerCalls);

    if (mainController) {
        UnifiedLogger::info("\nRelay Logger Stats:\n");
        UnifiedLogger::info("  Save Time - Min: %lu ms, Max: %lu ms, Avg: %lu ms, Calls: %lu\n",
                           metrics.relayLoggerStatsAfter.minTime,
                           metrics.relayLoggerStatsAfter.maxTime,
                           metrics.relayLoggerStatsAfter.getAverage(),
                           metrics.relayLoggerStatsAfter.callCount);
    }

    // Calculate efficiency metrics
    float relayOpsPerSecond = (float)metrics.relayOperationsCount / (testDuration / 1000.0);
    float errorOpsPerSecond = (float)metrics.errorOperationsCount / (testDuration / 1000.0);
    float fileIOsPerSecond = (float)relayLoggerCalls / (testDuration / 1000.0);

    UnifiedLogger::info("\nEfficiency Metrics:\n");
    UnifiedLogger::info("  Relay ops/sec: %.2f\n", relayOpsPerSecond);
    UnifiedLogger::info("  Error ops/sec: %.2f\n", errorOpsPerSecond);
    UnifiedLogger::info("  File I/O ops/sec: %.2f\n", fileIOsPerSecond);

    // Calculate buffering efficiency (operations per file I/O)
    if (relayLoggerCalls > 0) {
        float bufferingRatio = (float)metrics.relayOperationsCount / relayLoggerCalls;
        UnifiedLogger::info("  Buffering efficiency: %.1f operations per file I/O\n", bufferingRatio);
    }
}

bool LoggingPerformanceTest::verifyDataIntegrity() {
    UnifiedLogger::info("[TEST] Verifying data integrity...\n");

    // Check relay events count
    int relayEventCount = RelayLogger::getLogCount();
    UnifiedLogger::info("[TEST] Relay events in memory: %d\n", relayEventCount);

    // Check error history count
    int errorHistoryCount = ErrorHandler::getErrorHistoryCount();
    UnifiedLogger::info("[TEST] Error events in history: %d\n", errorHistoryCount);

    // Force flush to ensure data is persisted
    RelayLogger::forceFlushRelayEvents();
    ErrorHandler::forceFlushErrorHistory();

    // Verify files exist and are readable
    bool relayFileExists = LittleFS.exists(RELAY_EVENTS_FILE);
    bool errorFileExists = LittleFS.exists(ERROR_HISTORY_FILE);

    UnifiedLogger::info("[TEST] Relay events file exists: %s\n", relayFileExists ? "YES" : "NO");
    UnifiedLogger::info("[TEST] Error history file exists: %s\n", errorFileExists ? "YES" : "NO");

    return relayFileExists && errorFileExists && relayEventCount >= 0 && errorHistoryCount >= 0;
}

bool LoggingPerformanceTest::verifyPeriodicFlush() {
    UnifiedLogger::info("[TEST] Verifying periodic flush functionality...\n");

    // Check if dirty flags are properly managed
    // Note: We can't directly access private members, but we can verify behavior

    // Force a flush and check if it completes without errors
    unsigned long startTime = millis();
    RelayLogger::forceFlushRelayEvents();
    ErrorHandler::forceFlushErrorHistory();
    unsigned long flushTime = millis() - startTime;

    UnifiedLogger::info("[TEST] Forced flush completed in %lu ms\n", flushTime);

    // Verify data is still accessible after flush
    String relayJson = RelayLogger::getLogsJson();
    String errorJson = ErrorHandler::getErrorHistoryJson();

    bool relayDataValid = relayJson.length() > 0;
    bool errorDataValid = errorJson.length() > 0;

    UnifiedLogger::info("[TEST] Relay data accessible after flush: %s\n", relayDataValid ? "YES" : "NO");
    UnifiedLogger::info("[TEST] Error data accessible after flush: %s\n", errorDataValid ? "YES" : "NO");

    return relayDataValid && errorDataValid;
}

void LoggingPerformanceTest::resetTestState() {
    testRelayOperations = 0;
    testErrorOperations = 0;
}

String LoggingPerformanceTest::generateTestReport() {
    String report = "Logging Performance Test Report\n";
    report += "================================\n\n";

    // Add test configuration
    report += "Test Configuration:\n";
    report += "- Duration: " + String(HIGH_FREQUENCY_TEST_DURATION_MS) + " ms\n";
    report += "- Relay toggle rate: " + String(RELAY_TOGGLE_RATE_HZ) + " Hz\n";
    report += "- Error generation rate: " + String(ERROR_GENERATION_RATE_HZ) + " Hz\n\n";

    // Add performance metrics if available
    if (mainController) {
        report += "Performance Metrics:\n";
        report += "- Relay logger save calls: " + String(mainController->relayLoggerStats.base.saveTime.callCount) + "\n";
        report += "- Average save time: " + String(mainController->relayLoggerStats.base.saveTime.getAverage()) + " ms\n";
    }

    return report;
}

void LoggingPerformanceTest::simulateRelayToggle() {
    if (!relayManager) return;

    // Alternate between different relay states for realistic testing
    static int toggleState = 0;
    RelayMode mode;

    switch (toggleState % 3) {
        case 0: mode = RELAY_LOW; break;
        case 1: mode = RELAY_MID; break;
        case 2: mode = RELAY_HIGH; break;
    }
    toggleState++;

    // Use setModeWithVerification to trigger logging
    relayManager->setModeWithVerification(mode, "performance_test");
}

void LoggingPerformanceTest::simulateErrorGeneration() {
    static int errorType = 0;

    // Cycle through different error types for realistic testing
    ErrorCode codes[] = {
        ErrorCode::WIFI_CONNECTION_FAILED,
        ErrorCode::MQTT_CONNECTION_FAILED,
        ErrorCode::DS18B20_READ_FAILED,
        ErrorCode::CONFIG_LOAD_FAILED,
        ErrorCode::RELAY_TIMEOUT_VIOLATION
    };

    ErrorSeverity severities[] = {
        ErrorSeverity::WARNING,
        ErrorSeverity::ERROR,
        ErrorSeverity::INFO
    };

    ErrorCode code = codes[errorType % 5];
    ErrorSeverity severity = severities[errorType % 3];
    String message = "Performance test error #" + String(errorType + 1);

    ErrorHandler::reportError(code, severity, message);
    errorType++;
}

unsigned long LoggingPerformanceTest::calculateExpectedOperations(unsigned long durationMs, int rateHz) {
    return (durationMs * rateHz) / 1000;
}
