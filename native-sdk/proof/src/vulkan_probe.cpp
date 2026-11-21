#include "magpie/magpie.h"
#include <algorithm>
#include <cstring>
#include <vulkan/vulkan.h>
namespace {
void init(MagpieBackendProbeV1* out) { std::memset(out,0,sizeof(*out)); out->struct_size=sizeof(*out); std::strncpy(out->reason,"probe only; FSR dispatch is not implemented in this proof library",sizeof(out->reason)-1); }
}
extern "C" MAGPIE_API MagpieStatusV1 magpie_vulkan_probe_v1(MagpieBackendProbeV1* out) {
    if(!out || out->struct_size<sizeof(*out)) return MAGPIE_INVALID_ARGUMENT;
    init(out); VkApplicationInfo app{}; app.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO; app.pApplicationName="Magpie SDK proof"; app.apiVersion=VK_API_VERSION_1_0;
    VkInstanceCreateInfo ci{}; ci.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO; ci.pApplicationInfo=&app; VkInstance instance=VK_NULL_HANDLE;
    if(vkCreateInstance(&ci,nullptr,&instance)!=VK_SUCCESS) { std::strncpy(out->reason,"vkCreateInstance failed",sizeof(out->reason)-1); return MAGPIE_UNAVAILABLE; }
    uint32_t n=0; VkResult r=vkEnumeratePhysicalDevices(instance,&n,nullptr); VkPhysicalDevice chosen=VK_NULL_HANDLE;
    if(r==VK_SUCCESS && n) { auto ds=new VkPhysicalDevice[n]; if(vkEnumeratePhysicalDevices(instance,&n,ds)==VK_SUCCESS) { for(uint32_t d=0;d<n&&!chosen;++d) { uint32_t qn=0; vkGetPhysicalDeviceQueueFamilyProperties(ds[d],&qn,nullptr); auto qs=new VkQueueFamilyProperties[qn]; vkGetPhysicalDeviceQueueFamilyProperties(ds[d],&qn,qs); for(uint32_t q=0;q<qn;++q) if(qs[q].queueCount&&(qs[q].queueFlags&VK_QUEUE_COMPUTE_BIT)){chosen=ds[d];break;} delete[] qs; } } delete[] ds; }
    if(chosen) { out->runtime_available=1; VkPhysicalDeviceProperties props{}; vkGetPhysicalDeviceProperties(chosen,&props); std::strncpy(out->device_name,props.deviceName,sizeof(out->device_name)-1); out->compute_available=1; VkFormatProperties fp{}; vkGetPhysicalDeviceFormatProperties(chosen,VK_FORMAT_R8G8B8A8_UNORM,&fp); if((fp.optimalTilingFeatures&(VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT|VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT))==(VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT|VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)) std::strncpy(out->reason,"runtime, compute queue and RGBA8 sampled/storage format found; no FSR dispatch performed",sizeof(out->reason)-1); else std::strncpy(out->reason,"compute queue found but RGBA8 sampled/storage format unavailable; no FSR dispatch",sizeof(out->reason)-1); }
    else std::strncpy(out->reason,"no Vulkan physical device with a compute queue; no FSR dispatch",sizeof(out->reason)-1);
    vkDestroyInstance(instance,nullptr); return out->runtime_available?MAGPIE_OK:MAGPIE_UNAVAILABLE;
}
