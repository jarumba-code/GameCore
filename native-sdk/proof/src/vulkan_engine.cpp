// M4: persistent in-app Vulkan FSR engine. The validated M2 dispatch sequence
// (tools/fsr1-reference/vulkan_runner.cpp) promoted into a lifecycle-owned engine: the instance,
// device, compute queue, command objects, pipelines and descriptor resources are created once and
// reused; only the sized resources (three images, two staging buffers, one descriptor pool) are
// recreated on resize. The two AMD FSR 1.0.2 compute passes stay separate, the intermediate stays
// GPU-resident, every layout change is an explicit barrier, and readback happens only in the
// caller's process step — there is no swapchain interception here and no claim about any game.
//
// Dispatch validation is an earned flag: fsr_dispatch_validated becomes 1 after at least one
// full submit → fence-wait → readback cycle has succeeded on this engine instance, and returns
// to 0 if the device is lost.
#include "magpie/magpie.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <new>
#include <string>
#include <vector>
#include <vulkan/vulkan.h>

namespace {
constexpr uint32_t kWorkgroupSize = 8;   // the FSR shaders dispatch 8x8 groups
constexpr uint32_t kMaxSharpnessStops = 8;

std::string vkResultName(VkResult r) {
    return "VkResult=" + std::to_string(static_cast<int>(r));
}

struct SizedImage {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
};

struct HostBuffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
};

}  // namespace

// The engine struct is defined at global scope, matching magpie.h's forward declaration: an
// anonymous-namespace definition would be a *different* type from the header's, and the extern
// "C" functions below would be ambiguous between the two.
struct MagpieVulkanFsrEngineV1 {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t family = UINT32_MAX;
    VkPhysicalDeviceProperties properties{};
    VkPhysicalDeviceMemoryProperties memoryProperties{};
    double timestampPeriodNanos = 0.0;

    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer command = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    VkQueryPool queries = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkShaderModule easuModule = VK_NULL_HANDLE;
    VkShaderModule rcasModule = VK_NULL_HANDLE;
    VkPipeline easuPipeline = VK_NULL_HANDLE;
    VkPipeline rcasPipeline = VK_NULL_HANDLE;

    // Sized (recreated on resize).
    SizedImage source{};   // TRANSFER_DST | SAMPLED
    SizedImage middle{};   // STORAGE | SAMPLED — the GPU-resident EASU intermediate
    SizedImage target{};   // STORAGE | TRANSFER_SRC
    HostBuffer upload{};   // staging: input bytes
    HostBuffer readback{}; // staging: output bytes
    VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
    VkDescriptorSet easuSet = VK_NULL_HANDLE;
    VkDescriptorSet rcasSet = VK_NULL_HANDLE;

    uint32_t inWidth = 0, inHeight = 0, outWidth = 0, outHeight = 0;
    uint64_t deviceImageBytes = 0;
    uint64_t hostBufferBytes = 0;
    uint32_t successfulDispatches = 0;
    bool deviceLost = false;
    std::string lastFailure;

    ~MagpieVulkanFsrEngineV1() { teardownAll(); }

