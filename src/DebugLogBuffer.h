#ifndef DEBUGLOGBUFFER_H
#define DEBUGLOGBUFFER_H

#include <Arduino.h>
#include <stdint.h>

class DebugLogBuffer {
public:
    static void clear();

    // Viewer lock + capture gating (single viewer by IP).
    // - If not locked, claims lock and clears buffer.
    // - If locked by same IP, refreshes TTL.
    // - If locked by different IP and not expired, returns false and sets remainingMs.
    enum class EnableResult : uint8_t {
        Ok = 0,
        Busy = 1,
        NoMem = 2,
    };
    static EnableResult enableFor(IPAddress ip, uint32_t ttlMs, uint32_t& remainingMs);

    // Returns true if capture is enabled *and* owned by ip; refreshes TTL on success.
    static bool keepAlive(IPAddress ip, uint32_t ttlMs);

    static void disableIfOwner(IPAddress ip);
    static bool isEnabled();
    static bool isOwner(IPAddress ip);
    static uint32_t remainingMs();

    // Fast capture gate for UnifiedLogger hot-path:
    // - returns false with near-zero overhead when viewer isn't active
    // - returns false after TTL expiry even if state hasn't been cleaned up yet
    static bool shouldCaptureFast();

    // Append raw bytes to line assembler. Safe to call often; returns quickly when disabled.
    static void append(const char* data, size_t len);

    // Read records with id > sinceId into JSON-ish output builder.
    // - Writes JSON string fragments into out until maxBytes is reached.
    // - Returns true if any records were skipped due to overflow/drops.
    static uint32_t latestId();

    struct LineOut {
        uint32_t id;
        bool hasEpoch;
        uint32_t ts; // epoch seconds if hasEpoch, else millis
        bool truncated;
        const char* msg;
        uint16_t msgLen;
    };

    // Iterates up to maxLines lines after sinceId, within maxBytes budget (for msg bytes).
    // Returns number of lines written to outLines; sets dropped=true if sinceId is too old.
    static size_t readSince(uint32_t sinceId, size_t maxLines, size_t maxBytes, LineOut* outLines, bool& dropped);

private:
    static void expireIfNeeded();
    static void commitPartialLine();
};

#endif // DEBUGLOGBUFFER_H
