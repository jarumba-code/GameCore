// Magpie FSR layer — Android Vulkan device layer implementing the researched pipeline:
//
//   app renders into layer-owned LOW-RESOLUTION images (substituted via vkGetSwapchainImagesKHR)
//     → vkQueuePresentKHR: layer records EASU (low→native) → RCAS → CopyImage into the REAL
//       native-resolution swapchain image → restores PRESENT_SRC_KHR → presents.
//
// Loading (verified against AOSP frameworks/native/vulkan/libvulkan, see
// docs/ANDROID_MAGPIE_FRAME_ACCESS_DECISION.md): the Android loader always scans the app's own
// nativeLibraryDir for libVkLayer_*.so; an explicit ppEnabledLayerNames enable works for ANY app
// on user builds — the controlled test app opts in by name. Nothing here can load into a retail
// game that does not ship this layer in its own APK.
//
// Synchronization (verified pattern, vkBasalt-style): vkQueuePresentKHR(queue, info) is called
// with the app's render-signal semaphore(s) in pWaitSemaphores. The layer records its command
// buffer waiting on those exact semaphores, signals a per-image semaphore of its own, rewrites
// pWaitSemaphores to it, and calls the next layer's/driver's vkQueuePresentKHR. Acquire and
// vkQueueSubmit are never intercepted.
//
// Fallback ladder (each rung logs its reason; the layer degrades, never errors out):
//   L1 FSR     — low-res substitution + EASU/RCAS + CopyImage (requires SAMPLED on fake images,
//                TRANSFER_DST on real images, VK_FORMAT_R8G8B8A8_UNORM, compute-capable present
//                queue)
//   L2 BLIT    — low-res substitution + vkCmdBlitImage upscale (linear filter; no FSR)
//   L3 PASS    — no substitution; the app's original swapchain is handed through untouched
#define VK_USE_PLATFORM_ANDROID_KHR
#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>
#include <android/log.h>
#include <sys/system_properties.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <utility>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include "magpie_shaders.h"

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "MagpieLayer", __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, "MagpieLayer", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "MagpieLayer", __VA_ARGS__)

static const char* MAGPIE_LAYER_NAME = "VK_LAYER_MAGPIE_FSR1";
static const char* MAGPIE_SCALE_PROP = "debug.magpie.scale";
static const char* MAGPIE_FSR_PROP = "debug.magpie.fsr";
static constexpr float MAGPIE_DEFAULT_SCALE = 0.5f;
static constexpr uint32_t kWorkgroup = 8;

namespace {

float magpieScale() {
    char value[32] = {0};
    if (__system_property_get(MAGPIE_SCALE_PROP, value) > 0) {
        const float parsed = strtof(value, nullptr);
        if (parsed > 0.1f && parsed < 1.0f) return parsed;
    }
    return MAGPIE_DEFAULT_SCALE;
}

bool magpieFsrWanted() {
    char value[32] = {0};
    if (__system_property_get(MAGPIE_FSR_PROP, value) > 0) return value[0] != '0';
    return true;
}

enum class Mode { FSR, BLIT, PASS };

struct SizedImage {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
};

// Per-swapchain substitution state, keyed by the REAL swapchain handle the app holds.
struct Substitution {
    VkSwapchainKHR real = VK_NULL_HANDLE;
    VkExtent2D lowExtent{};
    VkExtent2D realExtent{};
    VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
    uint32_t imageCount = 0;
    Mode mode = Mode::PASS;
    std::vector<VkImage> realImages;
    std::vector<SizedImage> fake;      // what the app renders into (low resolution)
    SizedImage middle{};               // EASU output at native size, GPU-resident
    SizedImage target{};               // RCAS output at native size, TRANSFER_SRC
    VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
    VkDescriptorSet easuSet = VK_NULL_HANDLE;
    VkDescriptorSet rcasSet = VK_NULL_HANDLE;
    std::vector<VkSemaphore> presentSems;
    uint64_t dispatchedFrames = 0;
    double easuMs = 0.0, rcasMs = 0.0;
    bool timingsValid = false;
};

struct DeviceData {
    // Entry points obtained from the next layer/driver at vkCreateDevice time.
    PFN_vkGetDeviceProcAddr gdpA_next = nullptr;
    PFN_vkGetInstanceProcAddr gpiA_next = nullptr;
    VkDevice device = VK_NULL_HANDLE;
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    std::string deviceName;

    // Shared, lazily created engine resources (FSR mode only).
    bool engineReady = false;
    bool engineFailed = false;
    std::string failureReason;
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkShaderModule easuModule = VK_NULL_HANDLE;
    VkShaderModule rcasModule = VK_NULL_HANDLE;
    VkPipeline easuPipeline = VK_NULL_HANDLE;
    VkPipeline rcasPipeline = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;

