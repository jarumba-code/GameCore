// Magpie controlled test app — Vulkan renderer.
//
// This app renders a deterministic animated scene into whatever images vkGetSwapchainImagesKHR
// returns, at the swapchain's own extent. That is the point of it: when the Magpie layer is
// enabled, the layer substitutes low-resolution images (360x800-class on the reference device)
// and upscales to the native swapchain at present time; when it is disabled, the app renders at
// native resolution through the same code path. The app logs both the requested extent and the
// returned image count/extent every frame batch, which is the diagnostic that establishes
// whether the substitution is actually happening — no other signal is trusted.
//
// Modes (chosen by the launcher intent, see RenderActivity):
//   A. layer disabled  — native render, no FSR (baseline)
//   B. layer enabled + `adb shell setprop debug.magpie.fsr 0` — reduced render, linear upscale
//   C. layer enabled, FSR on (default) — reduced render, EASU+RCAS in the layer
#include <android/log.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_android.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
#include "test_shaders.h"

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "MagpieTest", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "MagpieTest", __VA_ARGS__)

static const char* MAGPIE_LAYER_NAME = "VK_LAYER_MAGPIE_FSR1";

namespace {

struct FrameSync {
    VkSemaphore imageAvailable = VK_NULL_HANDLE;
    VkFence inFlight = VK_NULL_HANDLE;
};

struct Renderer {
    std::atomic<bool> running{false};
    std::thread thread;
    ANativeWindow* window = nullptr;
    bool layerEnabled = false;
    bool substituted = false;

    VkInstance instance = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    uint32_t graphicsFamily = UINT32_MAX;
    VkQueue queue = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkExtent2D surfaceExtent{};  // native Android surface / real presentation swapchain
    VkExtent2D extent{};          // app render extent (low-res when the layer is enabled)
    VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
    uint32_t imageCount = 0;
    std::vector<VkImage> swapchainImages;
    VkRenderPass renderPass = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkCommandPool commandPool = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> commandBuffers;
    std::vector<VkFramebuffer> framebuffers;
    std::vector<VkImageView> imageViews;
    std::vector<FrameSync> sync;  // per-frame-in-flight (2)
    std::vector<VkSemaphore> renderDoneByImage;  // presentation waits, reused only after reacquire
    uint32_t frameIndex = 0;
    std::chrono::steady_clock::time_point startedAt{};
    uint64_t frames = 0;
    uint32_t recreated = 0;

    ~Renderer() { teardown(); }

    static VKAPI_ATTR VkBool32 VKAPI_CALL debugReport(...) { return VK_FALSE; }

    bool init(ANativeWindow* win, bool enableLayer) {
        window = win;
        layerEnabled = enableLayer;
        ANativeWindow_acquire(win);

        VkApplicationInfo app{};
        app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        app.pApplicationName = "MagpieTest";
        app.apiVersion = VK_API_VERSION_1_1;

        std::vector<const char*> layers;
        if (enableLayer) {
            layers.push_back(MAGPIE_LAYER_NAME);
            LOGI("instance will enable layer %s", MAGPIE_LAYER_NAME);
        } else {
            LOGI("instance created without the Magpie layer (config A)");
        }

        VkInstanceCreateInfo ici{};
        ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        ici.pApplicationInfo = &app;
        ici.enabledLayerCount = static_cast<uint32_t>(layers.size());
        ici.ppEnabledLayerNames = layers.data();
        const char* surfaceExts[] = {"VK_KHR_surface", "VK_KHR_android_surface"};
        const char* swapchainExt = "VK_KHR_swapchain";
        ici.enabledExtensionCount = 2;
        ici.ppEnabledExtensionNames = surfaceExts;
        if (vkCreateInstance(&ici, nullptr, &instance) != VK_SUCCESS) {
            LOGE("vkCreateInstance failed");
            return false;
        }

        const auto createAndroidSurface = reinterpret_cast<PFN_vkCreateAndroidSurfaceKHR>(
            vkGetInstanceProcAddr(instance, "vkCreateAndroidSurfaceKHR"));
        VkAndroidSurfaceCreateInfoKHR sci{};
        sci.sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR;
        sci.window = win;
        if (!createAndroidSurface ||
            createAndroidSurface(instance, &sci, nullptr, &surface) != VK_SUCCESS) {
            LOGE("surface creation failed");
            return false;
        }

        uint32_t physicalCount = 0;
        vkEnumeratePhysicalDevices(instance, &physicalCount, nullptr);
        if (physicalCount == 0) {
            LOGE("no physical device");
            return false;
        }
        std::vector<VkPhysicalDevice> physicals(physicalCount);
        vkEnumeratePhysicalDevices(instance, &physicalCount, physicals.data());
        physical = physicals[0];

        // The layer's EASU/RCAS storage images and copy path are RGBA8_UNORM. Do not
        // silently create an incompatible BGRA or sRGB swapchain on devices that lack it.
        uint32_t surfaceFormatCount = 0;
        if (vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &surfaceFormatCount, nullptr) != VK_SUCCESS ||
            surfaceFormatCount == 0) {
            LOGE("surface format query failed or returned no formats");
            return false;
        }
        std::vector<VkSurfaceFormatKHR> surfaceFormats(surfaceFormatCount);
        if (vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &surfaceFormatCount,
                                                 surfaceFormats.data()) != VK_SUCCESS) {
            LOGE("surface format enumeration failed");
            return false;
        }
        bool formatSupported = false;
        for (const auto& candidate : surfaceFormats) {
            if (candidate.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR &&
                (candidate.format == VK_FORMAT_R8G8B8A8_UNORM ||
                 candidate.format == VK_FORMAT_UNDEFINED)) {
                format = VK_FORMAT_R8G8B8A8_UNORM;
                formatSupported = true;
                break;
            }
        }
        if (!formatSupported) {
            LOGE("device surface does not expose RGBA8_UNORM with SRGB_NONLINEAR color space");
            return false;
        }

