#include "DescriptorPoolSizes.hpp"

#include <algorithm>

std::vector<VkDescriptorPoolSize> descriptorPoolSizes(std::span<const VkDescriptorSetLayoutBinding> bindings,
                                                      uint32_t setCount) {
    std::vector<VkDescriptorPoolSize> sizes;
    for (const VkDescriptorSetLayoutBinding& binding : bindings) {
        if (binding.descriptorCount == 0) {
            continue;
        }
        const auto existing = std::find_if(sizes.begin(), sizes.end(), [&](const VkDescriptorPoolSize& size) {
            return size.type == binding.descriptorType;
        });
        if (existing != sizes.end()) {
            existing->descriptorCount += binding.descriptorCount * setCount;
        } else {
            sizes.push_back({.type = binding.descriptorType, .descriptorCount = binding.descriptorCount * setCount});
        }
    }
    return sizes;
}
