// Android-only adapter: Bitmap locks feed the stable C SDK API; no Java pixel
// array is created. The SDK owns image math and the caller owns bitmaps.
// M4 adds the persistent Vulkan FSR engine path, whose SPIR-V is embedded in
// the engine library by the build (magpie_shaders.h); the engine never compiles
// GLSL at runtime.
#include <android/bitmap.h>
#include <jni.h>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <string>
#include "magpie/magpie.h"
#include "magpie_shaders.h"

namespace {
std::string probeText(const char* name, MagpieStatusV1 status,
                      const MagpieBackendProbeV1& p) {
    std::string out = name;
    out += p.runtime_available ? " runtime=available" : " runtime=unavailable";
    out += p.compute_available ? " compute=available" : " compute=unavailable";
    out += p.fsr_dispatch_validated ? " fsr_dispatch=validated" : " fsr_dispatch=not-validated";
    if (p.device_name[0]) out += " device=" + std::string(p.device_name);
    out += status == MAGPIE_OK ? " probe=ok: " : " probe=unavailable: ";
    out += p.reason;
    return out;
}
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_gamecore_ui_graphics_FsrNative_capabilities(JNIEnv* env, jclass) {
    MagpieBackendProbeV1 vk{}; vk.struct_size = sizeof(vk);
    MagpieBackendProbeV1 gl{}; gl.struct_size = sizeof(gl);
    const auto vs = magpie_vulkan_probe_v1(&vk);
    const auto gs = magpie_gles_probe_v1(&gl);
    const std::string result = "CPU EASU+RCAS available; " +
        probeText("Vulkan", vs, vk) + "; " + probeText("GLES", gs, gl);
    return env->NewStringUTF(result.c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_gamecore_ui_graphics_FsrNative_processCpu(JNIEnv* env, jclass,
                                                   jobject src, jobject dst,
                                                   jfloat sharpness) {
    AndroidBitmapInfo si{}, di{};
    if (AndroidBitmap_getInfo(env, src, &si) != ANDROID_BITMAP_RESULT_SUCCESS ||
        AndroidBitmap_getInfo(env, dst, &di) != ANDROID_BITMAP_RESULT_SUCCESS ||
        si.format != ANDROID_BITMAP_FORMAT_RGBA_8888 ||
        di.format != ANDROID_BITMAP_FORMAT_RGBA_8888 ||
        !si.width || !si.height || di.width < si.width || di.height < si.height ||
        static_cast<uint64_t>(di.width) * di.height >
            static_cast<uint64_t>(si.width) * si.height * 4) {
        return env->NewStringUTF("invalid bitmap format or unsupported scale");
    }
    void *sp = nullptr, *dp = nullptr;
    if (AndroidBitmap_lockPixels(env, src, &sp) != ANDROID_BITMAP_RESULT_SUCCESS)
        return env->NewStringUTF("input bitmap lock failed");
    if (AndroidBitmap_lockPixels(env, dst, &dp) != ANDROID_BITMAP_RESULT_SUCCESS) {
        AndroidBitmap_unlockPixels(env, src);
        return env->NewStringUTF("output bitmap lock failed");
    }
    const auto start = std::chrono::steady_clock::now();
    const MagpieRgba8ViewV1 input{static_cast<const uint8_t*>(sp), si.width,
                                  si.height, si.stride};
    MagpieRgba8OutputV1 output{static_cast<uint8_t*>(dp), di.width,
                               di.height, di.stride};
    const auto status = magpie_fsr_cpu_process_rgba8_v1(&input, &output, sharpness);
    const double elapsed = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start).count();
    AndroidBitmap_unlockPixels(env, dst);
    AndroidBitmap_unlockPixels(env, src);
    if (status != MAGPIE_OK)
        return env->NewStringUTF(("CPU FSR failed; SDK status=" +
                                  std::to_string(static_cast<int>(status))).c_str());
    return env->NewStringUTF(("CPU EASU+RCAS elapsed_ms=" +
                              std::to_string(elapsed) +
                              "; timings include C API and image allocations").c_str());
}

extern "C" JNIEXPORT jlong JNICALL
Java_com_gamecore_ui_graphics_FsrNative_createVulkanRuntime(JNIEnv*, jclass) {
    MagpieVulkanRuntimeV1* runtime = nullptr;
    if (magpie_vulkan_runtime_create_v1(&runtime) != MAGPIE_OK || !runtime) return 0;
    return static_cast<jlong>(reinterpret_cast<uintptr_t>(runtime));
}
extern "C" JNIEXPORT jstring JNICALL
Java_com_gamecore_ui_graphics_FsrNative_describeVulkanRuntime(JNIEnv* env, jclass, jlong handle) {
    auto* runtime = reinterpret_cast<MagpieVulkanRuntimeV1*>(static_cast<uintptr_t>(handle));
    if (!runtime) return env->NewStringUTF("Vulkan runtime handle is null");
    MagpieBackendProbeV1 probe{}; probe.struct_size = sizeof(probe);
    if (magpie_vulkan_runtime_describe_v1(runtime, &probe) != MAGPIE_OK)
        return env->NewStringUTF("Vulkan runtime description failed");
    std::string result = probe.device_name;
    result += " | ";
    result += probe.reason;
    return env->NewStringUTF(result.c_str());
}
extern "C" JNIEXPORT void JNICALL
Java_com_gamecore_ui_graphics_FsrNative_destroyVulkanRuntime(JNIEnv*, jclass, jlong handle) {
    auto* runtime = reinterpret_cast<MagpieVulkanRuntimeV1*>(static_cast<uintptr_t>(handle));
    magpie_vulkan_runtime_destroy_v1(runtime);
}

// ---------------------------------------------------------------- M4: FSR engine

namespace {
// Shared bitmap validation + locking for the CPU and Vulkan paths. Both formats are RGBA_8888 and
// both locks are held for the duration of the call; the unlock order mirrors the lock order.
MagpieStatusV1 lockPair(JNIEnv* env, jobject src, jobject dst, AndroidBitmapInfo* si,
                        AndroidBitmapInfo* di, void** sp, void** dp, std::string* error) {
    if (AndroidBitmap_getInfo(env, src, si) != ANDROID_BITMAP_RESULT_SUCCESS ||
        AndroidBitmap_getInfo(env, dst, di) != ANDROID_BITMAP_RESULT_SUCCESS ||
        si->format != ANDROID_BITMAP_FORMAT_RGBA_8888 ||
        di->format != ANDROID_BITMAP_FORMAT_RGBA_8888 ||
        !si->width || !si->height || !di->width || !di->height) {
        *error = "invalid bitmap format or dimensions";
        return MAGPIE_INVALID_ARGUMENT;
    }
    if (AndroidBitmap_lockPixels(env, src, sp) != ANDROID_BITMAP_RESULT_SUCCESS) {
        *error = "input bitmap lock failed";
        return MAGPIE_INTERNAL_ERROR;
    }
    if (AndroidBitmap_lockPixels(env, dst, dp) != ANDROID_BITMAP_RESULT_SUCCESS) {
        AndroidBitmap_unlockPixels(env, src);
        *error = "output bitmap lock failed";
        return MAGPIE_INTERNAL_ERROR;
    }
    return MAGPIE_OK;
}

void unlockPair(JNIEnv* env, jobject src, jobject dst) {
    AndroidBitmap_unlockPixels(env, dst);
    AndroidBitmap_unlockPixels(env, src);
}
}  // namespace

extern "C" JNIEXPORT jlong JNICALL
Java_com_gamecore_ui_graphics_FsrNative_createVulkanFsrEngine(JNIEnv*, jclass, jint inW, jint inH,
                                                              jint outW, jint outH) {
    if (inW <= 0 || inH <= 0 || outW <= 0 || outH <= 0) return 0;
    MagpieVulkanFsrEngineConfigV1 cfg{};
    cfg.struct_size = sizeof(cfg);
    cfg.easu_spirv_words = magpie_shaders::kEasuSpv;
    cfg.easu_spirv_word_count = magpie_shaders::kEasuSpv_word_count;
    cfg.rcas_spirv_words = magpie_shaders::kRcasSpv;
    cfg.rcas_spirv_word_count = magpie_shaders::kRcasSpv_word_count;
    cfg.input_width = static_cast<uint32_t>(inW);
    cfg.input_height = static_cast<uint32_t>(inH);
    cfg.output_width = static_cast<uint32_t>(outW);
    cfg.output_height = static_cast<uint32_t>(outH);
    MagpieVulkanFsrEngineV1* engine = nullptr;
    if (magpie_vulkan_fsr_engine_create_v1(&engine, &cfg) != MAGPIE_OK || !engine) return 0;
    return static_cast<jlong>(reinterpret_cast<uintptr_t>(engine));
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_gamecore_ui_graphics_FsrNative_describeVulkanFsrEngine(JNIEnv* env, jclass, jlong handle) {
    auto* engine = reinterpret_cast<MagpieVulkanFsrEngineV1*>(static_cast<uintptr_t>(handle));
    if (!engine) return env->NewStringUTF("FSR engine handle is null");
    MagpieBackendProbeV1 probe{};
    probe.struct_size = sizeof(probe);
    if (magpie_vulkan_fsr_engine_describe_v1(engine, &probe) != MAGPIE_OK)
        return env->NewStringUTF("FSR engine description failed");
    std::string out = probe.device_name;
    out += " | runtime=";
    out += probe.runtime_available ? "available" : "unavailable";
    out += " compute=";
    out += probe.compute_available ? "available" : "unavailable";
    out += " fsr_dispatch=";
    out += probe.fsr_dispatch_validated ? "validated" : "not-validated";
    out += " | ";
    out += probe.reason;
    return env->NewStringUTF(out.c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_gamecore_ui_graphics_FsrNative_vulkanFsrProcess(JNIEnv* env, jclass, jlong handle,
                                                         jobject src, jobject dst,
                                                         jfloat sharpness) {
    auto* engine = reinterpret_cast<MagpieVulkanFsrEngineV1*>(static_cast<uintptr_t>(handle));
    if (!engine) return env->NewStringUTF("FSR engine handle is null");
    AndroidBitmapInfo si{}, di{};
    void *sp = nullptr, *dp = nullptr;
    std::string error;
    const MagpieStatusV1 locked = lockPair(env, src, dst, &si, &di, &sp, &dp, &error);
    if (locked != MAGPIE_OK) return env->NewStringUTF(error.c_str());
    if (si.width != di.width / 2 || si.height != di.height / 2) {
        unlockPair(env, src, dst);
        return env->NewStringUTF("engine dispatch expects a 2x output bitmap");
    }
    const MagpieRgba8ViewV1 input{static_cast<const uint8_t*>(sp), si.width, si.height, si.stride};
    MagpieRgba8OutputV1 output{static_cast<uint8_t*>(dp), di.width, di.height, di.stride};
    MagpieFsrTimingV1 timing{};
    const MagpieStatusV1 status =
        magpie_vulkan_fsr_engine_process_rgba8_v1(engine, &input, &output, sharpness, &timing);
    unlockPair(env, src, dst);
    if (status != MAGPIE_OK)
        return env->NewStringUTF(("Vulkan FSR dispatch failed; SDK status=" +
                                  std::to_string(static_cast<int>(status))).c_str());
    char buffer[160];
    std::snprintf(buffer, sizeof(buffer),
                  "Vulkan EASU+RCAS dispatched; easu_ms=%.3f rcas_ms=%.3f total_ms=%.3f",
                  timing.easu_ms, timing.rcas_ms, timing.dispatch_total_ms);
    return env->NewStringUTF(buffer);
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_gamecore_ui_graphics_FsrNative_vulkanFsrResize(JNIEnv*, jclass, jlong handle, jint inW,
                                                        jint inH, jint outW, jint outH) {
    auto* engine = reinterpret_cast<MagpieVulkanFsrEngineV1*>(static_cast<uintptr_t>(handle));
    if (!engine) return env->NewStringUTF("FSR engine handle is null");
    const MagpieStatusV1 status =
        magpie_vulkan_fsr_engine_resize_v1(engine, static_cast<uint32_t>(inW),
                                           static_cast<uint32_t>(inH),
                                           static_cast<uint32_t>(outW),
                                           static_cast<uint32_t>(outH));
    if (status != MAGPIE_OK)
        return env->NewStringUTF(("Vulkan FSR resize failed; SDK status=" +
                                  std::to_string(static_cast<int>(status))).c_str());
    return env->NewStringUTF("Resized.");
}

extern "C" JNIEXPORT void JNICALL
Java_com_gamecore_ui_graphics_FsrNative_destroyVulkanFsrEngine(JNIEnv*, jclass, jlong handle) {
    auto* engine = reinterpret_cast<MagpieVulkanFsrEngineV1*>(static_cast<uintptr_t>(handle));
    magpie_vulkan_fsr_engine_destroy_v1(engine);
}
