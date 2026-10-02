#pragma once

#include <volk.h>

#include <cstdint>
#include <span>
#include <vector>

// Pool sizes that hold exactly `setCount` sets of a layout built from
// `bindings`: one entry per descriptor type, summing every binding's
// descriptorCount. Deriving them from the layout's own bindings keeps a pool
// from drifting out of sync when bindings are added; drivers are free to fail
// allocations that exceed the declared pool sizes.
std::vector<VkDescriptorPoolSize> descriptorPoolSizes(std::span<const VkDescriptorSetLayoutBinding> bindings,
                                                      uint32_t setCount);
