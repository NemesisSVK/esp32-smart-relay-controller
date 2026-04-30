#include "PsramAlloc.h"

#if defined(ARDUINO_ARCH_ESP32)
#include <esp_heap_caps.h>
#endif

#include <stdlib.h>

namespace psram {
namespace {

#if defined(ARDUINO_ARCH_ESP32)
AllocationResult allocateWithCaps(size_t sizeBytes, uint32_t caps, AllocationTier tier) {
    AllocationResult result;
    result.ptr = heap_caps_malloc(sizeBytes, caps);
    result.tier = result.ptr ? tier : AllocationTier::None;
    return result;
}

AllocationResult reallocateWithCaps(void* ptr, size_t sizeBytes, uint32_t caps, AllocationTier tier) {
    AllocationResult result;
    result.ptr = heap_caps_realloc(ptr, sizeBytes, caps);
    result.tier = result.ptr ? tier : AllocationTier::None;
    return result;
}
#endif

} // namespace

AllocationResult allocate(size_t sizeBytes) {
    if (sizeBytes == 0) {
        return {};
    }

#if defined(ARDUINO_ARCH_ESP32)
    AllocationResult psramResult = allocateWithCaps(
        sizeBytes,
        static_cast<uint32_t>(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
        AllocationTier::Psram);
    if (psramResult.ok()) {
        return psramResult;
    }

    return allocateWithCaps(
        sizeBytes,
        static_cast<uint32_t>(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
        AllocationTier::Internal);
#else
    AllocationResult result;
    result.ptr = malloc(sizeBytes);
    result.tier = result.ptr ? AllocationTier::Internal : AllocationTier::None;
    return result;
#endif
}

AllocationResult reallocate(void* ptr, size_t sizeBytes) {
    if (!ptr) {
        return allocate(sizeBytes);
    }

    if (sizeBytes == 0) {
        deallocate(ptr);
        return {};
    }

#if defined(ARDUINO_ARCH_ESP32)
    AllocationResult psramResult = reallocateWithCaps(
        ptr,
        sizeBytes,
        static_cast<uint32_t>(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
        AllocationTier::Psram);
    if (psramResult.ok()) {
        return psramResult;
    }

    return reallocateWithCaps(
        ptr,
        sizeBytes,
        static_cast<uint32_t>(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
        AllocationTier::Internal);
#else
    AllocationResult result;
    result.ptr = realloc(ptr, sizeBytes);
    result.tier = result.ptr ? AllocationTier::Internal : AllocationTier::None;
    return result;
#endif
}

void deallocate(void* ptr) {
    if (!ptr) {
        return;
    }

#if defined(ARDUINO_ARCH_ESP32)
    heap_caps_free(ptr);
#else
    free(ptr);
#endif
}

bool isPsramAvailable() {
#if defined(ARDUINO_ARCH_ESP32)
    return ESP.getPsramSize() > 0;
#else
    return false;
#endif
}

size_t totalBytes() {
#if defined(ARDUINO_ARCH_ESP32)
    return static_cast<size_t>(ESP.getPsramSize());
#else
    return 0;
#endif
}

size_t freeBytes() {
#if defined(ARDUINO_ARCH_ESP32)
    return static_cast<size_t>(ESP.getFreePsram());
#else
    return 0;
#endif
}

size_t maxAllocBytes() {
#if defined(ARDUINO_ARCH_ESP32)
    return static_cast<size_t>(ESP.getMaxAllocPsram());
#else
    return 0;
#endif
}

const char* tierName(AllocationTier tier) {
    switch (tier) {
        case AllocationTier::Psram:
            return "psram";
        case AllocationTier::Internal:
            return "internal";
        default:
            return "none";
    }
}

} // namespace psram
