#pragma once
#include <volk.h>

// Scene (main view) depth uses reversed Z: the near plane maps to 1 and the far
// plane to 0. Floating-point depth then spends its precision where perspective
// division removes it, which nearly eliminates distant z-fighting. Shaders that
// read scene depth (SSAO, SSR, post, HZB culling) assume this convention.
//
// Shadow maps are orthographic, so their depth is already linear and they keep
// the conventional 0 = near mapping.
namespace scene_depth {
inline constexpr float clearValue = 0.0f;
inline constexpr VkCompareOp compareOp = VK_COMPARE_OP_GREATER;
inline constexpr VkCompareOp compareOrEqualOp = VK_COMPARE_OP_GREATER_OR_EQUAL;
} // namespace scene_depth
