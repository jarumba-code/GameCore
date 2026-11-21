#include "magpie/magpie.hpp"
#include <array>
#include <cstdio>
int main(){
 if(magpie_core_abi_version_v1()!=magpie::sdk::kAbiVersion)return 1;
 std::array<uint8_t,8*8*4> src{};for(int y=0;y<8;++y)for(int x=0;x<8;++x){auto i=(y*8+x)*4;src[i]=src[i+1]=src[i+2]=((x+y)&1)?240:16;src[i+3]=255;}
 std::array<uint8_t,16*16*4> dst{};MagpieRgba8ViewV1 in{src.data(),8,8,8*4};MagpieRgba8OutputV1 out{dst.data(),16,16,16*4};if(magpie::sdk::fsrCpu(in,out,2.0f)!=MAGPIE_OK)return 2;
 if(dst[3]!=255||dst[(15*16+15)*4+3]!=255)return 3;
 MagpieBackendProbeV1 vk{};vk.struct_size=sizeof(vk);auto vs=magpie_vulkan_probe_v1(&vk);if(vs==MAGPIE_OK&&vk.fsr_dispatch_validated)return 4;
 MagpieBackendProbeV1 gl{};gl.struct_size=sizeof(gl);auto gs=magpie_gles_probe_v1(&gl);if(gs==MAGPIE_OK&&gl.fsr_dispatch_validated)return 5;
 std::printf("abi=%u cpu_fsr=passed vulkan_probe=%s fsr_dispatch=%u gles_probe=%s fsr_dispatch=%u\n",magpie_core_abi_version_v1(),vk.reason,vk.fsr_dispatch_validated,gl.reason,gl.fsr_dispatch_validated);return 0;
}
