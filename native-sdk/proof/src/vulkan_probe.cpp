#include "magpie/magpie.h"
#include <vulkan/vulkan.h>
#include <cstring>
#include <new>
#include <string>
#include <vector>

struct MagpieVulkanRuntimeV1 {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t family = UINT32_MAX;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer command = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties properties{};

    ~MagpieVulkanRuntimeV1() {
        if (device != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(device);
            if (fence != VK_NULL_HANDLE) vkDestroyFence(device, fence, nullptr);
            if (pool != VK_NULL_HANDLE) vkDestroyCommandPool(device, pool, nullptr);
            vkDestroyDevice(device, nullptr);
        }
        if (instance != VK_NULL_HANDLE) vkDestroyInstance(instance, nullptr);
    }

    MagpieStatusV1 initialize() {
        VkApplicationInfo app{};
        app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        app.pApplicationName = "Magpie Android Runtime";
        app.apiVersion = VK_API_VERSION_1_0;
        VkInstanceCreateInfo ici{};
        ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        ici.pApplicationInfo = &app;
        if (vkCreateInstance(&ici, nullptr, &instance) != VK_SUCCESS) return MAGPIE_UNAVAILABLE;

        uint32_t count = 0;
        if (vkEnumeratePhysicalDevices(instance, &count, nullptr) != VK_SUCCESS || count == 0)
            return MAGPIE_UNAVAILABLE;
        std::vector<VkPhysicalDevice> devices(count);
        if (vkEnumeratePhysicalDevices(instance, &count, devices.data()) != VK_SUCCESS)
            return MAGPIE_UNAVAILABLE;

        for (VkPhysicalDevice candidate : devices) {
            uint32_t qcount = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &qcount, nullptr);
            std::vector<VkQueueFamilyProperties> queues(qcount);
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &qcount, queues.data());
            for (uint32_t i = 0; i < qcount; ++i) {
                if (queues[i].queueCount && (queues[i].queueFlags & VK_QUEUE_COMPUTE_BIT)) {
                    physical = candidate;
                    family = i;
                    break;
                }
            }
            if (physical != VK_NULL_HANDLE) break;
        }
        if (physical == VK_NULL_HANDLE) return MAGPIE_UNAVAILABLE;
        vkGetPhysicalDeviceProperties(physical, &properties);

        VkFormatProperties format{};
        vkGetPhysicalDeviceFormatProperties(physical, VK_FORMAT_R8G8B8A8_UNORM, &format);
        const VkFormatFeatureFlags required =
            VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT;
        if ((format.optimalTilingFeatures & required) != required ||
            properties.limits.maxComputeWorkGroupInvocations < 64 ||
            properties.limits.maxComputeWorkGroupSize[0] < 8 ||
            properties.limits.maxComputeWorkGroupSize[1] < 8 ||
            properties.limits.maxImageDimension2D == 0) return MAGPIE_UNAVAILABLE;

        const float priority = 1.0f;
        VkDeviceQueueCreateInfo qci{};
        qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        qci.queueFamilyIndex = family;
        qci.queueCount = 1;
        qci.pQueuePriorities = &priority;
        VkDeviceCreateInfo dci{};
        dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        dci.queueCreateInfoCount = 1;
        dci.pQueueCreateInfos = &qci;
        if (vkCreateDevice(physical, &dci, nullptr, &device) != VK_SUCCESS)
            return MAGPIE_UNAVAILABLE;
        vkGetDeviceQueue(device, family, 0, &queue);
        if (queue == VK_NULL_HANDLE) return MAGPIE_UNAVAILABLE;

        VkCommandPoolCreateInfo pci{};
        pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pci.queueFamilyIndex = family;
        if (vkCreateCommandPool(device, &pci, nullptr, &pool) != VK_SUCCESS)
            return MAGPIE_INTERNAL_ERROR;
        VkCommandBufferAllocateInfo cai{};
        cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cai.commandPool = pool;
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = 1;
        if (vkAllocateCommandBuffers(device, &cai, &command) != VK_SUCCESS)
            return MAGPIE_INTERNAL_ERROR;
        VkFenceCreateInfo fci{};
        fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        if (vkCreateFence(device, &fci, nullptr, &fence) != VK_SUCCESS)
            return MAGPIE_INTERNAL_ERROR;
        return MAGPIE_OK;
    }

    void describe(MagpieBackendProbeV1* out) const {
        const uint32_t suppliedSize = out->struct_size;
        std::memset(out, 0, sizeof(*out));
        out->struct_size = suppliedSize;
        out->runtime_available = 1;
        out->compute_available = 1;
        out->fsr_dispatch_validated = 0;
        std::strncpy(out->device_name, properties.deviceName, sizeof(out->device_name)-1);
        const std::string reason =
            "persistent Vulkan instance, logical device, compute queue, command pool, command buffer and fence ready; FSR dispatch not yet validated";
        std::strncpy(out->reason, reason.c_str(), sizeof(out->reason)-1);
    }
};

extern "C" MAGPIE_API MagpieStatusV1
magpie_vulkan_runtime_create_v1(MagpieVulkanRuntimeV1** out) {
    if (!out) return MAGPIE_INVALID_ARGUMENT;
    *out = nullptr;
    auto* runtime = new (std::nothrow) MagpieVulkanRuntimeV1();
    if (!runtime) return MAGPIE_INTERNAL_ERROR;
    const auto status = runtime->initialize();
    if (status != MAGPIE_OK) { delete runtime; return status; }
    *out = runtime;
    return MAGPIE_OK;
}
extern "C" MAGPIE_API MagpieStatusV1
magpie_vulkan_runtime_describe_v1(MagpieVulkanRuntimeV1* runtime, MagpieBackendProbeV1* out) {
    if (!runtime || !out || out->struct_size < sizeof(*out)) return MAGPIE_INVALID_ARGUMENT;
    runtime->describe(out);
    return MAGPIE_OK;
}
extern "C" MAGPIE_API void magpie_vulkan_runtime_destroy_v1(MagpieVulkanRuntimeV1* runtime) {
    delete runtime;
}
extern "C" MAGPIE_API MagpieStatusV1 magpie_vulkan_probe_v1(MagpieBackendProbeV1* out) {
    if (!out || out->struct_size < sizeof(*out)) return MAGPIE_INVALID_ARGUMENT;
    const uint32_t suppliedSize = out->struct_size;
    std::memset(out, 0, sizeof(*out));
    out->struct_size = suppliedSize;
    std::strncpy(out->reason, "Vulkan runtime unavailable; FSR dispatch not validated", sizeof(out->reason)-1);
    MagpieVulkanRuntimeV1* runtime = nullptr;
    const auto status = magpie_vulkan_runtime_create_v1(&runtime);
    if (status == MAGPIE_OK && runtime) {
        runtime->describe(out);
        magpie_vulkan_runtime_destroy_v1(runtime);
    } else {
        std::strncpy(out->reason, "Vulkan instance/device/compute runtime initialization failed; FSR dispatch not validated", sizeof(out->reason)-1);
    }
    return status;
}
