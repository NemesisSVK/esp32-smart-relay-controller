#pragma once
#include <vector>
#include <string>

class PinValidator {
public:
    // Pin range validation (0-50)
    static bool isValidPinNumber(int pin);
    
    // Check for duplicate pins within an array
    static bool hasDuplicatePins(const std::vector<int>& pins);
    
    
    // Validate SHT4x pins specifically
    static bool validateSHT4xPins(int sdaPin, int sclPin);
};