#include "ShaderModule.hpp"
#include "../vulkan/VulkanCheck.hpp"
#include <fstream>
#include <vector>

namespace {
bool readSpirv(const std::filesystem::path& path, std::vector<uint32_t>& code) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        std::cerr << "Cannot open shader: " << path << '\n';
        return false;
    }
    const std::streamoff size = file.tellg();
    if (size <= 0 || static_cast<size_t>(size) % sizeof(uint32_t) != 0) {
        std::cerr << "Invalid SPIR-V size: " << path << '\n';
        return false;
    }
    code.resize(static_cast<size_t>(size) / sizeof(uint32_t));
    file.seekg(0);
    constexpr uint32_t spirvMagic = 0x07230203;
    if (!file.read(reinterpret_cast<char*>(code.data()), static_cast<std::streamsize>(size))) {
        std::cerr << "Cannot read shader: " << path << '\n';
        return false;
    }
    if (code.front() != spirvMagic) {
        std::cerr << "Invalid SPIR-V data: " << path << '\n';
        return false;
    }
    return true;
}
} // namespace

bool ShaderModule::load(const std::filesystem::path& path) {
    std::vector<uint32_t> code;
    if (!readSpirv(path, code)) {
        return false;
    }
    VkShaderModuleCreateInfo info{.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    info.codeSize = code.size() * sizeof(uint32_t);
    info.pCode = code.data();
    VkShaderModule module = VK_NULL_HANDLE;
    if (!checkVulkan(vkCreateShaderModule(m_device, &info, nullptr, &module), "Create shader module")) {
        return false;
    }
    if (m_module) {
        vkDestroyShaderModule(m_device, m_module, nullptr);
    }
    m_module = module;
    return true;
}

ShaderModule::~ShaderModule() {
    if (m_module) {
        vkDestroyShaderModule(m_device, m_module, nullptr);
    }
}
