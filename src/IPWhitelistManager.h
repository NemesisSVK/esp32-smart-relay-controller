#ifndef IPWHITELISTMANAGER_H
#define IPWHITELISTMANAGER_H

#include <Arduino.h>
#include <vector>
#include <IPAddress.h>

/**
 * IPWhitelistManager - Manages IP-based access control for the ESP32 web server
 *
 * Design Philosophy:
 * - Static methods for stateless validation (can be used without instance)
 * - Instance methods for stateful operations (require whitelist configuration)
 * - Consolidated core logic to eliminate duplication and ensure consistency
 * - Memory-efficient storage using std::vector of strings
 * - Security-first approach with 404 responses for blocked IPs
 */
class IPWhitelistManager {
public:
    IPWhitelistManager();
    ~IPWhitelistManager();

    // Configuration management
    bool setEnabled(bool enabled);
    bool addIPRange(const String& range);
    bool isEnabled() const;
    const std::vector<String>& getIPRanges() const;

    // Validation and checking
    bool isIPWhitelisted(const String& clientIP) const;
    bool isIPWhitelisted(const IPAddress& clientIP) const;
    
    // Input validation - Static methods for stateless validation that don't need instance data
    // These can be used for configuration validation without creating an instance
    static bool isValidIPv4(const String& ip);
    static bool isValidIPRange(const String& range);
    static bool isIPInRange(const String& ip, const String& range);

    // Utility functions - Static for reusability across different contexts
    // Used by both instance methods and external validation code
    static bool parseIPv4(const String& ip, uint8_t octets[4]);
    static bool parseIPRange(const String& range, uint8_t base[4], uint8_t& suffix);
    static bool compareIPs(const uint8_t ip1[4], const uint8_t ip2[4]);

private:
    bool enabled;
    std::vector<String> ipRanges;

    // Core range checking (consolidated logic) - Single implementation shared by all range checking methods
    // Eliminates code duplication and ensures consistent behavior across isIPInRange and isIPInRangeInternal
    static bool checkIPInRangeOctets(const uint8_t ip[4], const uint8_t base[4], uint8_t suffix);

    // Internal validation helpers
    bool isIPInRangeInternal(const uint8_t clientIP[4], const String& range) const;
    
    // Constants
    static const size_t MAX_IP_RANGES = 50;
    static const char* const RANGE_SEPARATOR;
};

#endif // IPWHITELISTMANAGER_H