#pragma once
#include <iostream>
#include <volk.h>

inline bool checkVulkan(VkResult result, const char* operation) {
    if (result == VK_SUCCESS) {
        return true;
    }
    std::cerr << operation << " failed (VkResult " << result << ").\n";
    return false;
}
