#pragma once
#include <cstdint>

// Vulkan timestamps wrap at the queue family's timestampValidBits boundary.
inline double timestampMilliseconds(uint64_t first, uint64_t last, uint32_t validBits, double periodNanoseconds) {
    if (validBits == 0 || validBits > 64) {
        return 0.0;
    }
    const uint64_t mask = validBits == 64 ? UINT64_MAX : (uint64_t{1} << validBits) - 1;
    return static_cast<double>((last - first) & mask) * periodNanoseconds / 1'000'000.0;
}