    static MagpieStatusV1 chooseMemoryType(const VkPhysicalDeviceMemoryProperties& mp,
                                           uint32_t bits, VkMemoryPropertyFlags needed,
                                           uint32_t* out) {
        for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
            if ((bits & (1u << i)) &&
                (mp.memoryTypes[i].propertyFlags & needed) == needed) {
                *out = i;
                return MAGPIE_OK;
            }
        }
        return MAGPIE_UNAVAILABLE;
    }

    MagpieStatusV1 initCore(const MagpieVulkanFsrEngineConfigV1& cfg) {
        VkApplicationInfo app{};
        app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        app.pApplicationName = "Magpie Android FSR Engine";
        app.apiVersion = VK_API_VERSION_1_1;
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
        vkGetPhysicalDeviceMemoryProperties(physical, &memoryProperties);
        timestampPeriodNanos = properties.limits.timestampPeriod;

        VkFormatProperties format{};
        vkGetPhysicalDeviceFormatProperties(physical, VK_FORMAT_R8G8B8A8_UNORM, &format);
        const VkFormatFeatureFlags required =
            VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT;
        if ((format.optimalTilingFeatures & required) != required ||
            properties.limits.maxComputeWorkGroupInvocations < 64 ||
            properties.limits.maxComputeWorkGroupSize[0] < kWorkgroupSize ||
            properties.limits.maxComputeWorkGroupSize[1] < kWorkgroupSize ||
            properties.limits.maxImageDimension2D < cfg.output_width ||
            properties.limits.maxImageDimension2D < cfg.output_height) {
            lastFailure = "device limits do not support RGBA8 sampled+storage or the 8x8 FSR workgroup at this size";
            return MAGPIE_UNAVAILABLE;
        }

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
        if (vkCreateDevice(physical, &dci, nullptr, &device) != VK_SUCCESS) return MAGPIE_UNAVAILABLE;
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

        VkSamplerCreateInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        si.magFilter = si.minFilter = VK_FILTER_NEAREST;
        si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.maxLod = 0;
        if (vkCreateSampler(device, &si, nullptr, &sampler) != VK_SUCCESS)
            return MAGPIE_INTERNAL_ERROR;

        VkQueryPoolCreateInfo qp{};
        qp.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        qp.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qp.queryCount = 4;
        if (vkCreateQueryPool(device, &qp, nullptr, &queries) != VK_SUCCESS)
            return MAGPIE_INTERNAL_ERROR;
        return MAGPIE_OK;
    }

    MagpieStatusV1 initPipelines(const MagpieVulkanFsrEngineConfigV1& cfg) {
        VkDescriptorSetLayoutBinding binds[2]{};
        binds[0] = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                    VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        binds[1] = {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1,
                    VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo sli{};
        sli.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        sli.bindingCount = 2;
        sli.pBindings = binds;
        if (vkCreateDescriptorSetLayout(device, &sli, nullptr, &setLayout) != VK_SUCCESS)
            return MAGPIE_INTERNAL_ERROR;

        VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT, 0, 72};
        VkPipelineLayoutCreateInfo lci{};
        lci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        lci.setLayoutCount = 1;
        lci.pSetLayouts = &setLayout;
        lci.pushConstantRangeCount = 1;
        lci.pPushConstantRanges = &pcr;
        if (vkCreatePipelineLayout(device, &lci, nullptr, &pipelineLayout) != VK_SUCCESS)
            return MAGPIE_INTERNAL_ERROR;

        VkShaderModuleCreateInfo smi{};
        smi.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        smi.codeSize = static_cast<size_t>(cfg.easu_spirv_word_count) * 4u;
        smi.pCode = cfg.easu_spirv_words;
        if (vkCreateShaderModule(device, &smi, nullptr, &easuModule) != VK_SUCCESS)
            return MAGPIE_INTERNAL_ERROR;
        smi.pCode = cfg.rcas_spirv_words;
        smi.codeSize = static_cast<size_t>(cfg.rcas_spirv_word_count) * 4u;
        if (vkCreateShaderModule(device, &smi, nullptr, &rcasModule) != VK_SUCCESS)
            return MAGPIE_INTERNAL_ERROR;

        const auto makePipeline = [this](VkShaderModule module, VkPipeline* out) -> VkResult {
            VkPipelineShaderStageCreateInfo st{};
            st.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            st.stage = VK_SHADER_STAGE_COMPUTE_BIT;
            st.module = module;
            st.pName = "main";
            VkComputePipelineCreateInfo ci{};
            ci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
            ci.stage = st;
            ci.layout = pipelineLayout;
            return vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &ci, nullptr, out);
        };
        if (makePipeline(easuModule, &easuPipeline) != VK_SUCCESS ||
            makePipeline(rcasModule, &rcasPipeline) != VK_SUCCESS) {
            lastFailure = "compute pipeline creation failed";
            return MAGPIE_INTERNAL_ERROR;
        }
        return MAGPIE_OK;
    }

    MagpieStatusV1 makeImage(uint32_t w, uint32_t h, VkImageUsageFlags usage, SizedImage* out) {
        VkImageCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ci.imageType = VK_IMAGE_TYPE_2D;
        ci.format = VK_FORMAT_R8G8B8A8_UNORM;
        ci.extent = {w, h, 1};
        ci.mipLevels = 1;
        ci.arrayLayers = 1;
        ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        ci.usage = usage;
        ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(device, &ci, nullptr, &out->image) != VK_SUCCESS) return MAGPIE_INTERNAL_ERROR;
        VkMemoryRequirements mr{};
        vkGetImageMemoryRequirements(device, out->image, &mr);
        VkMemoryAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.allocationSize = mr.size;
        const auto status = chooseMemoryType(memoryProperties, mr.memoryTypeBits,
                                             VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                             &ai.memoryTypeIndex);
        if (status != MAGPIE_OK) return status;
        deviceImageBytes += mr.size;
        if (vkAllocateMemory(device, &ai, nullptr, &out->mem) != VK_SUCCESS) return MAGPIE_INTERNAL_ERROR;
        if (vkBindImageMemory(device, out->image, out->mem, 0) != VK_SUCCESS) return MAGPIE_INTERNAL_ERROR;
        VkImageViewCreateInfo vi{};
        vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vi.image = out->image;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = ci.format;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (vkCreateImageView(device, &vi, nullptr, &out->view) != VK_SUCCESS) return MAGPIE_INTERNAL_ERROR;
        return MAGPIE_OK;
    }

    MagpieStatusV1 makeHostBuffer(uint64_t bytes, VkBufferUsageFlags usage, HostBuffer* out) {
        VkBufferCreateInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bi.size = bytes;
        bi.usage = usage;
        bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(device, &bi, nullptr, &out->buffer) != VK_SUCCESS) return MAGPIE_INTERNAL_ERROR;
        VkMemoryRequirements rq{};
        vkGetBufferMemoryRequirements(device, out->buffer, &rq);
        VkMemoryAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.allocationSize = rq.size;
        const auto status = chooseMemoryType(memoryProperties, rq.memoryTypeBits,
                                             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                 VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                             &ai.memoryTypeIndex);
        if (status != MAGPIE_OK) return status;
        hostBufferBytes += rq.size;
        if (vkAllocateMemory(device, &ai, nullptr, &out->mem) != VK_SUCCESS) return MAGPIE_INTERNAL_ERROR;
        if (vkBindBufferMemory(device, out->buffer, out->mem, 0) != VK_SUCCESS) return MAGPIE_INTERNAL_ERROR;
        return MAGPIE_OK;
    }

    void destroySized() {
        struct Item { VkImageView view; VkImage image; VkDeviceMemory mem; };
        const Item images[] = {
            {source.view, source.image, source.mem},
            {middle.view, middle.image, middle.mem},
            {target.view, target.image, target.mem},
        };
        for (const Item& it : images) {
            if (it.view) vkDestroyImageView(device, it.view, nullptr);
            if (it.image) vkDestroyImage(device, it.image, nullptr);
            if (it.mem) vkFreeMemory(device, it.mem, nullptr);
        }
        source = {}; middle = {}; target = {};
        for (HostBuffer* b : {&upload, &readback}) {
            if (b->buffer) vkDestroyBuffer(device, b->buffer, nullptr);
            if (b->mem) vkFreeMemory(device, b->mem, nullptr);
            *b = {};
        }
        if (descriptorPool) {
            vkDestroyDescriptorPool(device, descriptorPool, nullptr);
            descriptorPool = VK_NULL_HANDLE;
        }
        easuSet = rcasSet = VK_NULL_HANDLE;
        deviceImageBytes = 0;
        hostBufferBytes = 0;
    }

    MagpieStatusV1 buildSized(uint32_t iw, uint32_t ih, uint32_t ow, uint32_t oh) {
        const auto status = makeImage(iw, ih,
                                      VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                      &source);
        if (status != MAGPIE_OK) return status;
        const auto midStatus = makeImage(ow, oh,
                                         VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                         &middle);
        if (midStatus != MAGPIE_OK) return midStatus;
        const auto dstStatus = makeImage(ow, oh,
                                         VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                         &target);
        if (dstStatus != MAGPIE_OK) return dstStatus;
        const auto upStatus = makeHostBuffer(static_cast<uint64_t>(iw) * ih * 4u,
                                             VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &upload);
        if (upStatus != MAGPIE_OK) return upStatus;
        const auto rbStatus = makeHostBuffer(static_cast<uint64_t>(ow) * oh * 4u,
                                             VK_BUFFER_USAGE_TRANSFER_DST_BIT, &readback);
        if (rbStatus != MAGPIE_OK) return rbStatus;

        VkDescriptorPoolSize sizes[] = {
            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2},
            {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2},
        };
        VkDescriptorPoolCreateInfo dpi{};
        dpi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        dpi.maxSets = 2;
        dpi.poolSizeCount = 2;
        dpi.pPoolSizes = sizes;
        if (vkCreateDescriptorPool(device, &dpi, nullptr, &descriptorPool) != VK_SUCCESS)
            return MAGPIE_INTERNAL_ERROR;
        VkDescriptorSetLayout layouts[] = {setLayout, setLayout};
        VkDescriptorSetAllocateInfo dai{};
        dai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dai.descriptorPool = descriptorPool;
        dai.descriptorSetCount = 2;
        dai.pSetLayouts = layouts;
        VkDescriptorSet sets[2] = {};
        if (vkAllocateDescriptorSets(device, &dai, sets) != VK_SUCCESS) return MAGPIE_INTERNAL_ERROR;
        easuSet = sets[0];
        rcasSet = sets[1];

        VkDescriptorImageInfo reads[2] = {
            {sampler, source.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            {sampler, middle.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        };
        VkDescriptorImageInfo writes[2] = {
            {VK_NULL_HANDLE, middle.view, VK_IMAGE_LAYOUT_GENERAL},
            {VK_NULL_HANDLE, target.view, VK_IMAGE_LAYOUT_GENERAL},
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
        vkUpdateDescriptorSets(device, 4, wr, 0, nullptr);
        return MAGPIE_OK;
    }

    void barrier(VkCommandBuffer cb, VkImage image, VkImageLayout from, VkImageLayout to,
                 VkAccessFlags srcAccess, VkAccessFlags dstAccess,
                 VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage) {
        VkImageMemoryBarrier b{};
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.srcAccessMask = srcAccess;
        b.dstAccessMask = dstAccess;
        b.oldLayout = from;
        b.newLayout = to;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = image;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cb, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &b);
    }

    static uint32_t floatBits(float value) {
        uint32_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        return bits;
    }

    static void fillEasuConstants(uint32_t* out, uint32_t inW, uint32_t inH,
                                  uint32_t outW, uint32_t outH) {
        // FsrEasuCon offsets and scales, exactly as the M2 harness packs them.
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

  public:
    MagpieStatusV1 initialize(const MagpieVulkanFsrEngineConfigV1& cfg) {
        if (cfg.easu_spirv_words == nullptr || cfg.rcas_spirv_words == nullptr ||
            cfg.easu_spirv_word_count == 0 || cfg.rcas_spirv_word_count == 0 ||
            cfg.input_width == 0 || cfg.input_height == 0 ||
            cfg.output_width == 0 || cfg.output_height == 0) {
            return MAGPIE_INVALID_ARGUMENT;
        }
        const auto core = initCore(cfg);
        if (core != MAGPIE_OK) return core;
        const auto pipelines = initPipelines(cfg);
        if (pipelines != MAGPIE_OK) return pipelines;
        const auto sized = buildSized(cfg.input_width, cfg.input_height,
                                      cfg.output_width, cfg.output_height);
        if (sized != MAGPIE_OK) return sized;
        inWidth = cfg.input_width;
        inHeight = cfg.input_height;
        outWidth = cfg.output_width;
        outHeight = cfg.output_height;
        return MAGPIE_OK;
    }

    void describe(MagpieBackendProbeV1* out) const {
        const uint32_t supplied = out->struct_size;
        std::memset(out, 0, sizeof(*out));
        out->struct_size = supplied;
        out->runtime_available = deviceLost ? 0u : 1u;
        out->compute_available = deviceLost ? 0u : 1u;
        out->fsr_dispatch_validated = successfulDispatches > 0 && !deviceLost ? 1u : 0u;
        std::strncpy(out->device_name, properties.deviceName, sizeof(out->device_name) - 1);
        char reason[160];
        std::snprintf(reason, sizeof(reason),
                      "persistent engine %ux%u->%ux%u; dispatches=%u%s%s",
                      inWidth, inHeight, outWidth, outHeight, successfulDispatches,
                      deviceLost ? "; DEVICE LOST" : "",
                      lastFailure.empty() ? "" : ("; last failure: " + lastFailure).c_str());
        std::strncpy(out->reason, reason, sizeof(out->reason) - 1);
    }

    MagpieStatusV1 resize(uint32_t iw, uint32_t ih, uint32_t ow, uint32_t oh) {
        if (iw == 0 || ih == 0 || ow == 0 || oh == 0) return MAGPIE_INVALID_ARGUMENT;
        if (deviceLost) return MAGPIE_UNAVAILABLE;
        if (iw == inWidth && ih == inHeight && ow == outWidth && oh == outHeight) return MAGPIE_OK;
        if (vkDeviceWaitIdle(device) != VK_SUCCESS) {
            deviceLost = true;
            lastFailure = "device lost during resize";
            return MAGPIE_UNAVAILABLE;
        }
        destroySized();
        const auto status = buildSized(iw, ih, ow, oh);
        if (status != MAGPIE_OK) {
            lastFailure = "resource recreation after resize failed";
            return status;
        }
        inWidth = iw;
        inHeight = ih;
        outWidth = ow;
        outHeight = oh;
        return MAGPIE_OK;
    }

    MagpieStatusV1 process(const MagpieRgba8ViewV1* input,
                           const MagpieRgba8OutputV1* output,
                           float sharpnessStops, MagpieFsrTimingV1* timing) {
        if (!input || !output || input->pixels == nullptr || output->pixels == nullptr)
            return MAGPIE_INVALID_ARGUMENT;
        if (input->width != inWidth || input->height != inHeight ||
            output->width != outWidth || output->height != outHeight)
            return MAGPIE_INVALID_ARGUMENT;
        if (!std::isfinite(sharpnessStops) || sharpnessStops < 0.0f ||
            sharpnessStops > static_cast<float>(kMaxSharpnessStops))
            return MAGPIE_INVALID_ARGUMENT;
        if (deviceLost) return MAGPIE_UNAVAILABLE;

        const uint64_t inBytes = static_cast<uint64_t>(inWidth) * inHeight * 4u;
        const uint64_t outBytes = static_cast<uint64_t>(outWidth) * outHeight * 4u;

        // Upload: caller pixels → host staging → device image. Stride-respecting, RGBA8.
        void* mapped = nullptr;
        if (vkMapMemory(device, upload.mem, 0, static_cast<VkDeviceSize>(inBytes), 0, &mapped)
            != VK_SUCCESS) {
            return MAGPIE_INTERNAL_ERROR;
        }
        auto* stage = static_cast<uint8_t*>(mapped);
        for (uint32_t y = 0; y < inHeight; ++y) {
            const uint8_t* row = input->pixels + static_cast<size_t>(y) * input->row_stride_bytes;
            std::memcpy(stage + static_cast<size_t>(y) * inWidth * 4u, row,
                        static_cast<size_t>(inWidth) * 4u);
        }
        vkUnmapMemory(device, upload.mem);

        if (vkResetFences(device, 1, &fence) != VK_SUCCESS) return MAGPIE_INTERNAL_ERROR;
        // The pool was created with RESET_COMMAND_BUFFER_BIT, so a completed buffer resets
        // implicitly at begin — but a *previously failed* begin can leave state that a fresh
        // begin would reject. An explicit reset makes the failure path survivable.
        vkResetCommandBuffer(command, VK_COMMAND_BUFFER_RESET_RELEASE_RESOURCES_BIT);
        VkCommandBufferBeginInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (vkBeginCommandBuffer(command, &bi) != VK_SUCCESS) return MAGPIE_INTERNAL_ERROR;
        vkCmdResetQueryPool(command, queries, 0, 4);

        barrier(command, source.image, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {inWidth, inHeight, 1};
        vkCmdCopyBufferToImage(command, upload.buffer, source.image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        barrier(command, source.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        barrier(command, middle.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                0, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        barrier(command, target.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                0, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

        vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, queries, 0);
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, easuPipeline);
        vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout, 0, 1,
                                &easuSet, 0, nullptr);
        struct EasuPush {
            uint32_t con[16];
            uint32_t size[2];
        } easu{};
        fillEasuConstants(easu.con, inWidth, inHeight, outWidth, outHeight);
        easu.size[0] = outWidth;
        easu.size[1] = outHeight;
        vkCmdPushConstants(command, pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                           sizeof(easu), &easu);
        vkCmdDispatch(command, (outWidth + kWorkgroupSize - 1) / kWorkgroupSize,
                      (outHeight + kWorkgroupSize - 1) / kWorkgroupSize, 1);
        vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, queries, 1);

        barrier(command, middle.image, VK_IMAGE_LAYOUT_GENERAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_SHADER_WRITE_BIT,
                VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, rcasPipeline);
        vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout, 0, 1,
                                &rcasSet, 0, nullptr);
        struct RcasPush {
            uint32_t con[4];
            uint32_t size[2];
        } rcas{};
        rcas.con[0] = floatBits(std::exp2(-sharpnessStops));
        rcas.con[1] = floatBits(1.0f);
        rcas.con[2] = 0;
        rcas.con[3] = 0;
        rcas.size[0] = outWidth;
        rcas.size[1] = outHeight;
        vkCmdPushConstants(command, pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                           sizeof(rcas), &rcas);
        vkCmdDispatch(command, (outWidth + kWorkgroupSize - 1) / kWorkgroupSize,
                      (outHeight + kWorkgroupSize - 1) / kWorkgroupSize, 1);
        vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, queries, 2);

        barrier(command, target.image, VK_IMAGE_LAYOUT_GENERAL,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_SHADER_WRITE_BIT,
                VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy outCopy{};
        outCopy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        outCopy.imageExtent = {outWidth, outHeight, 1};
        vkCmdCopyImageToBuffer(command, target.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               readback.buffer, 1, &outCopy);
        vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queries, 3);
        if (vkEndCommandBuffer(command) != VK_SUCCESS) return MAGPIE_INTERNAL_ERROR;

        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &command;
        const VkResult submitted = vkQueueSubmit(queue, 1, &submit, fence);
        if (submitted != VK_SUCCESS) {
            if (submitted == VK_ERROR_DEVICE_LOST) {
                deviceLost = true;
                lastFailure = "device lost on submit";
            }
            return MAGPIE_INTERNAL_ERROR;
        }
        const VkResult waited = vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX);
        if (waited != VK_SUCCESS) {
            if (waited == VK_ERROR_DEVICE_LOST) {
                deviceLost = true;
                lastFailure = "device lost waiting for the dispatch";
            }
            return MAGPIE_INTERNAL_ERROR;
        }

        uint64_t ts[4] = {};
        if (vkGetQueryPoolResults(device, queries, 0, 4, sizeof(ts), ts, sizeof(uint64_t),
                                  VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT)
            != VK_SUCCESS) {
            return MAGPIE_INTERNAL_ERROR;
        }

        if (vkMapMemory(device, readback.mem, 0, static_cast<VkDeviceSize>(outBytes), 0, &mapped)
            != VK_SUCCESS) {
            return MAGPIE_INTERNAL_ERROR;
        }
        const auto* produced = static_cast<const uint8_t*>(mapped);
        for (uint32_t y = 0; y < outHeight; ++y) {
            uint8_t* row = output->pixels + static_cast<size_t>(y) * output->row_stride_bytes;
            std::memcpy(row, produced + static_cast<size_t>(y) * outWidth * 4u,
                        static_cast<size_t>(outWidth) * 4u);
        }
        vkUnmapMemory(device, readback.mem);

        if (timing) {
            timing->easu_ms = static_cast<double>(ts[1] - ts[0]) * timestampPeriodNanos / 1e6;
            timing->rcas_ms = static_cast<double>(ts[2] - ts[1]) * timestampPeriodNanos / 1e6;
            timing->dispatch_total_ms =
                static_cast<double>(ts[3] - ts[0]) * timestampPeriodNanos / 1e6;
        }
        ++successfulDispatches;
        lastFailure.clear();
        return MAGPIE_OK;
    }

  private:
    void teardownAll() {
        if (device != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(device);
            destroySized();
            if (easuPipeline) vkDestroyPipeline(device, easuPipeline, nullptr);
            if (rcasPipeline) vkDestroyPipeline(device, rcasPipeline, nullptr);
            if (easuModule) vkDestroyShaderModule(device, easuModule, nullptr);
            if (rcasModule) vkDestroyShaderModule(device, rcasModule, nullptr);
            if (pipelineLayout) vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
            if (setLayout) vkDestroyDescriptorSetLayout(device, setLayout, nullptr);
            if (queries) vkDestroyQueryPool(device, queries, nullptr);
            if (sampler) vkDestroySampler(device, sampler, nullptr);
            if (fence) vkDestroyFence(device, fence, nullptr);
            if (pool) vkDestroyCommandPool(device, pool, nullptr);
            vkDestroyDevice(device, nullptr);
        }
        if (instance != VK_NULL_HANDLE) vkDestroyInstance(instance, nullptr);
        instance = VK_NULL_HANDLE;
        device = VK_NULL_HANDLE;
    }
};

