#ifndef MAGPIE_SDK_PROOF_MAGPIE_H
#define MAGPIE_SDK_PROOF_MAGPIE_H
#include <stddef.h>
#include <stdint.h>
#if defined(_WIN32)
#define MAGPIE_API __declspec(dllimport)
#else
#define MAGPIE_API __attribute__((visibility("default")))
#endif
#ifdef __cplusplus
extern "C" {
#endif
typedef enum MagpieStatusV1 { MAGPIE_OK=0, MAGPIE_INVALID_ARGUMENT=1, MAGPIE_UNAVAILABLE=2, MAGPIE_INTERNAL_ERROR=3 } MagpieStatusV1;
typedef struct MagpieRgba8ViewV1 { const uint8_t* pixels; uint32_t width,height; size_t row_stride_bytes; } MagpieRgba8ViewV1;
typedef struct MagpieRgba8OutputV1 { uint8_t* pixels; uint32_t width,height; size_t row_stride_bytes; } MagpieRgba8OutputV1;
typedef struct MagpieBackendProbeV1 { uint32_t struct_size; uint32_t runtime_available; uint32_t compute_available; uint32_t fsr_dispatch_validated; char device_name[128]; char reason[192]; } MagpieBackendProbeV1;
MAGPIE_API uint32_t magpie_core_abi_version_v1(void);
MAGPIE_API MagpieStatusV1 magpie_fsr_cpu_process_rgba8_v1(const MagpieRgba8ViewV1*, MagpieRgba8OutputV1*, float sharpness_stops);
MAGPIE_API MagpieStatusV1 magpie_vulkan_probe_v1(MagpieBackendProbeV1*);
MAGPIE_API MagpieStatusV1 magpie_gles_probe_v1(MagpieBackendProbeV1*);
typedef struct MagpieVulkanRuntimeV1 MagpieVulkanRuntimeV1;
MAGPIE_API MagpieStatusV1 magpie_vulkan_runtime_create_v1(MagpieVulkanRuntimeV1** out_runtime);
MAGPIE_API MagpieStatusV1 magpie_vulkan_runtime_describe_v1(MagpieVulkanRuntimeV1* runtime, MagpieBackendProbeV1* out);
MAGPIE_API void magpie_vulkan_runtime_destroy_v1(MagpieVulkanRuntimeV1* runtime);
#ifdef __cplusplus
}
#endif
#endif