        uint32_t familyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &familyCount, nullptr);
        std::vector<VkQueueFamilyProperties> families(familyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &familyCount, families.data());
        const auto presentSupport = reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceSupportKHR>(
            vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceSurfaceSupportKHR"));
        for (uint32_t i = 0; i < familyCount; ++i) {
            if (!(families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) continue;
            // The layer records compute EASU/RCAS work on the app's present queue. The
            // controlled test app therefore selects a graphics+present+compute family.
            if (enableLayer && !(families[i].queueFlags & VK_QUEUE_COMPUTE_BIT)) continue;
            VkBool32 supported = VK_FALSE;
            if (presentSupport && presentSupport(physical, i, surface, &supported) == VK_SUCCESS &&
                supported) {
                graphicsFamily = i;
                break;
            }
        }
        if (graphicsFamily == UINT32_MAX) {
            LOGE("no graphics+present queue family");
            return false;
        }

        const float priority = 1.0f;
        VkDeviceQueueCreateInfo qci{};
        qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        qci.queueFamilyIndex = graphicsFamily;
        qci.queueCount = 1;
        qci.pQueuePriorities = &priority;
        std::vector<const char*> deviceExts = {swapchainExt};
        if (layerEnabled) {
            const char* magpieControlExt = "VK_MAGPIE_layer_control";
            uint32_t layerDeviceExtCount = 0;
            if (vkEnumerateDeviceExtensionProperties(
                    physical, "VK_LAYER_MAGPIE_FSR1", &layerDeviceExtCount, nullptr) != VK_SUCCESS ||
                layerDeviceExtCount == 0) {
                LOGE("Magpie control device extension is not exposed by the enabled layer");
                return false;
            }
            std::vector<VkExtensionProperties> layerDeviceExts(layerDeviceExtCount);
            if (vkEnumerateDeviceExtensionProperties(
                    physical, "VK_LAYER_MAGPIE_FSR1", &layerDeviceExtCount,
                    layerDeviceExts.data()) != VK_SUCCESS) {
                LOGE("Magpie control device extension enumeration failed");
                return false;
            }
            bool foundControlExt = false;
            for (const auto& extension : layerDeviceExts) {
                if (std::strcmp(extension.extensionName, magpieControlExt) == 0) {
                    foundControlExt = true;
                    break;
                }
            }
            if (!foundControlExt) {
                LOGE("Magpie control device extension was not advertised");
                return false;
            }
            deviceExts.push_back(magpieControlExt);
        }
        VkDeviceCreateInfo dci{};
        dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        dci.queueCreateInfoCount = 1;
        dci.pQueueCreateInfos = &qci;
        dci.enabledExtensionCount = static_cast<uint32_t>(deviceExts.size());
        dci.ppEnabledExtensionNames = deviceExts.data();
        if (vkCreateDevice(physical, &dci, nullptr, &device) != VK_SUCCESS) {
            LOGE("vkCreateDevice failed");
            return false;
        }
        vkGetDeviceQueue(device, graphicsFamily, 0, &queue);
        if (layerEnabled) {
            using PFN_vkMagpieSetFakeImageFinalLayout =
                VkResult(VKAPI_PTR*)(VkDevice, VkImageLayout);
            const auto setFakeLayout = reinterpret_cast<PFN_vkMagpieSetFakeImageFinalLayout>(
                vkGetDeviceProcAddr(device, "vkMagpieSetFakeImageFinalLayout"));
            if (!setFakeLayout ||
                setFakeLayout(device, VK_IMAGE_LAYOUT_GENERAL) != VK_SUCCESS) {
                LOGE("Magpie layer did not expose its controlled-app image-layout contract");
                return false;
            }
        }

        // The critical diagnostic pair: the surface's own extent vs the extent the swapchain
        // images report. With the layer substituted they differ (low-res images on a native
        // surface); without it they match.
        const auto capsFn = reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR>(
            vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR"));
        VkSurfaceCapabilitiesKHR caps{};
        capsFn(physical, surface, &caps);
        LOGI("surface currentExtent=%ux%u minImageExtent=%ux%u (requested this extent)",
             caps.currentExtent.width, caps.currentExtent.height,
             caps.minImageExtent.width, caps.minImageExtent.height);
        surfaceExtent = caps.currentExtent;
        extent = surfaceExtent;
        return createSwapchainObjects() && createRenderPipeline() && createFrameSync();
    }

    bool createSwapchainObjects() {
        // Rebuild only after prior graphics and presentation work has completed. This also
        // makes it safe to retire the old swapchain's image views and per-image present semaphores.
        if (swapchain != VK_NULL_HANDLE) vkDeviceWaitIdle(device);
        const auto capsFn = reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR>(
            vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR"));
        VkSurfaceCapabilitiesKHR caps{};
        capsFn(physical, surface, &caps);
        surfaceExtent = caps.currentExtent;
        extent = surfaceExtent;
        const bool wasSubstituted = substituted;
        const VkExtent2D oldRenderExtent = extent;
        VkSwapchainCreateInfoKHR sci{};
        sci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        sci.surface = surface;
        sci.minImageCount = std::max(3u, caps.minImageCount);
        if (caps.maxImageCount > 0)
            sci.minImageCount = std::min(sci.minImageCount, caps.maxImageCount);
        sci.imageFormat = format;
        sci.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
        // Android's fixed surface extent remains native. The layer substitutes smaller images;
        // this controlled renderer sizes its framebuffer, viewport and scissor to the fake image.
        sci.imageExtent = surfaceExtent;
        sci.imageArrayLayers = 1;
        if ((caps.supportedUsageFlags & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) == 0) {
            LOGE("surface does not support color-attachment swapchain images");
            return false;
        }
        sci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        sci.preTransform = caps.currentTransform;
        sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        if ((caps.supportedCompositeAlpha & sci.compositeAlpha) == 0) {
            const VkCompositeAlphaFlagBitsKHR alphaChoices[] = {
                VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
                VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
                VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR
            };
            bool alphaFound = false;
            for (const auto candidate : alphaChoices) {
                if (caps.supportedCompositeAlpha & candidate) {
                    sci.compositeAlpha = candidate;
                    alphaFound = true;
                    break;
                }
            }
            if (!alphaFound) {
                LOGE("surface exposes no supported composite-alpha mode");
                return false;
            }
        }
        sci.presentMode = VK_PRESENT_MODE_FIFO_KHR;  // vsync; the honest pacing source
        sci.clipped = VK_TRUE;
        sci.oldSwapchain = swapchain;
        VkSwapchainKHR old = swapchain;
        if (vkCreateSwapchainKHR(device, &sci, nullptr, &swapchain) != VK_SUCCESS) {
            LOGE("swapchain creation failed");
            return false;
        }
        substituted = false;
        extent = surfaceExtent;
        if (layerEnabled) {
            using PFN_vkMagpieGetSwapchainRenderExtent =
                VkResult(VKAPI_PTR*)(VkDevice, VkSwapchainKHR, VkExtent2D*);
            const auto getRenderExtent = reinterpret_cast<PFN_vkMagpieGetSwapchainRenderExtent>(
                vkGetDeviceProcAddr(device, "vkMagpieGetSwapchainRenderExtent"));
            VkExtent2D layerExtent{};
            if (getRenderExtent &&
                getRenderExtent(device, swapchain, &layerExtent) == VK_SUCCESS) {
                extent = layerExtent;
                substituted = true;
            }
        }
        if (old != VK_NULL_HANDLE && wasSubstituted != substituted) {
            LOGE("layer substitution mode changed during swapchain recreation; stopping safely");
            vkDestroySwapchainKHR(device, swapchain, nullptr);
            swapchain = old;
            substituted = wasSubstituted;
            extent = oldRenderExtent;
            running = false;
            return false;
        }
        LOGI("renderer extents: surface=%ux%u render=%ux%u substituted=%d",
             surfaceExtent.width, surfaceExtent.height, extent.width, extent.height,
             substituted ? 1 : 0);

        if (old != VK_NULL_HANDLE) {
            for (VkFramebuffer fb : framebuffers) vkDestroyFramebuffer(device, fb, nullptr);
            for (VkImageView iv : imageViews) vkDestroyImageView(device, iv, nullptr);
            framebuffers.clear();
            imageViews.clear();
            for (VkSemaphore sem : renderDoneByImage)
                vkDestroySemaphore(device, sem, nullptr);
            renderDoneByImage.clear();
            vkDestroySwapchainKHR(device, old, nullptr);
            recreated++;
        }
        imageCount = 0;
        if (vkGetSwapchainImagesKHR(device, swapchain, &imageCount, nullptr) != VK_SUCCESS ||
            imageCount == 0) {
            LOGE("swapchain image count query failed");
            return false;
        }
        swapchainImages.resize(imageCount);
        if (vkGetSwapchainImagesKHR(device, swapchain, &imageCount, swapchainImages.data()) != VK_SUCCESS) {
            LOGE("swapchain image handle query failed");
            return false;
        }
        // THE DIAGNOSTIC LINE: what images did the app actually get? With the layer substituted,
        // this extent is the reduced one (e.g. 800x360 on the 1600x720 surface).
        LOGI("swapchain images: count=%u surface=%ux%u render=%ux%u",
             imageCount, surfaceExtent.width, surfaceExtent.height, extent.width, extent.height);

        renderDoneByImage.resize(imageCount, VK_NULL_HANDLE);
        for (uint32_t i = 0; i < imageCount; ++i) {
            VkSemaphoreCreateInfo semaphoreInfo{};
            semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
            if (vkCreateSemaphore(device, &semaphoreInfo, nullptr, &renderDoneByImage[i]) != VK_SUCCESS) {
                LOGE("per-image render-finished semaphore creation failed at image %u", i);
                return false;
            }
        }
        imageViews.resize(imageCount);
        for (uint32_t i = 0; i < imageCount; ++i) {
            VkImageViewCreateInfo vi{};
            vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            vi.image = swapchainImages[i];
            vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
            vi.format = format;
            vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            if (vkCreateImageView(device, &vi, nullptr, &imageViews[i]) != VK_SUCCESS) return false;
        }
        return true;
    }

    bool createRenderPipeline() {
        VkAttachmentDescription color{};
        color.format = format;
        color.samples = VK_SAMPLE_COUNT_1_BIT;
        color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        color.finalLayout = substituted ? VK_IMAGE_LAYOUT_GENERAL
                                        : VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        VkAttachmentReference colorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &colorRef;
        VkRenderPassCreateInfo rpci{};
        rpci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        rpci.attachmentCount = 1;
        rpci.pAttachments = &color;
        rpci.subpassCount = 1;
        rpci.pSubpasses = &subpass;
        if (vkCreateRenderPass(device, &rpci, nullptr, &renderPass) != VK_SUCCESS) return false;

        const auto createModule = [&](const uint32_t* code, uint32_t words) {
            VkShaderModuleCreateInfo ci{};
            ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
            ci.codeSize = static_cast<size_t>(words) * 4u;
            ci.pCode = code;
            VkShaderModule m = VK_NULL_HANDLE;
            if (vkCreateShaderModule(device, &ci, nullptr, &m) != VK_SUCCESS) {
                LOGE("test shader module creation failed — the build must embed test_shaders.h");
            }
            return m;
        };
        // The test renderer's own shaders, embedded by its CMake (test_shaders.h).
VkShaderModule vs = createModule(magpie_test::kVertSpv, magpie_test::kVertSpv_word_count);
        VkShaderModule fs = createModule(magpie_test::kFragSpv, magpie_test::kFragSpv_word_count);
        if (vs == VK_NULL_HANDLE || fs == VK_NULL_HANDLE) return false;

        VkPushConstantRange pcr{VK_SHADER_STAGE_FRAGMENT_BIT, 0, 4};
        VkPipelineLayoutCreateInfo lci{};
        lci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        lci.pushConstantRangeCount = 1;
        lci.pPushConstantRanges = &pcr;
        if (vkCreatePipelineLayout(device, &lci, nullptr, &pipelineLayout) != VK_SUCCESS) return false;

        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vs;
        stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = fs;
        stages[1].pName = "main";
        VkPipelineVertexInputStateCreateInfo vi{};
        vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        VkPipelineInputAssemblyStateCreateInfo ia{};
        ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo vp{};
        vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        vp.viewportCount = 1;
        vp.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rs{};
        rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo ms{};
        ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState blend{};
        blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                               VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo cb{};
        cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        cb.attachmentCount = 1;
        cb.pAttachments = &blend;
        VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo ds{};
        ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        ds.dynamicStateCount = 2;
        ds.pDynamicStates = dyn;
        VkGraphicsPipelineCreateInfo gpi{};
        gpi.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        gpi.stageCount = 2;
        gpi.pStages = stages;
        gpi.pVertexInputState = &vi;
        gpi.pInputAssemblyState = &ia;
        gpi.pViewportState = &vp;
        gpi.pRasterizationState = &rs;
        gpi.pMultisampleState = &ms;
        gpi.pColorBlendState = &cb;
        gpi.pDynamicState = &ds;
        gpi.layout = pipelineLayout;
        gpi.renderPass = renderPass;
        if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gpi, nullptr, &pipeline) != VK_SUCCESS) {
            LOGE("graphics pipeline creation failed");
            return false;
        }
        vkDestroyShaderModule(device, vs, nullptr);
        vkDestroyShaderModule(device, fs, nullptr);

        VkCommandPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = graphicsFamily;
        if (vkCreateCommandPool(device, &poolInfo, nullptr, &commandPool) != VK_SUCCESS) {
            LOGE("renderer command pool creation failed");
            return false;
        }
        commandBuffers.resize(imageCount);
        VkCommandBufferAllocateInfo cai{};
        cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cai.commandPool = commandPool;
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = imageCount;
        if (vkAllocateCommandBuffers(device, &cai, commandBuffers.data()) != VK_SUCCESS) {
            LOGE("renderer command buffer allocation failed");
            return false;
        }
        framebuffers.resize(imageCount);
        for (uint32_t i = 0; i < imageCount; ++i) {
            VkFramebufferCreateInfo fci{};
            fci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            fci.renderPass = renderPass;
            fci.attachmentCount = 1;
            fci.pAttachments = &imageViews[i];
            fci.width = extent.width;
            fci.height = extent.height;
            fci.layers = 1;
            if (vkCreateFramebuffer(device, &fci, nullptr, &framebuffers[i]) != VK_SUCCESS) return false;
        }
        sync.resize(2);
        for (auto& s : sync) {
            VkSemaphoreCreateInfo sci{};
            sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
            if (vkCreateSemaphore(device, &sci, nullptr, &s.imageAvailable) != VK_SUCCESS)
                return false;
            VkFenceCreateInfo fci{};
            fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
            fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
            if (vkCreateFence(device, &fci, nullptr, &s.inFlight) != VK_SUCCESS)
                return false;
        }
        return true;
    }

    void renderFrame(double seconds) {
        uint32_t imageIndex = 0;
        FrameSync& s = sync[frameIndex % 2];
        const VkResult frameWait = vkWaitForFences(device, 1, &s.inFlight, VK_TRUE, UINT64_MAX);
        if (frameWait != VK_SUCCESS) {
            LOGE("frame fence wait failed: %d", (int)frameWait);
            running = false;
            return;
        }
        VkResult acquire = vkAcquireNextImageKHR(device, swapchain, UINT64_MAX,
                                                 s.imageAvailable, VK_NULL_HANDLE, &imageIndex);
        if (acquire == VK_ERROR_OUT_OF_DATE_KHR) {
            if (!createSwapchainObjects() || !rebuildFramebuffersOnly()) running = false;
            return;
        }
        if (acquire != VK_SUCCESS && acquire != VK_SUBOPTIMAL_KHR) {
            LOGE("swapchain image acquisition failed: %d", (int)acquire);
            running = false;
            return;
        }
        const bool acquireSuboptimal = acquire == VK_SUBOPTIMAL_KHR;
        if (imageIndex >= commandBuffers.size() || imageIndex >= framebuffers.size() ||
            imageIndex >= renderDoneByImage.size()) {
            LOGE("acquired image index %u exceeds renderer resources", imageIndex);
            running = false;
            return;
        }
        VkCommandBuffer cb = commandBuffers[imageIndex];
        if (vkResetCommandBuffer(cb, VK_COMMAND_BUFFER_RESET_RELEASE_RESOURCES_BIT) != VK_SUCCESS) {
            LOGE("command buffer reset failed");
            running = false;
            return;
        }
        VkCommandBufferBeginInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        if (vkBeginCommandBuffer(cb, &bi) != VK_SUCCESS) {
            LOGE("command buffer begin failed");
            running = false;
            return;
        }
        VkClearValue clear{{{0.05f, 0.05f, 0.08f, 1.0f}}};
        VkRenderPassBeginInfo rp{};
        rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rp.renderPass = renderPass;
        rp.framebuffer = framebuffers[imageIndex];
        rp.renderArea = {{0, 0}, {extent.width, extent.height}};
        rp.clearValueCount = 1;
        rp.pClearValues = &clear;
        vkCmdBeginRenderPass(cb, &rp, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        VkViewport viewport{0, 0, static_cast<float>(extent.width),
                            static_cast<float>(extent.height), 0, 1};
        VkRect2D scissor{{0, 0}, {extent.width, extent.height}};
        vkCmdSetViewport(cb, 0, 1, &viewport);
        vkCmdSetScissor(cb, 0, 1, &scissor);
        float time = static_cast<float>(seconds);
        vkCmdPushConstants(cb, pipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 4, &time);
        vkCmdDraw(cb, 3, 1, 0, 0);
        vkCmdEndRenderPass(cb);
        if (vkEndCommandBuffer(cb) != VK_SUCCESS) {
            LOGE("command buffer end failed");
            running = false;
            return;
        }

        VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.waitSemaphoreCount = 1;
        si.pWaitSemaphores = &s.imageAvailable;
        si.pWaitDstStageMask = &waitStage;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cb;
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &renderDoneByImage[imageIndex];
        if (vkResetFences(device, 1, &s.inFlight) != VK_SUCCESS) {
            LOGE("frame fence reset failed");
            running = false;
            return;
        }
        if (vkQueueSubmit(queue, 1, &si, s.inFlight) != VK_SUCCESS) {
            LOGE("graphics queue submission failed");
            running = false;
            return;
        }

        VkPresentInfoKHR pi{};
        pi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores = &renderDoneByImage[imageIndex];
        pi.swapchainCount = 1;
        pi.pSwapchains = &swapchain;
        pi.pImageIndices = &imageIndex;
        VkResult present = vkQueuePresentKHR(queue, &pi);
        frameIndex++;
        frames++;
        if (frames % 120 == 0) {
            const double elapsed =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - startedAt).count();
            LOGI("frames=%llu fps=%.2f swapchainExtent=%ux%u recreates=%u",
                 static_cast<unsigned long long>(frames), frames / elapsed,
                 extent.width, extent.height, recreated);
        }
        if (present == VK_ERROR_OUT_OF_DATE_KHR || present == VK_SUBOPTIMAL_KHR ||
            acquireSuboptimal) {
            if (!createSwapchainObjects() || !rebuildFramebuffersOnly()) running = false;
        } else if (present != VK_SUCCESS) {
            LOGE("queue presentation failed: %d", (int)present);
            running = false;
        }
    }

    bool rebuildFramebuffersOnly() {
        // Swapchain image count can change across recreation. The device was idled in
        // createSwapchainObjects, so old per-image command buffers are no longer in flight.
        if (!commandBuffers.empty()) {
            vkFreeCommandBuffers(device, commandPool,
                                 static_cast<uint32_t>(commandBuffers.size()),
                                 commandBuffers.data());
            commandBuffers.clear();
        }
        commandBuffers.resize(imageCount);
        VkCommandBufferAllocateInfo cai{};
        cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cai.commandPool = commandPool;
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = imageCount;
        if (vkAllocateCommandBuffers(device, &cai, commandBuffers.data()) != VK_SUCCESS) {
            LOGE("command buffer reallocation failed");
            return false;
        }
        framebuffers.resize(imageCount);
        for (uint32_t i = 0; i < imageCount; ++i) {
            VkFramebufferCreateInfo fci{};
            fci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            fci.renderPass = renderPass;
            fci.attachmentCount = 1;
            fci.pAttachments = &imageViews[i];
            fci.width = extent.width;
            fci.height = extent.height;
            fci.layers = 1;
            if (vkCreateFramebuffer(device, &fci, nullptr, &framebuffers[i]) != VK_SUCCESS) {
                LOGE("framebuffer recreation failed at image %u", i);
                return false;
            }
        }
        return true;
    }

    void teardown() {
        running = false;
        if (thread.joinable()) thread.join();
        if (device != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(device);
            for (auto& s : sync) {
                if (s.imageAvailable) vkDestroySemaphore(device, s.imageAvailable, nullptr);
                if (s.inFlight) vkDestroyFence(device, s.inFlight, nullptr);
            }
            sync.clear();
            for (VkSemaphore sem : renderDoneByImage)
                if (sem) vkDestroySemaphore(device, sem, nullptr);
            renderDoneByImage.clear();
            for (VkFramebuffer fb : framebuffers) vkDestroyFramebuffer(device, fb, nullptr);
            for (VkImageView iv : imageViews) vkDestroyImageView(device, iv, nullptr);
            framebuffers.clear();
            imageViews.clear();
            if (commandPool) vkDestroyCommandPool(device, commandPool, nullptr);
            if (pipeline) vkDestroyPipeline(device, pipeline, nullptr);
            if (pipelineLayout) vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
            if (renderPass) vkDestroyRenderPass(device, renderPass, nullptr);
            if (swapchain) vkDestroySwapchainKHR(device, swapchain, nullptr);
            vkDestroyDevice(device, nullptr);
            device = VK_NULL_HANDLE;
        }
        if (surface != VK_NULL_HANDLE) {
            vkDestroySurfaceKHR(instance, surface, nullptr);
            surface = VK_NULL_HANDLE;
        }
        if (instance != VK_NULL_HANDLE) {
            vkDestroyInstance(instance, nullptr);
            instance = VK_NULL_HANDLE;
        }
        if (window) {
            ANativeWindow_release(window);
            window = nullptr;
        }
    }

    void run() {
        startedAt = std::chrono::steady_clock::now();
        const auto start = startedAt;
        while (running) {
            const double seconds =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            renderFrame(seconds);
        }
    }
};

Renderer* g_renderer = nullptr;

}  // namespace

extern "C" JNIEXPORT void JNICALL
Java_com_gamecore_magpietest_RendererNative_onSurfaceCreated(JNIEnv* env, jclass,
                                                             jobject surface, jboolean layer) {
    if (g_renderer) return;
    ANativeWindow* win = ANativeWindow_fromSurface(env, surface);
    auto* renderer = new Renderer();
    if (renderer->init(win, layer == JNI_TRUE)) {
        g_renderer = renderer;
        renderer->running = true;
        renderer->thread = std::thread([renderer] { renderer->run(); });
    } else {
        delete renderer;
        LOGE("renderer initialization failed — no frames will be presented");
    }
}

extern "C" JNIEXPORT void JNICALL
Java_com_gamecore_magpietest_RendererNative_onSurfaceDestroyed(JNIEnv*, jclass) {
    if (g_renderer) {
        g_renderer->running = false;
        if (g_renderer->thread.joinable()) g_renderer->thread.join();
        delete g_renderer;
        g_renderer = nullptr;
    }
}
