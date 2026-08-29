#pragma once

#include <volk.h>
#include "vulkan/VulkanContext.hpp"

class Renderer{

public:
bool init();
void run();
void shutdown();

private:

    VulkanContext m_context;

};
