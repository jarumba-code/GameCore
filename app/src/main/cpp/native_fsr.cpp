// JNI bitmap bridge to the retained CPU reference. Pixel locks avoid a Java byte-array copy.
#include <jni.h>
#include <android/bitmap.h>
#include <chrono>
#include <cstdint>
#include <exception>
#include <string>
#include "magpie/fsr1/easu.h"
#include "magpie/fsr1/rcas.h"
#ifdef MAGPIE_HAS_VULKAN
#include <vulkan/vulkan.h>
#endif
using namespace magpie::fsr1;
extern "C" JNIEXPORT jstring JNICALL Java_com_gamecore_ui_graphics_FsrNative_capabilities(JNIEnv* e,jclass) {
 std::string s="CPU reference available; Vulkan FSR dispatch not integrated; GLES dispatch not integrated";
#ifdef MAGPIE_HAS_VULKAN
 VkInstance i=VK_NULL_HANDLE; VkApplicationInfo a{VK_STRUCTURE_TYPE_APPLICATION_INFO}; a.pApplicationName="Android Magpie"; a.apiVersion=VK_API_VERSION_1_0;
 VkInstanceCreateInfo c{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; c.pApplicationInfo=&a;
 if(vkCreateInstance(&c,nullptr,&i)==VK_SUCCESS){uint32_t n=0; if(vkEnumeratePhysicalDevices(i,&n,nullptr)==VK_SUCCESS&&n)s+="; Vulkan runtime present (probe only)";else s+="; Vulkan runtime has no enumerated device";vkDestroyInstance(i,nullptr);}else s+="; Vulkan instance creation unavailable";
#else
 s+="; Vulkan loader unavailable at native build time";
#endif
 return e->NewStringUTF(s.c_str());
}
extern "C" JNIEXPORT jstring JNICALL Java_com_gamecore_ui_graphics_FsrNative_processCpu(JNIEnv* e,jclass,jobject src,jobject dst,jfloat sharpness){
 AndroidBitmapInfo si{},di{};
 if(AndroidBitmap_getInfo(e,src,&si)!=ANDROID_BITMAP_RESULT_SUCCESS||AndroidBitmap_getInfo(e,dst,&di)!=ANDROID_BITMAP_RESULT_SUCCESS||si.format!=ANDROID_BITMAP_FORMAT_RGBA_8888||di.format!=ANDROID_BITMAP_FORMAT_RGBA_8888||di.width<si.width||di.height<si.height||di.width>si.width*4||di.height>si.height*4)return e->NewStringUTF("invalid bitmap format or unsupported scale");
 void *sp=nullptr,*dp=nullptr;if(AndroidBitmap_lockPixels(e,src,&sp)!=ANDROID_BITMAP_RESULT_SUCCESS)return e->NewStringUTF("input bitmap lock failed");if(AndroidBitmap_lockPixels(e,dst,&dp)!=ANDROID_BITMAP_RESULT_SUCCESS){AndroidBitmap_unlockPixels(e,src);return e->NewStringUTF("output bitmap lock failed");}
 try{auto start=std::chrono::steady_clock::now();Image input((int)si.width,(int)si.height);auto*pixels=(const uint8_t*)sp;
 for(uint32_t y=0;y<si.height;y++)for(uint32_t x=0;x<si.width;x++){auto*p=pixels+y*si.stride+x*4;input.at((int)x,(int)y)={p[0]/255.f,p[1]/255.f,p[2]/255.f};}
 auto mid=easu(input,(int)di.width,(int)di.height);auto outImage=rcas(mid,sharpness);auto*out=(uint8_t*)dp;
 for(uint32_t y=0;y<di.height;y++)for(uint32_t x=0;x<di.width;x++){auto c=outImage.at((int)x,(int)y);auto*p=out+y*di.stride+x*4;p[0]=(uint8_t)(c.r*255.f+.5f);p[1]=(uint8_t)(c.g*255.f+.5f);p[2]=(uint8_t)(c.b*255.f+.5f);p[3]=255;}
 double ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();AndroidBitmap_unlockPixels(e,dst);AndroidBitmap_unlockPixels(e,src);return e->NewStringUTF(("CPU EASU+RCAS elapsed_ms="+std::to_string(ms)+"; GPU timing not measured").c_str());
 }catch(const std::exception&x){AndroidBitmap_unlockPixels(e,dst);AndroidBitmap_unlockPixels(e,src);return e->NewStringUTF(x.what());}
}
