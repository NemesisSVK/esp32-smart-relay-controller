#include "IPWhitelistManager.h"
#include <algorithm>

const char* const IPWhitelistManager::RANGE_SEPARATOR = "-";

IPWhitelistManager::IPWhitelistManager() : enabled(false) {
}

IPWhitelistManager::~IPWhitelistManager() {
}

bool IPWhitelistManager::setEnabled(bool enabled) {
    this->enabled = enabled;
    return true;
}

bool IPWhitelistManager::addIPRange(const String& range) {
    if (!isValidIPRange(range)) {
        return false;
    }
     
    if (ipRanges.size() >= MAX_IP_RANGES) {
        return false; // Too many ranges
    }
     
    // Check for duplicates
    if (std::any_of(ipRanges.begin(), ipRanges.end(), [&](const String& existing) { return existing == range; })) {
        return true; // Already exists
    }
     
    ipRanges.push_back(range);
    return true;
}

bool IPWhitelistManager::isEnabled() const {
    return enabled;
}

const std::vector<String>& IPWhitelistManager::getIPRanges() const {
    return ipRanges;
}

bool IPWhitelistManager::isIPWhitelisted(const String& clientIP) const {
    if (!enabled || ipRanges.empty()) {
        return true; // Allow if disabled or no ranges configured
    }
    
    uint8_t clientOctets[4];
    if (!parseIPv4(clientIP, clientOctets)) {
        return false; // Invalid IP format
    }
    
    return isIPWhitelisted(IPAddress(clientOctets[0], clientOctets[1], clientOctets[2], clientOctets[3]));
}

bool IPWhitelistManager::isIPWhitelisted(const IPAddress& clientIP) const {
    if (!enabled || ipRanges.empty()) {
        return true; // Allow if disabled or no ranges configured
    }
    
    uint8_t clientOctets[4] = {
        clientIP[0], clientIP[1], clientIP[2], clientIP[3]
    };
    
    if (std::any_of(ipRanges.begin(), ipRanges.end(), [&](const String& range) { return isIPInRangeInternal(clientOctets, range); })) {
        return true;
    }
    
    return false;
}

bool IPWhitelistManager::isValidIPv4(const String& ip) {
    uint8_t octets[4];
    return parseIPv4(ip, octets);
}

bool IPWhitelistManager::isValidIPRange(const String& range) {
    if (range.length() == 0) {
        return false;
    }
    
    // Check if it's a single IP
    if (range.indexOf(RANGE_SEPARATOR) == -1) {
        return isValidIPv4(range);
    }
    
    // Check if it's a range
    int dashPos = range.indexOf(RANGE_SEPARATOR);
    if (dashPos <= 0 || dashPos >= range.length() - 1) {
        return false; // Invalid dash position
    }
    
    String baseIP = range.substring(0, dashPos);
    String suffixStr = range.substring(dashPos + 1);
    
    // Validate base IP
    uint8_t baseOctets[4];
    if (!parseIPv4(baseIP, baseOctets)) {
        return false;
    }
    
    // Validate suffix
    int suffix = suffixStr.toInt();
    if (suffix < 1 || suffix > 255) {
        return false;
    }
    
    // Check that suffix doesn't exceed 255 when added to last octet
    if (baseOctets[3] + suffix > 255) {
        return false;
    }
    
    return true;
}

bool IPWhitelistManager::isIPInRange(const String& ip, const String& range) {
    uint8_t ipOctets[4];
    uint8_t baseOctets[4];
    uint8_t suffix;

    if (!parseIPv4(ip, ipOctets) || !parseIPRange(range, baseOctets, suffix)) {
        return false;
    }

    return checkIPInRangeOctets(ipOctets, baseOctets, suffix);
}

bool IPWhitelistManager::parseIPv4(const String& ip, uint8_t octets[4]) {
    int octetCount = 0;
    String currentOctet = "";
    
    for (size_t i = 0; i < ip.length(); i++) {
        char c = ip[i];
        
        if (c == '.') {
            if (currentOctet.length() == 0) {
                return false; // Empty octet
            }
            
            int value = currentOctet.toInt();
            if (value < 0 || value > 255) {
                return false; // Out of range
            }
            
            octets[octetCount++] = (uint8_t)value;
            currentOctet = "";
            
            if (octetCount > 3) {
                return false; // Too many octets
            }
        } else if (c >= '0' && c <= '9') {
            currentOctet += c;
            if (currentOctet.length() > 3) {
                return false; // Too many digits
            }
        } else {
            return false; // Invalid character
        }
    }
    
    // Process last octet
    if (currentOctet.length() == 0) {
        return false; // Empty last octet
    }
    
    int value = currentOctet.toInt();
    if (value < 0 || value > 255) {
        return false; // Out of range
    }
    
    octets[octetCount++] = (uint8_t)value;
    
    return octetCount == 4;
}

bool IPWhitelistManager::parseIPRange(const String& range, uint8_t base[4], uint8_t& suffix) {
    int dashPos = range.indexOf(RANGE_SEPARATOR);
    
    if (dashPos == -1) {
        // Single IP
        if (!parseIPv4(range, base)) {
            return false;
        }
        suffix = 0;
        return true;
    }
    
    // Range format
    String baseIP = range.substring(0, dashPos);
    String suffixStr = range.substring(dashPos + 1);
    
    if (!parseIPv4(baseIP, base)) {
        return false;
    }
    
    int suffixValue = suffixStr.toInt();
    if (suffixValue < 1 || suffixValue > 255) {
        return false;
    }
    
    suffix = (uint8_t)suffixValue;
    return true;
}

bool IPWhitelistManager::compareIPs(const uint8_t ip1[4], const uint8_t ip2[4]) {
    return (ip1[0] == ip2[0] && ip1[1] == ip2[1] &&
            ip1[2] == ip2[2] && ip1[3] == ip2[3]);
}

/**
 * Core range checking logic - consolidated implementation used by all range validation methods
 *
 * This method implements the essential IP range checking algorithm:
 * 1. First three octets must match exactly (same subnet)
 * 2. Last octet must be within [base[3], base[3] + suffix] range
 *
 * Range format: "192.168.1.50-13" means IPs 192.168.1.50 through 192.168.1.63
 * where suffix=13, so range is 50 to 50+13=63
 *
 * @param ip The IP address to check (4 octets)
 * @param base The base IP address (4 octets)
 * @param suffix Number of additional IPs to include in range
 * @return true if IP is within the range, false otherwise
 */
bool IPWhitelistManager::checkIPInRangeOctets(const uint8_t ip[4], const uint8_t base[4], uint8_t suffix) {
    // Check first three octets match
    if (ip[0] != base[0] || ip[1] != base[1] || ip[2] != base[2]) {
        return false;
    }

    // Check if last octet is in range
    uint8_t minOctet = base[3];
    uint8_t maxOctet = base[3] + suffix;

    return (ip[3] >= minOctet && ip[3] <= maxOctet);
}

bool IPWhitelistManager::isIPInRangeInternal(const uint8_t clientIP[4], const String& range) const {
    uint8_t baseOctets[4];
    uint8_t suffix;

    if (!parseIPRange(range, baseOctets, suffix)) {
        return false;
    }

    return checkIPInRangeOctets(clientIP, baseOctets, suffix);
}