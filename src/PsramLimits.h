#ifndef PSRAM_LIMITS_H
#define PSRAM_LIMITS_H

#include <stddef.h>

namespace psram_limits {

// Shared PSRAM budgets for staged async-memory migrations.
// These are intentionally conservative initial values and can be tuned
// after device-side stress tests.
constexpr size_t kRequestBodyPoolBytes = 192U * 1024U;
constexpr size_t kRequestBodyMaxPerRequestBytes = 16U * 1024U;

constexpr size_t kResponseChunkPoolBytes = 256U * 1024U;
constexpr size_t kResponseChunkMaxPerResponseBytes = 128U * 1024U;

constexpr size_t kJsonTempBudgetBytes = 128U * 1024U;

// Deferred MQTT publish cache (latest-value per topic, no history replay).
constexpr size_t kMqttOfflineCacheBytes = 64U * 1024U;
constexpr size_t kMqttOfflineCacheMaxEntryBytes = 2048U;
constexpr size_t kMqttOfflineCacheMaxEntries = 96U;

} // namespace psram_limits

#endif // PSRAM_LIMITS_H
