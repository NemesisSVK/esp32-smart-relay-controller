#include "PinValidator.h"
#include <algorithm>

bool PinValidator::isValidPinNumber(int pin) {
    return pin >= 0 && pin <= 50;
}

bool PinValidator::hasDuplicatePins(const std::vector<int>& pins) {
    if (pins.size() <= 1) {
        return false;
    }
    
    std::vector<int> sortedPins = pins;
    std::sort(sortedPins.begin(), sortedPins.end());
    
    for (size_t i = 1; i < sortedPins.size(); i++) {
        if (sortedPins[i] == sortedPins[i-1]) {
            return true;
        }
    }
    return false;
}


bool PinValidator::validateSHT4xPins(int sdaPin, int sclPin) {
    // SHT4x pins must be valid and different
    if (!isValidPinNumber(sdaPin) || !isValidPinNumber(sclPin)) {
        return false;
    }
    return sdaPin != sclPin;
}