    // One serialized layer command buffer. Its fence must complete before the buffer is reset
    // or its query pool is reused on the next present.
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer command = VK_NULL_HANDLE;
    VkQueryPool queries = VK_NULL_HANDLE;
    VkFence submitFence = VK_NULL_HANDLE;
    bool commandPending = false;
    double timestampPeriodNanos = 0.0;
    // Default for applications that treat substituted images like swapchain images. The
    // controlled test app opts into GENERAL because its layer-owned images are not presentable.
    VkImageLayout fakeFinalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    std::unordered_map<VkSwapchainKHR, Substitution> substitutions;
    std::unordered_map<VkQueue, uint32_t> queueFamilies;
    std::mutex lock;  // guards substitutions + command buffer recording
};

std::mutex g_instanceLock;
std::unordered_map<VkInstance, PFN_vkGetInstanceProcAddr> g_nextGpiA;
std::mutex g_deviceLock;
std::unordered_map<VkDevice, DeviceData*> g_devices;
std::mutex g_swapchainOwnerLock;
std::unordered_map<VkSwapchainKHR, DeviceData*> g_swapchainOwners;
std::mutex g_queueOwnerLock;
std::unordered_map<VkQueue, DeviceData*> g_queueOwners;

void registerSwapchainOwner(VkSwapchainKHR swapchain, DeviceData* device) {
    if (swapchain == VK_NULL_HANDLE || device == nullptr) return;
    std::lock_guard<std::mutex> guard(g_swapchainOwnerLock);
    g_swapchainOwners[swapchain] = device;
}

PFN_vkGetInstanceProcAddr nextGpiA(VkInstance instance) {
    std::lock_guard<std::mutex> guard(g_instanceLock);
    const auto it = g_nextGpiA.find(instance);
    return it == g_nextGpiA.end() ? nullptr : it->second;
}

DeviceData* deviceData(VkDevice device) {
    std::lock_guard<std::mutex> guard(g_deviceLock);
    const auto it = g_devices.find(device);
    return it == g_devices.end() ? nullptr : it->second;
}

extern "C" VKAPI_ATTR VkResult VKAPI_CALL
magpie_GetSwapchainRenderExtent(VkDevice device, VkSwapchainKHR swapchain,
                                VkExtent2D* pRenderExtent) {
    auto* d = deviceData(device);
    if (!d || !pRenderExtent) return VK_ERROR_INITIALIZATION_FAILED;
    std::lock_guard<std::mutex> guard(d->lock);
    const auto it = d->substitutions.find(swapchain);
    if (it == d->substitutions.end() || it->second.mode == Mode::PASS)
        return VK_ERROR_FEATURE_NOT_PRESENT;
    *pRenderExtent = it->second.lowExtent;
    return VK_SUCCESS;
}

extern "C" VKAPI_ATTR VkResult VKAPI_CALL
magpie_SetFakeImageFinalLayout(VkDevice device, VkImageLayout layout) {
    auto* d = deviceData(device);
    if (!d || (layout != VK_IMAGE_LAYOUT_PRESENT_SRC_KHR && layout != VK_IMAGE_LAYOUT_GENERAL))
        return VK_ERROR_INITIALIZATION_FAILED;
    std::lock_guard<std::mutex> guard(d->lock);
    d->fakeFinalLayout = layout;
    LOGI("controlled app selected fake image final layout %d", (int)layout);
    return VK_SUCCESS;
}

template <typename T>
T deviceProc(DeviceData* d, const char* name) {
    return reinterpret_cast<T>(d->gdpA_next(d->device, name));
}

void destroySubstitution(DeviceData* d, Substitution& s) {
    auto destroyImage = [&](SizedImage& im) {
        if (im.view) deviceProc<PFN_vkDestroyImageView>(d, "vkDestroyImageView")(d->device, im.view, nullptr);
        if (im.image) deviceProc<PFN_vkDestroyImage>(d, "vkDestroyImage")(d->device, im.image, nullptr);
        if (im.mem) deviceProc<PFN_vkFreeMemory>(d, "vkFreeMemory")(d->device, im.mem, nullptr);
        im = {};
    };
    for (auto& fake : s.fake) destroyImage(fake);
    s.fake.clear();
    destroyImage(s.middle);
    destroyImage(s.target);
    if (s.descriptorPool) {
        deviceProc<PFN_vkDestroyDescriptorPool>(d, "vkDestroyDescriptorPool")(d->device, s.descriptorPool, nullptr);
        s.descriptorPool = VK_NULL_HANDLE;
    }
    for (VkSemaphore sem : s.presentSems) {
        deviceProc<PFN_vkDestroySemaphore>(d, "vkDestroySemaphore")(d->device, sem, nullptr);
    }
    s.presentSems.clear();
    s.realImages.clear();
    s.real = VK_NULL_HANDLE;
}

// Shared per-device engine creation: shader modules, layouts, pipelines, sampler. Only called
// once; safe on failure (engineFailed latches and every later present passes through).
bool initEngine(DeviceData* d) {
    if (d->engineReady) return true;
    if (d->engineFailed) return false;

    auto create = [&](const char* name) {
        return d->gdpA_next(d->device, name);
    };
    const auto createShaderModule = reinterpret_cast<PFN_vkCreateShaderModule>(
        create("vkCreateShaderModule"));
    const auto createDescriptorSetLayout = reinterpret_cast<PFN_vkCreateDescriptorSetLayout>(
        create("vkCreateDescriptorSetLayout"));
    const auto createPipelineLayout = reinterpret_cast<PFN_vkCreatePipelineLayout>(
        create("vkCreatePipelineLayout"));
    const auto createComputePipelines = reinterpret_cast<PFN_vkCreateComputePipelines>(
        create("vkCreateComputePipelines"));
    const auto createSampler = reinterpret_cast<PFN_vkCreateSampler>(
        create("vkCreateSampler"));
    const auto destroyShaderModule = reinterpret_cast<PFN_vkDestroyShaderModule>(
        create("vkDestroyShaderModule"));

    VkShaderModuleCreateInfo smi{};
    smi.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smi.codeSize = static_cast<size_t>(magpie_shaders::kEasuSpv_word_count) * 4u;
    smi.pCode = magpie_shaders::kEasuSpv;
    if (createShaderModule(d->device, &smi, nullptr, &d->easuModule) != VK_SUCCESS) {
        d->failureReason = "EASU shader module creation failed";
        d->engineFailed = true;
        return false;
    }
    smi.pCode = magpie_shaders::kRcasSpv;
    smi.codeSize = static_cast<size_t>(magpie_shaders::kRcasSpv_word_count) * 4u;
    if (createShaderModule(d->device, &smi, nullptr, &d->rcasModule) != VK_SUCCESS) {
        d->failureReason = "RCAS shader module creation failed";
        d->engineFailed = true;
        return false;
    }

    VkDescriptorSetLayoutBinding binds[2]{};
    binds[0] = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    binds[1] = {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo sli{};
    sli.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    sli.bindingCount = 2;
    sli.pBindings = binds;
    if (createDescriptorSetLayout(d->device, &sli, nullptr, &d->setLayout) != VK_SUCCESS) {
        d->failureReason = "descriptor set layout creation failed";
        d->engineFailed = true;
        return false;
    }
    VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT, 0, 72};
    VkPipelineLayoutCreateInfo lci{};
    lci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    lci.setLayoutCount = 1;
    lci.pSetLayouts = &d->setLayout;
    lci.pushConstantRangeCount = 1;
    lci.pPushConstantRanges = &pcr;
    if (createPipelineLayout(d->device, &lci, nullptr, &d->pipelineLayout) != VK_SUCCESS) {
        d->failureReason = "pipeline layout creation failed";
        d->engineFailed = true;
        return false;
    }
    const auto makePipeline = [&](VkShaderModule module, VkPipeline* out) {
        VkPipelineShaderStageCreateInfo st{};
        st.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        st.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        st.module = module;
        st.pName = "main";
        VkComputePipelineCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        ci.stage = st;
        ci.layout = d->pipelineLayout;
        return createComputePipelines(d->device, VK_NULL_HANDLE, 1, &ci, nullptr, out) == VK_SUCCESS;
    };
    if (!makePipeline(d->easuModule, &d->easuPipeline) || !makePipeline(d->rcasModule, &d->rcasPipeline)) {
        d->failureReason = "compute pipeline creation failed";
        d->engineFailed = true;
        return false;
    }
    VkSamplerCreateInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    si.magFilter = si.minFilter = VK_FILTER_NEAREST;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.maxLod = 0;
    if (createSampler(d->device, &si, nullptr, &d->sampler) != VK_SUCCESS) {
        d->failureReason = "sampler creation failed";
        d->engineFailed = true;
        return false;
    }
    d->engineReady = true;
    LOGI("FSR engine ready (device %s)", d->deviceName.c_str());
    return true;
}

uint32_t floatBits(float value) {
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

void fillEasuConstants(uint32_t* out, uint32_t inW, uint32_t inH, uint32_t outW, uint32_t outH) {
    const float iw = static_cast<float>(inW), ih = static_cast<float>(inH);
    const float fw = static_cast<float>(outW), fh = static_cast<float>(outH);
    const float c[16] = {
        iw / fw, ih / fh,
        0.5f * iw / fw - 0.5f, 0.5f * ih / fh - 0.5f,
        1.0f / iw, 1.0f / ih,
        1.0f / iw, -1.0f / ih,
        -1.0f / iw, 2.0f / ih,
        1.0f / iw, 2.0f / ih,
        0.0f, 4.0f / ih,
        0.0f, 0.0f,
    };
    for (int i = 0; i < 16; ++i) out[i] = floatBits(c[i]);
}

void recordBarrier(VkCommandBuffer cb, VkImage image, VkImageLayout from, VkImageLayout to,
                   VkAccessFlags srcAccess, VkAccessFlags dstAccess,
                   VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage,
                   PFN_vkCmdPipelineBarrier barrierFn) {
    VkImageMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.srcAccessMask = srcAccess;
    b.dstAccessMask = dstAccess;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    barrierFn(cb, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

}  // namespace

// ---------------------------------------------------------------------------
// Instance chain
// ---------------------------------------------------------------------------

static const VkLayerProperties MAGPIE_LAYER_PROPERTIES = {
    MAGPIE_LAYER_NAME,
    VK_MAKE_VERSION(1, 0, 0),
    VK_MAKE_VERSION(1, 0, 0),
    "Magpie FSR 1.0.2 swapchain layer (GameCore controlled test app only)",
};

extern "C" VKAPI_ATTR VkResult VKAPI_CALL
magpie_EnumerateInstanceLayerProperties(uint32_t* pPropertyCount, VkLayerProperties* pProperties) {
    if (!pPropertyCount) return VK_ERROR_INITIALIZATION_FAILED;
    if (!pProperties) {
        *pPropertyCount = 1;
        return VK_SUCCESS;
    }
    if (*pPropertyCount == 0) return VK_INCOMPLETE;
    pProperties[0] = MAGPIE_LAYER_PROPERTIES;
    *pPropertyCount = 1;
    return VK_SUCCESS;
}

extern "C" VKAPI_ATTR VkResult VKAPI_CALL
magpie_EnumerateInstanceExtensionProperties(const char*, uint32_t* pCount,
                                            VkExtensionProperties* pProperties) {
    if (pCount) *pCount = 0;
    (void)pProperties;
    return VK_SUCCESS;
}

extern "C" VKAPI_ATTR VkResult VKAPI_CALL
magpie_CreateInstance(const VkInstanceCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator,
                      VkInstance* pInstance) {
    // Unchain the loader's link-info so the next layer/driver sees a clean create info.
    auto* chainInfo = static_cast<VkLayerInstanceCreateInfo*>(
        const_cast<void*>(pCreateInfo->pNext));
    PFN_vkGetInstanceProcAddr gpiA_next = nullptr;
    while (chainInfo && !(chainInfo.sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO &&
                          chainInfo.function == VK_LAYER_LINK_INFO)) {
        chainInfo = static_cast<VkLayerInstanceCreateInfo*>(const_cast<void*>(chainInfo.pNext));
    }
    if (chainInfo) {
        gpiA_next = chainInfo.u.pLayerInfo->pGetInstanceProcAddr;
        chainInfo.u.pLayerInfo = chainInfo.u.pLayerInfo->pNext;
    } else {
        LOGE("loader link info missing; layer cannot chain");
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    const auto createInstance =
        reinterpret_cast<PFN_vkCreateInstance>(gpiA_next(nullptr, "vkCreateInstance"));
    const VkResult result = createInstance(pCreateInfo, pAllocator, pInstance);
    if (result == VK_SUCCESS) {
        std::lock_guard<std::mutex> guard(g_instanceLock);
        g_nextGpiA[*pInstance] = gpiA_next;
        LOGI("instance created; layer enabled: scale=%.2f fsr=%d", magpieScale(),
             magpieFsrWanted() ? 1 : 0);
    }
    return result;
}

extern "C" VKAPI_ATTR void VKAPI_CALL
magpie_DestroyInstance(VkInstance instance, const VkAllocationCallbacks* allocator) {
    const auto gpiA_next = nextGpiA(instance);
    {
        std::lock_guard<std::mutex> guard(g_instanceLock);
        g_nextGpiA.erase(instance);
    }
    if (gpiA_next) {
        const auto destroyInstance =
            reinterpret_cast<PFN_vkDestroyInstance>(gpiA_next(instance, "vkDestroyInstance"));
        destroyInstance(instance, allocator);
    }
}

extern "C" VKAPI_ATTR void* VKAPI_CALL
magpie_GetInstanceProcAddr(VkInstance instance, const char* name) {
    if (!name) return nullptr;
    if (strcmp(name, "vkGetInstanceProcAddr") == 0) {
        return reinterpret_cast<void*>(&magpie_GetInstanceProcAddr);
    }
    if (strcmp(name, "vkEnumerateInstanceLayerProperties") == 0)
        return reinterpret_cast<void*>(&magpie_EnumerateInstanceLayerProperties);
    if (strcmp(name, "vkEnumerateInstanceExtensionProperties") == 0)
        return reinterpret_cast<void*>(&magpie_EnumerateInstanceExtensionProperties);
    if (strcmp(name, "vkCreateInstance") == 0)
        return reinterpret_cast<void*>(&magpie_CreateInstance);
    if (strcmp(name, "vkDestroyInstance") == 0)
        return reinterpret_cast<void*>(&magpie_DestroyInstance);
    const auto gpiA_next = instance ? nextGpiA(instance) : nullptr;
    return gpiA_next ? gpiA_next(instance, name) : nullptr;
}

// ---------------------------------------------------------------------------
// Device chain
// ---------------------------------------------------------------------------

extern "C" VKAPI_ATTR VkResult VKAPI_CALL
magpie_CreateDevice(VkPhysicalDevice physical, const VkDeviceCreateInfo* pCreateInfo,
                    const VkAllocationCallbacks* pAllocator, VkDevice* pDevice) {
    auto* chainInfo = static_cast<VkLayerDeviceCreateInfo*>(const_cast<void*>(pCreateInfo->pNext));
    PFN_vkGetInstanceProcAddr gpiA_next = nullptr;
    PFN_vkGetDeviceProcAddr gdpA_next = nullptr;
    while (chainInfo && !(chainInfo.sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO &&
                          chainInfo.function == VK_LAYER_LINK_INFO)) {
        chainInfo = static_cast<VkLayerDeviceCreateInfo*>(const_cast<void*>(chainInfo.pNext));
    }
    if (chainInfo) {
        gpiA_next = chainInfo.u.pLayerInfo->pGetInstanceProcAddr;
        gdpA_next = chainInfo.u.pLayerInfo->pGetNextDeviceProcAddr;
        chainInfo.u.pLayerInfo = chainInfo.u.pLayerInfo->pNext;
    } else {
        LOGE("device loader link info missing");
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    const auto createDevice =
        reinterpret_cast<PFN_vkCreateDevice>(gpiA_next(nullptr, "vkCreateDevice"));
    const VkResult result = createDevice(physical, pCreateInfo, pAllocator, pDevice);
    if (result != VK_SUCCESS) return result;

    auto* data = new DeviceData();
    data->gdpA_next = gdpA_next;
    data->gpiA_next = gpiA_next;
    data->device = *pDevice;
    data->physical = physical;
    const auto props = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(
        gpiA_next(nullptr, "vkGetPhysicalDeviceProperties"));
    if (props) {
        VkPhysicalDeviceProperties p{};
        props(physical, &p);
        data->deviceName = p.deviceName;
        data->timestampPeriodNanos = p.limits.timestampPeriod;
    }
    {
        std::lock_guard<std::mutex> guard(g_deviceLock);
        g_devices[*pDevice] = data;
    }
    LOGI("device created: %s", data->deviceName.c_str());
    return VK_SUCCESS;
}

extern "C" VKAPI_ATTR void VKAPI_CALL
magpie_DestroyDevice(VkDevice device, const VkAllocationCallbacks* allocator) {
    auto* d = deviceData(device);
    if (d) {
        std::lock_guard<std::mutex> guard(d->lock);
        if (d->commandPending && d->submitFence) {
            const VkResult waitResult = deviceProc<PFN_vkWaitForFences>(d, "vkWaitForFences")(
                device, 1, &d->submitFence, VK_TRUE, UINT64_MAX);
            if (waitResult != VK_SUCCESS)
                LOGE("waiting for layer work during device destruction failed: %d", (int)waitResult);
            d->commandPending = false;
        }
        const auto waitDeviceIdle = deviceProc<PFN_vkDeviceWaitIdle>(d, "vkDeviceWaitIdle");
        const VkResult idleResult = waitDeviceIdle(device);
        if (idleResult != VK_SUCCESS)
            LOGE("vkDeviceWaitIdle during device destruction returned %d", (int)idleResult);
        for (auto& entry : d->substitutions) {
            destroySubstitution(d, entry.second);
        }
        d->substitutions.clear();
        {
            std::lock_guard<std::mutex> ownerGuard(g_swapchainOwnerLock);
            for (auto it = g_swapchainOwners.begin(); it != g_swapchainOwners.end();) {
                if (it->second == d) it = g_swapchainOwners.erase(it);
                else ++it;
            }
        }
        {
            std::lock_guard<std::mutex> ownerGuard(g_queueOwnerLock);
            for (auto it = g_queueOwners.begin(); it != g_queueOwners.end();) {
                if (it->second == d) it = g_queueOwners.erase(it);
                else ++it;
            }
        }
        if (d->submitFence) {
            if (d->commandPending) {
                deviceProc<PFN_vkWaitForFences>(d, "vkWaitForFences")(device, 1, &d->submitFence,
                    VK_TRUE, UINT64_MAX);
                d->commandPending = false;
            }
            deviceProc<PFN_vkDestroyFence>(d, "vkDestroyFence")(device, d->submitFence, nullptr);
            d->submitFence = VK_NULL_HANDLE;
        }
        if (d->pool) {
            deviceProc<PFN_vkDestroyCommandPool>(d, "vkDestroyCommandPool")(device, d->pool, nullptr);
            d->pool = VK_NULL_HANDLE;
            d->command = VK_NULL_HANDLE;
        }
        if (d->queries) {
            deviceProc<PFN_vkDestroyQueryPool>(d, "vkDestroyQueryPool")(device, d->queries, nullptr);
            d->queries = VK_NULL_HANDLE;
        }
        if (d->sampler) {
            deviceProc<PFN_vkDestroySampler>(d, "vkDestroySampler")(device, d->sampler, nullptr);
            d->sampler = VK_NULL_HANDLE;
        }
        if (d->easuPipeline) {
            deviceProc<PFN_vkDestroyPipeline>(d, "vkDestroyPipeline")(device, d->easuPipeline, nullptr);
            d->easuPipeline = VK_NULL_HANDLE;
        }
        if (d->rcasPipeline) {
            deviceProc<PFN_vkDestroyPipeline>(d, "vkDestroyPipeline")(device, d->rcasPipeline, nullptr);
            d->rcasPipeline = VK_NULL_HANDLE;
        }
        if (d->easuModule) {
            deviceProc<PFN_vkDestroyShaderModule>(d, "vkDestroyShaderModule")(device, d->easuModule, nullptr);
            d->easuModule = VK_NULL_HANDLE;
        }
        if (d->rcasModule) {
            deviceProc<PFN_vkDestroyShaderModule>(d, "vkDestroyShaderModule")(device, d->rcasModule, nullptr);
            d->rcasModule = VK_NULL_HANDLE;
        }
        if (d->pipelineLayout) {
            deviceProc<PFN_vkDestroyPipelineLayout>(d, "vkDestroyPipelineLayout")(device, d->pipelineLayout, nullptr);
            d->pipelineLayout = VK_NULL_HANDLE;
        }
        if (d->setLayout) {
            deviceProc<PFN_vkDestroyDescriptorSetLayout>(d, "vkDestroyDescriptorSetLayout")(device, d->setLayout, nullptr);
            d->setLayout = VK_NULL_HANDLE;
        }
        const auto destroyDevice =
            reinterpret_cast<PFN_vkDestroyDevice>(d->gdpA_next(device, "vkDestroyDevice"));
        {
            std::lock_guard<std::mutex> guard(g_deviceLock);
            g_devices.erase(device);
        }
        delete d;
        destroyDevice(device, allocator);
        return;
    }
    // No interposed device known: pass through with the standard entry point.
    reinterpret_cast<PFN_vkDestroyDevice>(vkGetInstanceProcAddr(nullptr, "vkDestroyDevice"))(device, allocator);
}

extern "C" VKAPI_ATTR void VKAPI_CALL
magpie_GetDeviceQueue(VkDevice device, uint32_t queueFamilyIndex, uint32_t queueIndex, VkQueue* pQueue) {
    auto* d = deviceData(device);
    const auto getQueue = reinterpret_cast<PFN_vkGetDeviceQueue>(
        d ? d->gdpA_next(device, "vkGetDeviceQueue")
          : vkGetInstanceProcAddr(nullptr, "vkGetDeviceQueue"));
    getQueue(device, queueFamilyIndex, queueIndex, pQueue);
    if (d && *pQueue) {
        {
            std::lock_guard<std::mutex> guard(d->lock);
            d->queueFamilies[*pQueue] = queueFamilyIndex;
        }
        std::lock_guard<std::mutex> ownerGuard(g_queueOwnerLock);
        g_queueOwners[*pQueue] = d;
    }
}

extern "C" VKAPI_ATTR void VKAPI_CALL
magpie_GetDeviceQueue2(VkDevice device, VkDeviceQueueInfo2* pQueueInfo, VkQueue* pQueue) {
    auto* d = deviceData(device);
    const auto getQueue2 = reinterpret_cast<PFN_vkGetDeviceQueue2>(
        d ? d->gdpA_next(device, "vkGetDeviceQueue2")
          : vkGetInstanceProcAddr(nullptr, "vkGetDeviceQueue2"));
    getQueue2(device, pQueueInfo, pQueue);
    if (d && *pQueue) {
        {
            std::lock_guard<std::mutex> guard(d->lock);
            d->queueFamilies[*pQueue] = pQueueInfo->queueFamilyIndex;
        }
        std::lock_guard<std::mutex> ownerGuard(g_queueOwnerLock);
        g_queueOwners[*pQueue] = d;
    }
}

// ---------------------------------------------------------------------------
// Swapchain substitution
// ---------------------------------------------------------------------------

extern "C" VKAPI_ATTR VkResult VKAPI_CALL
magpie_CreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR* pCreateInfo,
                          const VkAllocationCallbacks* pAllocator, VkSwapchainKHR* pSwapchain) {
    auto* d = deviceData(device);
    if (!d) return VK_ERROR_INITIALIZATION_FAILED;
    const auto createSwapchain =
        deviceProc<PFN_vkCreateSwapchainKHR>(d, "vkCreateSwapchainKHR");

    // Query the surface's current extent through the next layer/driver.
    VkSurfaceCapabilitiesKHR caps{};
    {
        const auto surfaceCaps = reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR>(
            d->gpiA_next(nullptr, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR"));
        if (!surfaceCaps || surfaceCaps(d->physical, pCreateInfo->surface, &caps) != VK_SUCCESS) {
            LOGW("surface capabilities unavailable; passing swapchain through unsubstituted");
            const VkResult passResult = createSwapchain(device, pCreateInfo, pAllocator, pSwapchain);
            if (passResult == VK_SUCCESS) registerSwapchainOwner(*pSwapchain, d);
            return passResult;
        }
    }

    VkSwapchainCreateInfoKHR realCi = *pCreateInfo;
    realCi.imageExtent = caps.currentExtent;  // Android requires the window's own extent
    realCi.imageUsage |= VK_IMAGE_USAGE_TRANSFER_DST;  // for the FSR/blit copy-in
    // On recreate, the old REAL swapchain retires through the normal mechanism.
    realCi.oldSwapchain = VK_NULL_HANDLE;

    VkSwapchainKHR real = VK_NULL_HANDLE;
    VkResult result = createSwapchain(device, &realCi, pAllocator, &real);
    if (result != VK_SUCCESS) {
        LOGW("swapchain with TRANSFER_DST rejected (%d); retrying with the app's own usage", (int)result);
        realCi.imageUsage = pCreateInfo->imageUsage;
        result = createSwapchain(device, &realCi, pAllocator, &real);
        if (result != VK_SUCCESS) {
            LOGE("real swapchain creation failed: %d", (int)result);
            return result;
        }
        // Without TRANSFER_DST neither copy nor blit into the real image is possible.
        LOGW("real swapchain lacks TRANSFER_DST; layer is in passthrough mode (L3)");
        *pSwapchain = real;
        registerSwapchainOwner(real, d);
        return VK_SUCCESS;
    }

    uint32_t imageCount = 0;
    const auto getSwapchainImages =
        deviceProc<PFN_vkGetSwapchainImagesKHR>(d, "vkGetSwapchainImagesKHR");
    if (getSwapchainImages(device, real, &imageCount, nullptr) != VK_SUCCESS || imageCount == 0) {
        LOGE("real swapchain image count unavailable; passthrough mode (L3)");
        *pSwapchain = real;
        registerSwapchainOwner(real, d);
        return VK_SUCCESS;
    }

    std::vector<VkImage> realImages(imageCount);
    uint32_t fetchedImageCount = imageCount;
    const VkResult fetchImagesResult =
        getSwapchainImages(device, real, &fetchedImageCount, realImages.data());
    if (fetchImagesResult != VK_SUCCESS || fetchedImageCount == 0 ||
        fetchedImageCount > realImages.size()) {
        LOGE("real swapchain image handles unavailable; passthrough mode (L3)");
        *pSwapchain = real;
        registerSwapchainOwner(real, d);
        return VK_SUCCESS;
    }
    realImages.resize(fetchedImageCount);
    imageCount = fetchedImageCount;

    // Decide the mode. FSR requires the app-facing format to be RGBA8_UNORM (the compiled
    // shaders store with an rgba8 qualifier); the engine is created here so the ladder settles
    // before any resource is built.
    Mode mode = Mode::BLIT;
    std::string modeReason = "blit upscale (linear filter)";
    if (!magpieFsrWanted()) {
        modeReason = "FSR disabled by property; blit upscale (linear filter)";
    } else if (pCreateInfo->imageFormat != VK_FORMAT_R8G8B8A8_UNORM) {
        modeReason = "swapchain format is not R8G8B8A8_UNORM; blit upscale (linear filter)";
    } else if (initEngine(d)) {
        mode = Mode::FSR;
        modeReason = "FSR EASU+RCAS";
    } else {
        modeReason = "FSR engine init failed (" + d->failureReason + "); blit upscale";
    }

    const float scale = magpieScale();
    Substitution sub{};
    sub.real = real;
    sub.realExtent = realCi.imageExtent;
    sub.format = realCi.imageFormat;
    sub.imageCount = imageCount;
    sub.realImages = std::move(realImages);
    sub.lowExtent.width = std::max(
        1u, static_cast<uint32_t>(std::lround(static_cast<float>(realCi.imageExtent.width) * scale)));
    sub.lowExtent.height = std::max(
        1u, static_cast<uint32_t>(std::lround(static_cast<float>(realCi.imageExtent.height) * scale)));

    // Fake low-resolution images: the app renders into them (its own usage) and the EASU pass
    // samples them (SAMPLED). If SAMPLED is unsupported at this size, fall back to blit, where
    // the fake image is instead a blit SOURCE (TRANSFER_SRC). If even that fails, L3 passthrough.
    VkImageUsageFlags fakeUsage =
        pCreateInfo->imageUsage | VK_IMAGE_USAGE_SAMPLED | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (mode == Mode::BLIT) {
        fakeUsage = (pCreateInfo->imageUsage | VK_IMAGE_USAGE_TRANSFER_SRC) & ~VK_IMAGE_USAGE_SAMPLED;
    }
    auto create = deviceProc<PFN_vkCreateImage>(d, "vkCreateImage");
    auto allocate = deviceProc<PFN_vkAllocateMemory>(d, "vkAllocateMemory");
    auto bind = deviceProc<PFN_vkBindImageMemory>(d, "vkBindImageMemory");
    auto createView = deviceProc<PFN_vkCreateImageView>(d, "vkCreateImageView");
    auto getMemRequirements = deviceProc<PFN_vkGetImageMemoryRequirements>(d, "vkGetImageMemoryRequirements");
    auto freeMemory = deviceProc<PFN_vkFreeMemory>(d, "vkFreeMemory");
    auto destroyImage = deviceProc<PFN_vkDestroyImage>(d, "vkDestroyImage");

    // Physical-device memory type resolution.
    const auto memProps = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(
        d->gpiA_next(nullptr, "vkGetPhysicalDeviceMemoryProperties"));
    VkPhysicalDeviceMemoryProperties mp{};
    if (memProps) memProps(d->physical, &mp);
    auto chooseMemory = [&](uint32_t bits, VkMemoryPropertyFlags needed, uint32_t* out) {
        if (!memProps) return false;
        for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
            if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & needed) == needed) {
                *out = i;
                return true;
            }
        }
        return false;
    };

    bool fakeOk = true;
    for (uint32_t i = 0; i < imageCount && fakeOk; ++i) {
        SizedImage fake{};
        VkImageCreateInfo ici{};
        ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = pCreateInfo->imageFormat;
        ici.extent = {sub.lowExtent.width, sub.lowExtent.height, 1};
        ici.mipLevels = 1;
        ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = fakeUsage;
        ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (create(device, &ici, nullptr, &fake.image) != VK_SUCCESS) {
            fakeOk = false;
            break;
        }
        VkMemoryRequirements mr{};
        getMemRequirements(device, fake.image, &mr);
        VkMemoryAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.allocationSize = mr.size;
        if (!chooseMemory(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &ai.memoryTypeIndex)) {
            destroyImage(device, fake.image, nullptr);
            fakeOk = false;
            break;
        }
        if (allocate(device, &ai, nullptr, &fake.mem) != VK_SUCCESS ||
            bind(device, fake.image, fake.mem, 0) != VK_SUCCESS) {
            if (fake.mem) freeMemory(device, fake.mem, nullptr);
            destroyImage(device, fake.image, nullptr);
            fakeOk = false;
            break;
        }
        if (mode == Mode::FSR) {
            VkImageViewCreateInfo vi{};
            vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            vi.image = fake.image;
            vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
            vi.format = pCreateInfo->imageFormat;
            vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            if (createView(device, &vi, nullptr, &fake.view) != VK_SUCCESS) {
                freeMemory(device, fake.mem, nullptr);
                destroyImage(device, fake.image, nullptr);
                fakeOk = false;
                break;
            }
        }
        sub.fake.push_back(fake);
    }
    if (!fakeOk) {
        LOGW("low-resolution image creation failed; passthrough mode (L3)");
        destroySubstitution(d, sub);
        // Keep the real swapchain alive: the app must receive a valid passthrough handle.
        *pSwapchain = real;
        registerSwapchainOwner(real, d);
        return VK_SUCCESS;
    }

    if (mode == Mode::FSR) {
        // Engine-owned native-size images + descriptor sets for this swapchain.
        auto makeSized = [&](uint32_t w, uint32_t h, VkImageUsageFlags usage, SizedImage* out) {
            VkImageCreateInfo ici{};
            ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
            ici.imageType = VK_IMAGE_TYPE_2D;
            ici.format = VK_FORMAT_R8G8B8A8_UNORM;
            ici.extent = {w, h, 1};
            ici.mipLevels = 1;
            ici.arrayLayers = 1;
            ici.samples = VK_SAMPLE_COUNT_1_BIT;
            ici.tiling = VK_IMAGE_TILING_OPTIMAL;
            ici.usage = usage;
            ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            if (create(device, &ici, nullptr, &out->image) != VK_SUCCESS) return false;
            VkMemoryRequirements mr{};
            getMemRequirements(device, out->image, &mr);
            VkMemoryAllocateInfo ai{};
            ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            ai.allocationSize = mr.size;
            if (!chooseMemory(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &ai.memoryTypeIndex))
                return false;
            if (allocate(device, &ai, nullptr, &out->mem) != VK_SUCCESS) return false;
            if (bind(device, out->image, out->mem, 0) != VK_SUCCESS) return false;
            VkImageViewCreateInfo vi{};
            vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            vi.image = out->image;
            vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
            vi.format = ici.format;
            vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            return createView(device, &vi, nullptr, &out->view) == VK_SUCCESS;
        };
        if (!makeSized(realCi.imageExtent.width, realCi.imageExtent.height,
                       VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, &sub.middle) ||
            !makeSized(realCi.imageExtent.width, realCi.imageExtent.height,
                       VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, &sub.target)) {
            LOGW("native-resolution FSR images failed; blit mode (L2)");
            mode = Mode::BLIT;
            modeReason = "FSR image creation failed; blit upscale (linear filter)";
        }
    }

    if (mode == Mode::FSR) {
        VkDescriptorPoolSize sizes[] = {
            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2},
            {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2},
        };
        VkDescriptorPoolCreateInfo dpi{};
        dpi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        dpi.maxSets = 2;
        dpi.poolSizeCount = 2;
        dpi.pPoolSizes = sizes;
        const auto createDescriptorPool =
            deviceProc<PFN_vkCreateDescriptorPool>(d, "vkCreateDescriptorPool");
        if (createDescriptorPool(device, &dpi, nullptr, &sub.descriptorPool) != VK_SUCCESS) {
            LOGW("descriptor pool failed; blit mode (L2)");
            mode = Mode::BLIT;
            modeReason = "descriptor pool creation failed; blit upscale (linear filter)";
        } else {
            const auto allocateSets =
                deviceProc<PFN_vkAllocateDescriptorSets>(d, "vkAllocateDescriptorSets");
            const auto updateSets = deviceProc<PFN_vkUpdateDescriptorSets>(d, "vkUpdateDescriptorSets");
            VkDescriptorSetLayout layouts[] = {d->setLayout, d->setLayout};
            VkDescriptorSetAllocateInfo dai{};
            dai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            dai.descriptorPool = sub.descriptorPool;
            dai.descriptorSetCount = 2;
            dai.pSetLayouts = layouts;
            VkDescriptorSet sets[2] = {};
            if (allocateSets(device, &dai, sets) != VK_SUCCESS) {
                LOGW("descriptor sets failed; blit mode (L2)");
                mode = Mode::BLIT;
                modeReason = "descriptor set allocation failed; blit upscale (linear filter)";
            } else {
                sub.easuSet = sets[0];
                sub.rcasSet = sets[1];
                VkDescriptorImageInfo reads[2] = {
                    {d->sampler, sub.fake[0].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                    {d->sampler, sub.middle.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                };
                VkDescriptorImageInfo writes[2] = {
                    {VK_NULL_HANDLE, sub.middle.view, VK_IMAGE_LAYOUT_GENERAL},
                    {VK_NULL_HANDLE, sub.target.view, VK_IMAGE_LAYOUT_GENERAL},
                };
                VkWriteDescriptorSet wr[4]{};
                for (int k = 0; k < 2; ++k) {
                    wr[k * 2] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                    wr[k * 2].dstSet = sets[k];
                    wr[k * 2].dstBinding = 0;
                    wr[k * 2].descriptorCount = 1;
                    wr[k * 2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                    wr[k * 2].pImageInfo = &reads[k];
                    wr[k * 2 + 1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                    wr[k * 2 + 1].dstSet = sets[k];
                    wr[k * 2 + 1].dstBinding = 1;
                    wr[k * 2 + 1].descriptorCount = 1;
                    wr[k * 2 + 1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                    wr[k * 2 + 1].pImageInfo = &writes[k];
                }
                updateSets(device, 4, wr, 0, nullptr);
            }
        }
        if (mode == Mode::FSR) {
            // Present semaphores are created for the FINAL mode below (FSR and BLIT both need
            // the layer's own signal semaphore for the present-time chain).
        }
    }

    if (mode != Mode::PASS) {
        const auto createSemaphore = deviceProc<PFN_vkCreateSemaphore>(d, "vkCreateSemaphore");
        bool semsOk = true;
        for (uint32_t i = 0; i < imageCount && semsOk; ++i) {
            VkSemaphoreCreateInfo sci{};
            sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
            VkSemaphore sem = VK_NULL_HANDLE;
            if (createSemaphore(device, &sci, nullptr, &sem) != VK_SUCCESS) {
                semsOk = false;
            } else {
                sub.presentSems.push_back(sem);
            }
        }
        if (!semsOk) {
            LOGW("present semaphore creation failed; passthrough mode (L3)");
            destroySubstitution(d, sub);
            // Preserve the native swapchain and register it for safe QueuePresent passthrough.
            *pSwapchain = real;
            registerSwapchainOwner(real, d);
            return VK_SUCCESS;
        }
    }

    sub.mode = mode;
    {
        std::lock_guard<std::mutex> guard(d->lock);
        d->substitutions[real] = std::move(sub);
        std::lock_guard<std::mutex> ownerGuard(g_swapchainOwnerLock);
        g_swapchainOwners[real] = d;
    }
    // The app receives the REAL swapchain handle for acquire/present, but fake image handles
    // from vkGetSwapchainImagesKHR — the substitution is invisible to the app.
    *pSwapchain = real;
    LOGI("swapchain substituted: %ux%u -> %ux%u, mode=%s (%s), images=%u",
         realCi.imageExtent.width, realCi.imageExtent.height,
         sub.lowExtent.width, sub.lowExtent.height,
         mode == Mode::FSR ? "FSR" : (mode == Mode::BLIT ? "BLIT" : "PASS"),
         modeReason.c_str(), imageCount);
    return VK_SUCCESS;
}

extern "C" VKAPI_ATTR VkResult VKAPI_CALL
magpie_GetSwapchainImagesKHR(VkDevice device, VkSwapchainKHR swapchain, uint32_t* pCount,
                             VkImage* pSwapchainImages) {
    auto* d = deviceData(device);
    if (!d || !pCount) return VK_ERROR_INITIALIZATION_FAILED;
    Substitution* sub = nullptr;
    {
        std::lock_guard<std::mutex> guard(d->lock);
        const auto it = d->substitutions.find(swapchain);
        if (it != d->substitutions.end()) sub = &it->second;
    }
    if (!sub || sub->mode == Mode::PASS) {
        // Passthrough (no substitution, or the ladder settled to L3): the app sees the real
        // native-resolution images.
        return deviceProc<PFN_vkGetSwapchainImagesKHR>(d, "vkGetSwapchainImagesKHR")(
            device, swapchain, pCount, pSwapchainImages);
    }
    if (pSwapchainImages == nullptr) {
        *pCount = sub->imageCount;
        return VK_SUCCESS;
    }
    if (*pCount > sub->imageCount) *pCount = sub->imageCount;
    for (uint32_t i = 0; i < *pCount; ++i) pSwapchainImages[i] = sub->fake[i].image;
    return sub->imageCount <= *pCount ? VK_SUCCESS : VK_INCOMPLETE;
}

extern "C" VKAPI_ATTR VkResult VKAPI_CALL
magpie_DestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain,
                           const VkAllocationCallbacks* pAllocator) {
    auto* d = deviceData(device);
    if (d) {
        std::lock_guard<std::mutex> guard(d->lock);
        const VkResult idleResult =
            deviceProc<PFN_vkDeviceWaitIdle>(d, "vkDeviceWaitIdle")(device);
        if (idleResult != VK_SUCCESS)
            LOGE("vkDeviceWaitIdle before swapchain destruction returned %d", (int)idleResult);
        d->commandPending = false;
        const auto it = d->substitutions.find(swapchain);
        if (it != d->substitutions.end()) {
            destroySubstitution(d, it->second);
            d->substitutions.erase(it);
        }
        std::lock_guard<std::mutex> ownerGuard(g_swapchainOwnerLock);
        g_swapchainOwners.erase(swapchain);
        deviceProc<PFN_vkDestroySwapchainKHR>(d, "vkDestroySwapchainKHR")(device, swapchain, pAllocator);
        return VK_SUCCESS;
    }
    reinterpret_cast<PFN_vkDestroySwapchainKHR>(
        vkGetInstanceProcAddr(nullptr, "vkDestroySwapchainKHR"))(device, swapchain, pAllocator);
    return VK_SUCCESS;
}

// ---------------------------------------------------------------------------
// Present-time processing: the core of the pipeline.
// ---------------------------------------------------------------------------

namespace {

struct EasuPush {
    uint32_t con[16];
    uint32_t size[2];
};
struct RcasPush {
    uint32_t con[4];
    uint32_t size[2];
};

// One-time per-device command-pool creation on the presenting queue's family. The present queue
// is the queue the app itself chose for presentation, which on Android is graphics-capable and,
// on Mali, compute-capable; the family flags were recorded when the app called GetDeviceQueue.
bool ensureCommandBuffer(DeviceData* d, VkQueue queue) {
    if (d->pool != VK_NULL_HANDLE && d->command != VK_NULL_HANDLE &&
        d->submitFence != VK_NULL_HANDLE) return true;
    // A prior partial initialization failed. Release its incomplete resources before retrying.
    if (d->submitFence) {
        deviceProc<PFN_vkDestroyFence>(d, "vkDestroyFence")(d->device, d->submitFence, nullptr);
        d->submitFence = VK_NULL_HANDLE;
    }
    if (d->queries) {
        deviceProc<PFN_vkDestroyQueryPool>(d, "vkDestroyQueryPool")(d->device, d->queries, nullptr);
        d->queries = VK_NULL_HANDLE;
    }
    if (d->pool) {
        deviceProc<PFN_vkDestroyCommandPool>(d, "vkDestroyCommandPool")(d->device, d->pool, nullptr);
        d->pool = VK_NULL_HANDLE;
        d->command = VK_NULL_HANDLE;
    }
    uint32_t family = 0;
    {
        const auto it = d->queueFamilies.find(queue);
        if (it != d->queueFamilies.end()) family = it->second;
        else LOGW("present queue family unknown; assuming family 0");
    }
    VkCommandPoolCreateInfo pci{};
    pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = family;
    const auto createPool = deviceProc<PFN_vkCreateCommandPool>(d, "vkCreateCommandPool");
    const auto allocate = deviceProc<PFN_vkAllocateCommandBuffers>(d, "vkAllocateCommandBuffers");
    if (createPool(d->device, &pci, nullptr, &d->pool) != VK_SUCCESS) {
        d->failureReason = "layer command pool creation failed";
        return false;
    }
    VkCommandBufferAllocateInfo cai{};
    cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cai.commandPool = d->pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    if (allocate(d->device, &cai, &d->command) != VK_SUCCESS) {
        d->failureReason = "layer command buffer allocation failed";
        return false;
    }
    // Timestamp queries are best-effort: failure only disables timing, not processing.
    VkQueryPoolCreateInfo qp{};
    qp.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    qp.queryType = VK_QUERY_TYPE_TIMESTAMP;
    qp.queryCount = 4;
    const auto createQueryPool = deviceProc<PFN_vkCreateQueryPool>(d, "vkCreateQueryPool");
    if (createQueryPool(d->device, &qp, nullptr, &d->queries) != VK_SUCCESS) d->queries = VK_NULL_HANDLE;

    VkFenceCreateInfo fci{};
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    const auto createFence = deviceProc<PFN_vkCreateFence>(d, "vkCreateFence");
    if (createFence(d->device, &fci, nullptr, &d->submitFence) != VK_SUCCESS) {
        d->failureReason = "layer submission fence creation failed";
        return false;
    }
    return true;
}

}  // namespace

extern "C" VKAPI_ATTR VkResult VKAPI_CALL
magpie_QueuePresentKHR(VkQueue queue, const VkPresentInfoKHR* pPresentInfo) {
    // Locate the owning device through the first swapchain in the present info.
    DeviceData* d = nullptr;
    if (pPresentInfo->swapchainCount > 0) {
        std::lock_guard<std::mutex> ownerGuard(g_swapchainOwnerLock);
        const auto it = g_swapchainOwners.find(pPresentInfo->pSwapchains[0]);
        if (it != g_swapchainOwners.end()) d = it->second;
    }
    if (!d) {
        std::lock_guard<std::mutex> queueGuard(g_queueOwnerLock);
        const auto queueIt = g_queueOwners.find(queue);
        if (queueIt != g_queueOwners.end()) d = queueIt->second;
    }
    if (!d) return VK_ERROR_INITIALIZATION_FAILED;

    Substitution* sub = nullptr;
    uint32_t index = 0;
    {
        std::lock_guard<std::mutex> guard(d->lock);
        // Single-swapchain presents are the app contract here; multi-swapchain presents pass
        // through untouched rather than guessing synchronization across chains.
        if (pPresentInfo->swapchainCount == 1) {
            const auto it = d->substitutions.find(pPresentInfo->pSwapchains[0]);
            if (it != d->substitutions.end() && it->second.mode != Mode::PASS) {
                sub = &it->second;
                index = pPresentInfo->pImageIndices[0];
            }
        }
        if (!sub) {
            const auto present = deviceProc<PFN_vkQueuePresentKHR>(d, "vkQueuePresentKHR");
            return present(queue, pPresentInfo);
        }
        if (index >= sub->imageCount || index >= sub->fake.size() ||
            index >= sub->realImages.size() || index >= sub->presentSems.size()) {
            LOGE("present image index %u is outside substitution resources; passthrough", index);
            const auto present = deviceProc<PFN_vkQueuePresentKHR>(d, "vkQueuePresentKHR");
            return present(queue, pPresentInfo);
        }

        // Engine failure latched at any earlier point: passthrough with the app's own
        // semaphores, which is always safe (the app's render→present dependency is preserved).
        if (d->engineFailed || (sub->mode == Mode::FSR && !d->engineReady)) {
            if (sub->mode == Mode::FSR) {
                sub->mode = Mode::BLIT;
                LOGW("FSR engine unavailable at present time (%s); degrading to blit",
                     d->failureReason.c_str());
            }
        }

        const VkImage fakeImage = sub->fake[index].image;
        const VkImage realImage = sub->realImages[index];
        const VkExtent2D low = sub->lowExtent;

        if (sub->mode == Mode::FSR && !d->engineReady) {
            if (!initEngine(d)) {
                sub->mode = Mode::BLIT;
                LOGW("FSR engine init deferred-failed (%s); degrading to blit",
                     d->failureReason.c_str());
            }
        }

        const auto barrier = deviceProc<PFN_vkCmdPipelineBarrier>(d, "vkCmdPipelineBarrier");
        const auto submit = deviceProc<PFN_vkQueueSubmit>(d, "vkQueueSubmit");
        const auto begin = deviceProc<PFN_vkBeginCommandBuffer>(d, "vkBeginCommandBuffer");
        const auto end = deviceProc<PFN_vkEndCommandBuffer>(d, "vkEndCommandBuffer");
        const auto resetCb = deviceProc<PFN_vkResetCommandBuffer>(d, "vkResetCommandBuffer");
        const auto bindPipeline = deviceProc<PFN_vkCmdBindPipeline>(d, "vkCmdBindPipeline");
        const auto bindSets = deviceProc<PFN_vkCmdBindDescriptorSets>(d, "vkCmdBindDescriptorSets");
        const auto pushConstants = deviceProc<PFN_vkCmdPushConstants>(d, "vkCmdPushConstants");
        const auto dispatch = deviceProc<PFN_vkCmdDispatch>(d, "vkCmdDispatch");
        const auto writeTimestamp = deviceProc<PFN_vkCmdWriteTimestamp>(d, "vkCmdWriteTimestamp");
        const auto resetQueryPool = deviceProc<PFN_vkCmdResetQueryPool>(d, "vkCmdResetQueryPool");
        const auto getQueryResults = deviceProc<PFN_vkGetQueryPoolResults>(d, "vkGetQueryPoolResults");
        const auto updateDescriptorSets =
            deviceProc<PFN_vkUpdateDescriptorSets>(d, "vkUpdateDescriptorSets");
        const auto cmdCopyImage = deviceProc<PFN_vkCmdCopyImage>(d, "vkCmdCopyImage");
        const auto cmdBlitImage = deviceProc<PFN_vkCmdBlitImage>(d, "vkCmdBlitImage");

        if (d->pool == VK_NULL_HANDLE && !ensureCommandBuffer(d, queue)) {
            sub->mode = Mode::PASS;
            LOGE("layer command buffer unavailable (%s); passthrough from now on",
                 d->failureReason.c_str());
            const auto present = deviceProc<PFN_vkQueuePresentKHR>(d, "vkQueuePresentKHR");
            return present(queue, pPresentInfo);
        }

        // Read the previous frame's timestamps without blocking (no WAIT bit): timing is
        // diagnostic and must never stall the present path.
        if (d->queries != VK_NULL_HANDLE && sub->timingsValid) {
            uint64_t ts[4] = {};
            if (getQueryResults(d->device, d->queries, 0, 4, sizeof(ts), ts, sizeof(uint64_t),
                                VK_QUERY_RESULT_64_BIT) == VK_SUCCESS &&
                d->timestampPeriodNanos > 0.0 && ts[3] > ts[0]) {
                sub->easuMs = static_cast<double>(ts[1] - ts[0]) * d->timestampPeriodNanos / 1e6;
                sub->rcasMs = static_cast<double>(ts[2] - ts[1]) * d->timestampPeriodNanos / 1e6;
                sub->dispatchedFrames++;
                if (sub->dispatchedFrames % 300 == 0) {
                    LOGI("FSR stats: frames=%llu easu=%.3fms rcas=%.3fms", 
                         static_cast<unsigned long long>(sub->dispatchedFrames),
                         sub->easuMs, sub->rcasMs);
                }
            }
            sub->timingsValid = false;
        }

        // The previous submission may still be executing. Never reset or re-record a pending
        // command buffer; wait only for the layer's own prior work, not the whole device.
        if (d->commandPending) {
            const auto waitForFences = deviceProc<PFN_vkWaitForFences>(d, "vkWaitForFences");
            const VkResult waitResult = waitForFences(d->device, 1, &d->submitFence,
                                                       VK_TRUE, UINT64_MAX);
            if (waitResult != VK_SUCCESS) {
                LOGE("layer submission fence wait failed (%d); passthrough", (int)waitResult);
                const auto present = deviceProc<PFN_vkQueuePresentKHR>(d, "vkQueuePresentKHR");
                return present(queue, pPresentInfo);
            }
            d->commandPending = false;
        }
        // EASU must sample the image acquired for this frame, not always fake[0]. The previous
        // layer submission is complete (fence above), so updating this descriptor is safe.
        if (sub->mode == Mode::FSR) {
            VkDescriptorImageInfo inputInfo{
                d->sampler, sub->fake[index].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            VkWriteDescriptorSet inputWrite{};
            inputWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            inputWrite.dstSet = sub->easuSet;
            inputWrite.dstBinding = 0;
            inputWrite.descriptorCount = 1;
            inputWrite.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            inputWrite.pImageInfo = &inputInfo;
            updateDescriptorSets(d->device, 1, &inputWrite, 0, nullptr);
        }
        resetCb(d->command, VK_COMMAND_BUFFER_RESET_RELEASE_RESOURCES_BIT);
        VkCommandBufferBeginInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (begin(d->command, &bi) != VK_SUCCESS) {
            LOGE("command buffer begin failed; passthrough for this frame");
            const auto present = deviceProc<PFN_vkQueuePresentKHR>(d, "vkQueuePresentKHR");
            return present(queue, pPresentInfo);
        }

        if (d->queries != VK_NULL_HANDLE) resetQueryPool(d->command, d->queries, 0, 4);

        if (sub->mode == Mode::FSR) {
            // fake: the app's render pass ended with finalLayout=PRESENT_SRC_KHR, so the image
            // is in PRESENT_SRC here. Transitioning away from PRESENT_SRC between acquire and
            // present is spec-expected ("Upon reacquiring the image and transitioning it away
            // from the PRESENT_SRC layout, the contents will be the same"); srcAccessMask must
            // be 0 for a source layout of PRESENT_SRC. The semaphore wait on the app's
            // render-signal semaphore already orders the app's color writes before this barrier.
            // The app's render-signal semaphore is waited at ALL_COMMANDS in this submission,
            // providing the cross-submit memory dependency for its final color writes.
            recordBarrier(d->command, fakeImage, d->fakeFinalLayout,
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, VK_ACCESS_SHADER_READ_BIT,
                          VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, barrier);
            recordBarrier(d->command, sub->middle.image, VK_IMAGE_LAYOUT_UNDEFINED,
                          VK_IMAGE_LAYOUT_GENERAL, 0, VK_ACCESS_SHADER_WRITE_BIT,
                          VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                          barrier);
            recordBarrier(d->command, sub->target.image, VK_IMAGE_LAYOUT_UNDEFINED,
                          VK_IMAGE_LAYOUT_GENERAL, 0, VK_ACCESS_SHADER_WRITE_BIT,
                          VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                          barrier);
            if (d->queries != VK_NULL_HANDLE)
                writeTimestamp(d->command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, d->queries, 0);
            bindPipeline(d->command, VK_PIPELINE_BIND_POINT_COMPUTE, d->easuPipeline);
            bindSets(d->command, VK_PIPELINE_BIND_POINT_COMPUTE, d->pipelineLayout, 0, 1,
                     &sub->easuSet, 0, nullptr);
            EasuPush ep{};
            fillEasuConstants(ep.con, low.width, low.height, sub->realExtent.width,
                              sub->realExtent.height);
            ep.size[0] = sub->realExtent.width;
            ep.size[1] = sub->realExtent.height;
            pushConstants(d->command, d->pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                          sizeof(ep), &ep);
            dispatch(d->command,
                     (sub->realExtent.width + kWorkgroup - 1) / kWorkgroup,
                     (sub->realExtent.height + kWorkgroup - 1) / kWorkgroup, 1);
            if (d->queries != VK_NULL_HANDLE)
                writeTimestamp(d->command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, d->queries, 1);
            recordBarrier(d->command, sub->middle.image, VK_IMAGE_LAYOUT_GENERAL,
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_SHADER_WRITE_BIT,
                          VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, barrier);
            bindPipeline(d->command, VK_PIPELINE_BIND_POINT_COMPUTE, d->rcasPipeline);
            bindSets(d->command, VK_PIPELINE_BIND_POINT_COMPUTE, d->pipelineLayout, 0, 1,
                     &sub->rcasSet, 0, nullptr);
            RcasPush rp{};
            rp.con[0] = floatBits(1.0f);  // 0 sharpness stops (matches the validated fixtures)
            rp.con[1] = floatBits(1.0f);
            rp.con[2] = 0;
            rp.con[3] = 0;
            rp.size[0] = sub->realExtent.width;
            rp.size[1] = sub->realExtent.height;
            pushConstants(d->command, d->pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                          sizeof(rp), &rp);
            dispatch(d->command,
                     (sub->realExtent.width + kWorkgroup - 1) / kWorkgroup,
                     (sub->realExtent.height + kWorkgroup - 1) / kWorkgroup, 1);
            if (d->queries != VK_NULL_HANDLE)
                writeTimestamp(d->command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, d->queries, 2);
            recordBarrier(d->command, sub->target.image, VK_IMAGE_LAYOUT_GENERAL,
                          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_SHADER_WRITE_BIT,
                          VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                          VK_PIPELINE_STAGE_TRANSFER_BIT, barrier);
            recordBarrier(d->command, realImage, VK_IMAGE_LAYOUT_UNDEFINED,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                          VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                          barrier);
            VkImageCopy copy{};
            copy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy.srcOffset = {0, 0, 0};
            copy.dstOffset = {0, 0, 0};
            copy.extent = {sub->realExtent.width, sub->realExtent.height, 1};
            cmdCopyImage(d->command, sub->target.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         realImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
            if (d->queries != VK_NULL_HANDLE)
                writeTimestamp(d->command, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, d->queries, 3);
        } else if (sub->mode == Mode::BLIT) {
            // fake: PRESENT_SRC → TRANSFER_SRC (same reasoning as the FSR barrier above);
            // blit scales low→native with a linear filter (bilinear upscale, no FSR).
            recordBarrier(d->command, fakeImage, d->fakeFinalLayout,
                          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0, VK_ACCESS_TRANSFER_READ_BIT,
                          VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                          VK_PIPELINE_STAGE_TRANSFER_BIT, barrier);
            recordBarrier(d->command, realImage, VK_IMAGE_LAYOUT_UNDEFINED,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                          VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                          barrier);
            VkImageBlit blit{};
            blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            blit.srcOffsets[1] = {static_cast<int32_t>(low.width), static_cast<int32_t>(low.height), 1};
            blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            blit.dstOffsets[1] = {static_cast<int32_t>(sub->realExtent.width),
                                  static_cast<int32_t>(sub->realExtent.height), 1};
            cmdBlitImage(d->command, fakeImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, realImage,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
        }

        // Restore the real image to PRESENT_SRC_KHR before presenting (required).
        recordBarrier(d->command, realImage,
                      sub->mode == Mode::FSR ? VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL
                                             : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_ACCESS_TRANSFER_WRITE_BIT, 0,
                      VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                      barrier);
        if (end(d->command, &bi) != VK_SUCCESS) {
            LOGE("command buffer end failed; passthrough for this frame");
            const auto present = deviceProc<PFN_vkQueuePresentKHR>(d, "vkQueuePresentKHR");
            return present(queue, pPresentInfo);
        }

        // Semaphore chain: wait the app's render-signal semaphores, signal ours for the present.
        VkSemaphore layerSem = sub->presentSems[index];
        VkSubmitInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.waitSemaphoreCount = pPresentInfo->waitSemaphoreCount;
        si.pWaitSemaphores = pPresentInfo->pWaitSemaphores;
        std::vector<VkPipelineStageFlags> waitStages(
            pPresentInfo->waitSemaphoreCount, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
        si.pWaitDstStageMask = waitStages.empty() ? nullptr : waitStages.data();
        si.commandBufferCount = 1;
        si.pCommandBuffers = &d->command;
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &layerSem;
        const auto resetFences = deviceProc<PFN_vkResetFences>(d, "vkResetFences");
        const VkResult resetFenceResult = resetFences(d->device, 1, &d->submitFence);
        if (resetFenceResult != VK_SUCCESS) {
            LOGE("layer submission fence reset failed (%d); passthrough", (int)resetFenceResult);
            const auto present = deviceProc<PFN_vkQueuePresentKHR>(d, "vkQueuePresentKHR");
            return present(queue, pPresentInfo);
        }
        const VkResult submitResult = submit(queue, 1, &si, d->submitFence);
        if (submitResult != VK_SUCCESS) {
            LOGE("layer submit failed (%d); passthrough with app semaphores", (int)submitResult);
            sub->mode = sub->mode == Mode::FSR ? Mode::BLIT : sub->mode;
            const auto present = deviceProc<PFN_vkQueuePresentKHR>(d, "vkQueuePresentKHR");
            return present(queue, pPresentInfo);
        }
        d->commandPending = true;
        sub->timingsValid = sub->mode == Mode::FSR && d->queries != VK_NULL_HANDLE;

        VkPresentInfoKHR presentInfo = *pPresentInfo;
        presentInfo.waitSemaphoreCount = 1;
        presentInfo.pWaitSemaphores = &layerSem;
        const auto present = deviceProc<PFN_vkQueuePresentKHR>(d, "vkQueuePresentKHR");
        return present(queue, &presentInfo);
    }
}

// The device-chain entry point. The loader resolves every device-level function the app calls
// through this function; the layer returns its hooks for the intercepted functions and defers
// everything else to the next layer/driver.
extern "C" VKAPI_ATTR void* VKAPI_CALL
magpie_GetDeviceProcAddr(VkDevice device, const char* name) {
    if (!name) return nullptr;
    if (strcmp(name, "vkGetDeviceProcAddr") == 0)
        return reinterpret_cast<void*>(&magpie_GetDeviceProcAddr);
    if (strcmp(name, "vkMagpieSetFakeImageFinalLayout") == 0)
        return reinterpret_cast<void*>(&magpie_SetFakeImageFinalLayout);
    if (strcmp(name, "vkMagpieGetSwapchainRenderExtent") == 0)
        return reinterpret_cast<void*>(&magpie_GetSwapchainRenderExtent);
    if (strcmp(name, "vkDestroyDevice") == 0)
        return reinterpret_cast<void*>(&magpie_DestroyDevice);
    if (strcmp(name, "vkGetDeviceQueue") == 0)
        return reinterpret_cast<void*>(&magpie_GetDeviceQueue);
    if (strcmp(name, "vkGetDeviceQueue2") == 0)
        return reinterpret_cast<void*>(&magpie_GetDeviceQueue2);
    if (strcmp(name, "vkCreateSwapchainKHR") == 0)
        return reinterpret_cast<void*>(&magpie_CreateSwapchainKHR);
    if (strcmp(name, "vkDestroySwapchainKHR") == 0)
        return reinterpret_cast<void*>(&magpie_DestroySwapchainKHR);
    if (strcmp(name, "vkGetSwapchainImagesKHR") == 0)
        return reinterpret_cast<void*>(&magpie_GetSwapchainImagesKHR);
    if (strcmp(name, "vkQueuePresentKHR") == 0)
        return reinterpret_cast<void*>(&magpie_QueuePresentKHR);
    auto* d = deviceData(device);
    if (!d || !d->gdpA_next) return nullptr;
    return d->gdpA_next(device, name);
}
