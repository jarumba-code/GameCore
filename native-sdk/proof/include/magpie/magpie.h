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

/*
 * M4: the persistent in-app Vulkan FSR engine. The probe above proves the device can host a
 * compute runtime; the engine below owns one and dispatches AMD FSR 1.0.2's EASU and RCAS passes
 * (the same pinned sources the M2 harness validated on Redmi A5) against caller-owned RGBA8
 * images, GPU-resident between passes, with timestamped pass timings.
 *
 * `fsr_dispatch_validated` becomes 1 only after a full dispatch — submit, fence wait, readback —
 * has succeeded on this engine. A created engine reports 0 until then, exactly as the probe does;
 * "the library loaded" is never reported as "the shader ran".
 *
 * SPIR-V is supplied by the caller (the Android build embeds the pinned-shader compilation), so
 * the engine never compiles GLSL at runtime and the license/revision question stays with the
 * vendored sources and the build that produced them.
 */
typedef struct MagpieVulkanFsrEngineConfigV1 {
    uint32_t struct_size;
    const uint32_t* easu_spirv_words;
    uint32_t easu_spirv_word_count;
    const uint32_t* rcas_spirv_words;
    uint32_t rcas_spirv_word_count;
    uint32_t input_width;
    uint32_t input_height;
    uint32_t output_width;
    uint32_t output_height;
} MagpieVulkanFsrEngineConfigV1;

typedef struct MagpieFsrTimingV1 {
    double easu_ms;
    double rcas_ms;
    double dispatch_total_ms;
} MagpieFsrTimingV1;

typedef struct MagpieVulkanFsrEngineV1 MagpieVulkanFsrEngineV1;
MAGPIE_API MagpieStatusV1 magpie_vulkan_fsr_engine_create_v1(
    MagpieVulkanFsrEngineV1** out_engine, const MagpieVulkanFsrEngineConfigV1* config);
MAGPIE_API MagpieStatusV1 magpie_vulkan_fsr_engine_describe_v1(
    MagpieVulkanFsrEngineV1* engine, MagpieBackendProbeV1* out);
/*
 * `input` describes the source image; `output` must be the configured output size. Both may carry
 * their own row stride. `sharpness_stops` follows the CPU reference: 0 is maximum sharpening.
 */
MAGPIE_API MagpieStatusV1 magpie_vulkan_fsr_engine_process_rgba8_v1(
    MagpieVulkanFsrEngineV1* engine, const MagpieRgba8ViewV1* input,
    const MagpieRgba8OutputV1* output, float sharpness_stops, MagpieFsrTimingV1* timing_out);
MAGPIE_API MagpieStatusV1 magpie_vulkan_fsr_engine_resize_v1(
    MagpieVulkanFsrEngineV1* engine, uint32_t input_width, uint32_t input_height,
    uint32_t output_width, uint32_t output_height);
MAGPIE_API void magpie_vulkan_fsr_engine_destroy_v1(MagpieVulkanFsrEngineV1* engine);
#ifdef __cplusplus
}
#endif
#endif
