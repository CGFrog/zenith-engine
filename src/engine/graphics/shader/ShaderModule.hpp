#pragma once
#include <filesystem>
#include <volk.h>

// A short-lived shader module used during pipeline creation.
// The device must outlive this object.
class ShaderModule {
  public:
    explicit ShaderModule(VkDevice device)
        : m_device(device) {}

    ~ShaderModule();
    ShaderModule(const ShaderModule&) = delete;
    ShaderModule& operator=(const ShaderModule&) = delete;

    bool load(const std::filesystem::path& path);

    VkShaderModule handle() const {
        return m_module;
    }

  private:
    VkDevice m_device;
    VkShaderModule m_module{VK_NULL_HANDLE};
};
