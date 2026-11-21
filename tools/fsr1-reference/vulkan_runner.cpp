// Device-side proof harness. EASU/RCAS math is the pinned AMD GLSL header.
#include "magpie/fsr1/easu.h"
#include "magpie/fsr1/rcas.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/resource.h>
#include <vector>
#include <vulkan/vulkan.h>
using namespace magpie::fsr1;
#define VKOK(x)                                                                \
  do {                                                                         \
    VkResult r = (x);                                                          \
    if (r != VK_SUCCESS)                                                       \
      throw std::runtime_error(#x " VkResult=" + std::to_string(r));           \
  } while (0)
struct Ppm {
  int w, h;
  std::vector<uint8_t> rgb;
};
static std::string tok(std::istream &s) {
  std::string a;
  while (s >> a) {
    if (a[0] == '#') {
      std::string q;
      std::getline(s, q);
      continue;
    }
    return a;
  }
  throw std::runtime_error("bad PPM");
}
static Ppm readppm(const std::string &p) {
  std::ifstream f(p, std::ios::binary);
  if (tok(f) != "P6")
    throw std::runtime_error("P6 required");
  Ppm a{std::stoi(tok(f)), std::stoi(tok(f)), {}};
  if (tok(f) != "255")
    throw std::runtime_error("8-bit PPM required");
  f.get();
  a.rgb.resize((size_t)a.w * a.h * 3);
  f.read((char *)a.rgb.data(), a.rgb.size());
  if ((size_t)f.gcount() != a.rgb.size())
    throw std::runtime_error("short PPM");
  return a;
}
static void writeppm(const std::string &p, int w, int h, const uint8_t *rgba) {
  std::ofstream f(p, std::ios::binary);
  f << "P6\n" << w << " " << h << "\n255\n";
  for (size_t i = 0; i < (size_t)w * h; i++)
    f.write((const char *)rgba + i * 4, 3);
}
static std::vector<uint32_t> spv(const char *p) {
  std::ifstream f(p, std::ios::binary | std::ios::ate);
  if (!f)
    throw std::runtime_error(std::string("missing shader: ") + p);
  auto n = f.tellg();
  f.seekg(0);
  std::vector<uint32_t> b((size_t)n / 4);
  f.read((char *)b.data(), n);
  return b;
}
struct Img {
  VkImage image{};
  VkDeviceMemory mem{};
  VkImageView view{};
  int w{}, h{};
};
struct VkRun {
  VkInstance instance{};
  VkPhysicalDevice physical{};
  VkDevice device{};
  VkQueue queue{};
  uint32_t family{};
  VkPhysicalDeviceProperties props{};
  VkPhysicalDeviceMemoryProperties mp{};
  VkCommandPool pool{};
  VkSampler sampler{};
  VkQueryPool queries{};
  double period{};
  uint64_t deviceImageAllocationBytes{};
  uint64_t hostAllocationBytes{};
  uint32_t memtype(uint32_t bits, VkMemoryPropertyFlags f) {
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
      if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & f) == f)
        return i;
    throw std::runtime_error("no matching Vulkan memory type");
  }
  VkRun() {
    VkApplicationInfo ai{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    ai.pApplicationName = "MagpieFSR1M2";
    ai.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ii{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ii.pApplicationInfo = &ai;
    VKOK(vkCreateInstance(&ii, nullptr, &instance));
    uint32_t n = 0;
    VKOK(vkEnumeratePhysicalDevices(instance, &n, nullptr));
    if (!n)
      throw std::runtime_error("no Vulkan physical device");
    std::vector<VkPhysicalDevice> ds(n);
    VKOK(vkEnumeratePhysicalDevices(instance, &n, ds.data()));
    for (auto d : ds) {
      uint32_t qn = 0;
      vkGetPhysicalDeviceQueueFamilyProperties(d, &qn, nullptr);
      std::vector<VkQueueFamilyProperties> qs(qn);
      vkGetPhysicalDeviceQueueFamilyProperties(d, &qn, qs.data());
      for (uint32_t i = 0; i < qn; i++)
        if (qs[i].queueCount && (qs[i].queueFlags & VK_QUEUE_COMPUTE_BIT)) {
          physical = d;
          family = i;
          break;
        }
      if (physical)
        break;
    }
    if (!physical)
      throw std::runtime_error("no compute queue");
    vkGetPhysicalDeviceProperties(physical, &props);
    vkGetPhysicalDeviceMemoryProperties(physical, &mp);
    VkFormatProperties rgba8{};
    vkGetPhysicalDeviceFormatProperties(physical, VK_FORMAT_R8G8B8A8_UNORM,
                                        &rgba8);
    const VkFormatFeatureFlags required = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                                          VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT;
    if ((rgba8.optimalTilingFeatures & required) != required)
      throw std::runtime_error(
          "RGBA8 optimal images lack sampled/storage support");
    if (props.limits.maxComputeWorkGroupInvocations < 64 ||
        props.limits.maxComputeWorkGroupSize[0] < 8 ||
        props.limits.maxComputeWorkGroupSize[1] < 8)
      throw std::runtime_error(
          "device limits do not support the 8x8 FSR workgroup");
    period = props.limits.timestampPeriod;
    float pr = 1;
    VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qi.queueFamilyIndex = family;
    qi.queueCount = 1;
    qi.pQueuePriorities = &pr;
    VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    di.queueCreateInfoCount = 1;
    di.pQueueCreateInfos = &qi;
    VKOK(vkCreateDevice(physical, &di, nullptr, &device));
    vkGetDeviceQueue(device, family, 0, &queue);
    VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pi.queueFamilyIndex = family;
    pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    VKOK(vkCreateCommandPool(device, &pi, nullptr, &pool));
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = si.minFilter = VK_FILTER_NEAREST;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW =
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.maxLod = 0;
    VKOK(vkCreateSampler(device, &si, nullptr, &sampler));
    VkQueryPoolCreateInfo qp{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    qp.queryType = VK_QUERY_TYPE_TIMESTAMP;
    qp.queryCount = 4;
    VKOK(vkCreateQueryPool(device, &qp, nullptr, &queries));
  }
  Img image(int w, int h, VkImageUsageFlags usage) {
    Img a{};
    a.w = w;
    a.h = h;
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = VK_FORMAT_R8G8B8A8_UNORM;
    ci.extent = {(uint32_t)w, (uint32_t)h, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VKOK(vkCreateImage(device, &ci, nullptr, &a.image));
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(device, a.image, &mr);
    VkMemoryAllocateInfo ma{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ma.allocationSize = mr.size;
    ma.memoryTypeIndex =
        memtype(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    deviceImageAllocationBytes += mr.size;
    VKOK(vkAllocateMemory(device, &ma, nullptr, &a.mem));
    VKOK(vkBindImageMemory(device, a.image, a.mem, 0));
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = a.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = ci.format;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VKOK(vkCreateImageView(device, &vi, nullptr, &a.view));
    return a;
  }
  void barrier(VkCommandBuffer c, Img a, VkImageLayout old, VkImageLayout now,
               VkAccessFlags src, VkAccessFlags dst, VkPipelineStageFlags ss,
               VkPipelineStageFlags ds) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = src;
    b.dstAccessMask = dst;
    b.oldLayout = old;
    b.newLayout = now;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = a.image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(c, ss, ds, 0, 0, nullptr, 0, nullptr, 1, &b);
  }
  VkShaderModule shader(const char *p) {
    auto b = spv(p);
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = b.size() * 4;
    ci.pCode = b.data();
    VkShaderModule m;
    VKOK(vkCreateShaderModule(device, &ci, nullptr, &m));
    return m;
  }
  ~VkRun() {
    if (device)
      vkDeviceWaitIdle(device);
    if (descriptorPool)
      vkDestroyDescriptorPool(device, descriptorPool, nullptr);
    if (queries)
      vkDestroyQueryPool(device, queries, nullptr);
    if (sampler)
      vkDestroySampler(device, sampler, nullptr);
    if (pool)
      vkDestroyCommandPool(device, pool, nullptr);
    if (device)
      vkDestroyDevice(device, nullptr);
    if (instance)
      vkDestroyInstance(instance, nullptr);
  }
  void destroyImage(Img &a) {
    if (a.view)
      vkDestroyImageView(device, a.view, nullptr);
    if (a.image)
      vkDestroyImage(device, a.image, nullptr);
    if (a.mem)
      vkFreeMemory(device, a.mem, nullptr);
    a = {};
  }
  void run(const Ppm &in, const std::string &out, int ow, int oh,
           const char *easuSpv, const char *rcasSpv) {
    auto start = std::chrono::steady_clock::now();
    Img src =
            image(in.w, in.h,
                  VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT),
        mid = image(ow, oh,
                    VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT),
        dst =
            image(ow, oh,
                  VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    uint64_t bytes = (uint64_t)in.w * in.h * 4,
             outbytes = (uint64_t)ow * oh * 4;
    VkBuffer sb, rb;
    VkDeviceMemory sm, rm;
    auto mkbuf = [&](uint64_t size, VkBufferUsageFlags usage, VkBuffer &b,
                     VkDeviceMemory &m) {
      VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
      bi.size = size;
      bi.usage = usage;
      bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
      VKOK(vkCreateBuffer(device, &bi, nullptr, &b));
      VkMemoryRequirements rq;
      vkGetBufferMemoryRequirements(device, b, &rq);
      VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
      ai.allocationSize = rq.size;
      ai.memoryTypeIndex =
          memtype(rq.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
      hostAllocationBytes += rq.size;
      VKOK(vkAllocateMemory(device, &ai, nullptr, &m));
      VKOK(vkBindBufferMemory(device, b, m, 0));
    };
    mkbuf(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, sb, sm);
    mkbuf(outbytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, rb, rm);
    void *map;
    VKOK(vkMapMemory(device, sm, 0, bytes, 0, &map));
    auto *rgba = (uint8_t *)map;
    for (size_t i = 0; i < (size_t)in.w * in.h; i++) {
      rgba[i * 4] = in.rgb[i * 3];
      rgba[i * 4 + 1] = in.rgb[i * 3 + 1];
      rgba[i * 4 + 2] = in.rgb[i * 3 + 2];
      rgba[i * 4 + 3] = 255;
    }
    vkUnmapMemory(device, sm);
    VkDescriptorSetLayoutBinding binds[2]{};
    binds[0] = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    binds[1] = {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1,
                VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo sli{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    sli.bindingCount = 2;
    sli.pBindings = binds;
    VkDescriptorSetLayout sl;
    VKOK(vkCreateDescriptorSetLayout(device, &sli, nullptr, &sl));
    VkDescriptorPoolSize pss[] = {
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2}};
    VkDescriptorPoolCreateInfo dpi{
        VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpi.maxSets = 2;
    dpi.poolSizeCount = 2;
    dpi.pPoolSizes = pss;
    VKOK(vkCreateDescriptorPool(device, &dpi, nullptr, &descriptorPool));
    VkDescriptorSetLayout layouts[] = {sl, sl};
    VkDescriptorSetAllocateInfo dai{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dai.descriptorPool = descriptorPool;
    dai.descriptorSetCount = 2;
    dai.pSetLayouts = layouts;
    VkDescriptorSet sets[2];
    VKOK(vkAllocateDescriptorSets(device, &dai, sets));
    VkDescriptorImageInfo is[2] = {
        {sampler, src.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {sampler, mid.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
    VkDescriptorImageInfo os[2] = {
        {VK_NULL_HANDLE, mid.view, VK_IMAGE_LAYOUT_GENERAL},
        {VK_NULL_HANDLE, dst.view, VK_IMAGE_LAYOUT_GENERAL}};
    VkWriteDescriptorSet wr[4]{};
    for (int k = 0; k < 2; k++) {
      wr[k * 2] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
      wr[k * 2].dstSet = sets[k];
      wr[k * 2].dstBinding = 0;
      wr[k * 2].descriptorCount = 1;
      wr[k * 2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
      wr[k * 2].pImageInfo = &is[k];
      wr[k * 2 + 1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
      wr[k * 2 + 1].dstSet = sets[k];
      wr[k * 2 + 1].dstBinding = 1;
      wr[k * 2 + 1].descriptorCount = 1;
      wr[k * 2 + 1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
      wr[k * 2 + 1].pImageInfo = &os[k];
    }
    vkUpdateDescriptorSets(device, 4, wr, 0, nullptr);
    VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT, 0, 72};
    VkPipelineLayoutCreateInfo lci{
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    lci.setLayoutCount = 1;
    lci.pSetLayouts = &sl;
    lci.pushConstantRangeCount = 1;
    lci.pPushConstantRanges = &pcr;
    VkPipelineLayout pl;
    VKOK(vkCreatePipelineLayout(device, &lci, nullptr, &pl));
    VkShaderModule em = shader(easuSpv), rmsh = shader(rcasSpv);
    auto mkpipe = [&](VkShaderModule smod) {
      VkPipelineShaderStageCreateInfo st{
          VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
      st.stage = VK_SHADER_STAGE_COMPUTE_BIT;
      st.module = smod;
      st.pName = "main";
      VkComputePipelineCreateInfo ci{
          VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
      ci.stage = st;
      ci.layout = pl;
      VkPipeline pipe;
      VKOK(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &ci, nullptr,
                                    &pipe));
      return pipe;
    };
    VkPipeline ep = mkpipe(em), rp = mkpipe(rmsh);
    VkCommandBufferAllocateInfo cai{
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    VkCommandBuffer cb;
    VKOK(vkAllocateCommandBuffers(device, &cai, &cb));
    VkCommandBufferBeginInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    cbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VKOK(vkBeginCommandBuffer(cb, &cbi));
    vkCmdResetQueryPool(cb, queries, 0, 4);
    barrier(cb, src, VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {(uint32_t)in.w, (uint32_t)in.h, 1};
    vkCmdCopyBufferToImage(cb, sb, src.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    barrier(cb, src, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    barrier(cb, mid, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0,
            VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    barrier(cb, dst, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0,
            VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, queries, 0);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, ep);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1,
                            &sets[0], 0, nullptr);
    struct EP {
      uint32_t c[16];
      uint32_t size[2];
    } pc{};
    auto bits = [](float f) {
      uint32_t b;
      std::memcpy(&b, &f, 4);
      return b;
    };
    float iw = in.w, ih = in.h, fw = ow, fh = oh;
    float c[] = {iw / fw,
                 ih / fh,
                 .5f * iw / fw - .5f,
                 .5f * ih / fh - .5f,
                 1 / iw,
                 1 / ih,
                 1 / iw,
                 -1 / ih,
                 -1 / iw,
                 2 / ih,
                 1 / iw,
                 2 / ih,
                 0,
                 4 / ih,
                 0,
                 0};
    for (int j = 0; j < 16; j++)
      pc.c[j] = bits(c[j]);
    pc.size[0] = ow;
    pc.size[1] = oh;
    vkCmdPushConstants(cb, pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
    vkCmdDispatch(cb, (ow + 7) / 8, (oh + 7) / 8, 1);
    vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, queries, 1);
    barrier(cb, mid, VK_IMAGE_LAYOUT_GENERAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, rp);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1,
                            &sets[1], 0, nullptr);
    struct RP {
      uint32_t con[4];
      uint32_t size[2];
    } rc{{bits(1.0f), bits(1.0f), 0, 0}, {(uint32_t)ow, (uint32_t)oh}};
    vkCmdPushConstants(cb, pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(rc), &rc);
    vkCmdDispatch(cb, (ow + 7) / 8, (oh + 7) / 8, 1);
    vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, queries, 2);
    barrier(cb, dst, VK_IMAGE_LAYOUT_GENERAL,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_SHADER_WRITE_BIT,
            VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy oc{};
    oc.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    oc.imageExtent = {(uint32_t)ow, (uint32_t)oh, 1};
    vkCmdCopyImageToBuffer(cb, dst.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           rb, 1, &oc);
    vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queries, 3);
    VKOK(vkEndCommandBuffer(cb));
    VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence;
    VKOK(vkCreateFence(device, &fi, nullptr, &fence));
    VkSubmitInfo si2{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si2.commandBufferCount = 1;
    si2.pCommandBuffers = &cb;
    VKOK(vkQueueSubmit(queue, 1, &si2, fence));
    VKOK(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX));
    uint64_t ts[4]{};
    VKOK(vkGetQueryPoolResults(
        device, queries, 0, 4, sizeof(ts), ts, sizeof(uint64_t),
        VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT));
    VKOK(vkMapMemory(device, rm, 0, outbytes, 0, &map));
    auto *outp = (uint8_t *)map;
    std::vector<uint8_t> pixels(outp, outp + outbytes);
    writeppm(out, ow, oh, pixels.data());
    vkUnmapMemory(device, rm);
    outp = pixels.data();
    double easuMs = (ts[1] - ts[0]) * period / 1e6,
           rcasMs = (ts[2] - ts[1]) * period / 1e6,
           totalMs = (ts[3] - ts[0]) * period / 1e6;
    std::cout << "gpu_easu_ms=" << easuMs << " gpu_rcas_ms=" << rcasMs
              << " gpu_timestamp_total_ms=" << totalMs
              << " device_image_alloc_bytes=" << deviceImageAllocationBytes
              << " output=" << out << "\n";
    Image cpuIn(in.w, in.h);
    for (size_t k = 0; k < (size_t)in.w * in.h; k++)
      cpuIn.at(k % in.w, k / in.w) = {(float)in.rgb[k * 3] / 255.f,
                                      (float)in.rgb[k * 3 + 1] / 255.f,
                                      (float)in.rgb[k * 3 + 2] / 255.f};
    auto t0 = std::chrono::steady_clock::now();
    auto cpuMid = easu(cpuIn, ow, oh);
    auto t1 = std::chrono::steady_clock::now();
    auto cpuOut = rcas(cpuMid, 0.f);
    auto t2 = std::chrono::steady_clock::now();
    double maxerr = 0, rmse = 0;
    size_t count = (size_t)ow * oh * 3;
    for (size_t k = 0; k < (size_t)ow * oh; k++)
      for (int ch = 0; ch < 3; ch++) {
        float cv = ch == 0 ? cpuOut.at(k % ow, k / ow).r
                           : (ch == 1 ? cpuOut.at(k % ow, k / ow).g
                                      : cpuOut.at(k % ow, k / ow).b);
        double e = std::abs((double)outp[k * 4 + ch] / 255.0 - cv);
        maxerr = std::max(maxerr, e);
        rmse += e * e;
      }
    rusage usage{};
    getrusage(RUSAGE_SELF, &usage);
    std::cout << "process_peak_rss_bytes=" << (uint64_t)usage.ru_maxrss * 1024
              << " cpu_easu_ms="
              << std::chrono::duration<double, std::milli>(t1 - t0).count()
              << " cpu_rcas_ms="
              << std::chrono::duration<double, std::milli>(t2 - t1).count()
              << " cpu_total_ms="
              << std::chrono::duration<double, std::milli>(t2 - t0).count()
              << " amd_shader_vs_cpu_max_abs=" << maxerr
              << " amd_shader_vs_cpu_rmse=" << std::sqrt(rmse / count)
              << " staging_alloc_bytes=" << hostAllocationBytes
              << " setup_submit_readback_ms="
              << std::chrono::duration<double, std::milli>(
                     std::chrono::steady_clock::now() - start)
                     .count()
              << "\n";
    vkDestroyFence(device, fence, nullptr);
    vkDestroyPipeline(device, ep, nullptr);
    vkDestroyPipeline(device, rp, nullptr);
    vkDestroyShaderModule(device, em, nullptr);
    vkDestroyShaderModule(device, rmsh, nullptr);
    vkDestroyPipelineLayout(device, pl, nullptr);
    vkDestroyDescriptorPool(device, descriptorPool, nullptr);
    descriptorPool = VK_NULL_HANDLE;
    vkDestroyDescriptorSetLayout(device, sl, nullptr);
    vkDestroyBuffer(device, sb, nullptr);
    vkFreeMemory(device, sm, nullptr);
    vkDestroyBuffer(device, rb, nullptr);
    vkFreeMemory(device, rm, nullptr);
    vkFreeCommandBuffers(device, pool, 1, &cb);
    destroyImage(src);
    destroyImage(mid);
    destroyImage(dst);
  }
  VkDescriptorPool descriptorPool{};
};
int main(int argc, char **argv) {
  try {
    if (argc != 5) {
      std::cerr
          << "usage: fsr1_vulkan input.ppm output.ppm easu.spv rcas.spv\n";
      return 2;
    }
    auto in = readppm(argv[1]);
    VkRun v;
    std::cout << "device=" << v.props.deviceName << " vendor_id=0x" << std::hex
              << v.props.vendorID << " device_id=0x" << v.props.deviceID
              << std::dec << " api=" << VK_VERSION_MAJOR(v.props.apiVersion)
              << "." << VK_VERSION_MINOR(v.props.apiVersion) << "."
              << VK_VERSION_PATCH(v.props.apiVersion)
              << " timestamp_period_ns=" << v.period << "\n";
    v.run(in, argv[2], in.w * 2, in.h * 2, argv[3], argv[4]);
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "ERROR: " << e.what() << "\n";
    return 1;
  }
}