extern "C" MAGPIE_API MagpieStatusV1 magpie_vulkan_fsr_engine_create_v1(
    MagpieVulkanFsrEngineV1** out, const MagpieVulkanFsrEngineConfigV1* config) {
    if (!out || !config || config->struct_size < sizeof(*config)) return MAGPIE_INVALID_ARGUMENT;
    *out = nullptr;
    auto* engine = new (std::nothrow) MagpieVulkanFsrEngineV1();
    if (!engine) return MAGPIE_INTERNAL_ERROR;
    const auto status = engine->initialize(*config);
    if (status != MAGPIE_OK) {
        delete engine;
        return status;
    }
    *out = engine;
    return MAGPIE_OK;
}

extern "C" MAGPIE_API MagpieStatusV1 magpie_vulkan_fsr_engine_describe_v1(
    MagpieVulkanFsrEngineV1* engine, MagpieBackendProbeV1* out) {
    if (!engine || !out || out->struct_size < sizeof(*out)) return MAGPIE_INVALID_ARGUMENT;
    engine->describe(out);
    return MAGPIE_OK;
}

extern "C" MAGPIE_API MagpieStatusV1 magpie_vulkan_fsr_engine_process_rgba8_v1(
    MagpieVulkanFsrEngineV1* engine, const MagpieRgba8ViewV1* input,
    const MagpieRgba8OutputV1* output, float sharpness_stops, MagpieFsrTimingV1* timing_out) {
    if (!engine) return MAGPIE_INVALID_ARGUMENT;
    return engine->process(input, output, sharpness_stops, timing_out);
}

extern "C" MAGPIE_API MagpieStatusV1 magpie_vulkan_fsr_engine_resize_v1(
    MagpieVulkanFsrEngineV1* engine, uint32_t input_width, uint32_t input_height,
    uint32_t output_width, uint32_t output_height) {
    if (!engine) return MAGPIE_INVALID_ARGUMENT;
    return engine->resize(input_width, input_height, output_width, output_height);
}

extern "C" MAGPIE_API void magpie_vulkan_fsr_engine_destroy_v1(MagpieVulkanFsrEngineV1* engine) {
    delete engine;
}
