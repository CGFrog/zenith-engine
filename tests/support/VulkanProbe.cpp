#include "support/VulkanProbe.hpp"

// No volk and no Vulkan prototypes: this probe is linked into processes that
// also load the engine, whose volk would otherwise be preempted by a second
// copy's global function pointers. It loads the Vulkan loader itself and
// resolves the four functions it calls.
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include <vector>

namespace testsupport {
namespace {

// The platform's Vulkan loader, unloaded when it goes.
class VulkanLoader {
  public:
    VulkanLoader() {
#if defined(_WIN32)
        m_library = LoadLibraryA("vulkan-1.dll");
#elif defined(__APPLE__)
        m_library = dlopen("libvulkan.1.dylib", RTLD_NOW | RTLD_LOCAL);
        if (m_library == nullptr) {
            m_library = dlopen("libMoltenVK.dylib", RTLD_NOW | RTLD_LOCAL);
        }
#else
        m_library = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
        if (m_library == nullptr) {
            m_library = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
        }
#endif
    }

    ~VulkanLoader() {
        if (m_library == nullptr) {
            return;
        }
#if defined(_WIN32)
        FreeLibrary(static_cast<HMODULE>(m_library));
#else
        dlclose(m_library);
#endif
    }

    VulkanLoader(const VulkanLoader&) = delete;
    VulkanLoader& operator=(const VulkanLoader&) = delete;

    PFN_vkGetInstanceProcAddr getInstanceProcAddr() const {
        if (m_library == nullptr) {
            return nullptr;
        }
#if defined(_WIN32)
        return reinterpret_cast<PFN_vkGetInstanceProcAddr>(
            GetProcAddress(static_cast<HMODULE>(m_library), "vkGetInstanceProcAddr"));
#else
        return reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(m_library, "vkGetInstanceProcAddr"));
#endif
    }

  private:
    void* m_library{nullptr};
};

} // namespace

bool vulkanDeviceAvailable(VulkanDeviceKind kind, std::string& reason) {
    const VulkanLoader loader;
    const PFN_vkGetInstanceProcAddr getInstanceProcAddr = loader.getInstanceProcAddr();
    if (getInstanceProcAddr == nullptr) {
        reason = "no Vulkan loader";
        return false;
    }
    const auto createInstance =
        reinterpret_cast<PFN_vkCreateInstance>(getInstanceProcAddr(VK_NULL_HANDLE, "vkCreateInstance"));
    const VkApplicationInfo application{.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                                        .pApplicationName = "zenith_test_vulkan_probe",
                                        .apiVersion = VK_API_VERSION_1_3};
    const VkInstanceCreateInfo info{.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &application};
    VkInstance instance = VK_NULL_HANDLE;
    if (createInstance == nullptr || createInstance(&info, nullptr, &instance) != VK_SUCCESS) {
        reason = "cannot create a Vulkan instance";
        return false;
    }
    const auto destroyInstance =
        reinterpret_cast<PFN_vkDestroyInstance>(getInstanceProcAddr(instance, "vkDestroyInstance"));
    const auto enumerateDevices =
        reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(getInstanceProcAddr(instance, "vkEnumeratePhysicalDevices"));
    const auto deviceProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(
        getInstanceProcAddr(instance, "vkGetPhysicalDeviceProperties"));
    bool found = false;
    if (enumerateDevices != nullptr && deviceProperties != nullptr) {
        uint32_t count = 0;
        enumerateDevices(instance, &count, nullptr);
        std::vector<VkPhysicalDevice> devices(count);
        enumerateDevices(instance, &count, devices.data());
        for (VkPhysicalDevice device : devices) {
            VkPhysicalDeviceProperties properties{};
            deviceProperties(device, &properties);
            found = found || kind == VulkanDeviceKind::Any || properties.deviceType != VK_PHYSICAL_DEVICE_TYPE_CPU;
        }
    }
    if (destroyInstance != nullptr) {
        destroyInstance(instance, nullptr);
    }
    reason = kind == VulkanDeviceKind::Any ? "no Vulkan device" : "no hardware Vulkan device";
    return found;
}

} // namespace testsupport
