#pragma once
#include <volk.h>
class GraphicsPipeline{
public:

bool initGraphicsPipeline();

private:

const char *entryPoint = "main";
VkPipelineLayout m_pipelineLayout{VK_NULL_HANDLE};
VkPipeline pipeline {VK_NULL_HANDLE};

};