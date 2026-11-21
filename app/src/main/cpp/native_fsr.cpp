// Android-only adapter: Bitmap locks feed the stable C SDK API; no Java pixel
// array is created. The SDK owns image math and the caller owns bitmaps.
#include <android/bitmap.h>
#include <jni.h>
#include <chrono>
#include <cstdint>
#include <string>
#include "magpie/magpie.h"

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
