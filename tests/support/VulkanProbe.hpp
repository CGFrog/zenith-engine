#pragma once

// Whether this machine has a Vulkan device a GPU test can run on, so the test can skip (77) instead of failing.

#include <string>

namespace testsupport {

enum class VulkanDeviceKind {
    // Any device, including a software one such as Mesa's lavapipe: what smokes
    // and the validation matrix need.
    Any,
    // A hardware GPU: reference captures, which software rasterizers cannot
    // reproduce.
    Hardware,
};

// Loads the Vulkan loader on its own (volk) and looks for a device of kind;
// false, with the reason, when there is none. Creates and destroys its own
// instance, so it is independent of the engine's.
bool vulkanDeviceAvailable(VulkanDeviceKind kind, std::string& reason);

} // namespace testsupport
