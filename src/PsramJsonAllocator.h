#ifndef PSRAM_JSON_ALLOCATOR_H
#define PSRAM_JSON_ALLOCATOR_H

#include <ArduinoJson.h>
#include "PsramAlloc.h"

class PsramJsonAllocator : public ArduinoJson::Allocator {
public:
    void* allocate(size_t size) override {
        return psram::allocate(size).ptr;
    }

    void deallocate(void* ptr) override {
        psram::deallocate(ptr);
    }

    void* reallocate(void* ptr, size_t new_size) override {
        return psram::reallocate(ptr, new_size).ptr;
    }
};

#endif // PSRAM_JSON_ALLOCATOR_H
