#ifndef MAGPIE_SDK_PROOF_MAGPIE_HPP
#define MAGPIE_SDK_PROOF_MAGPIE_HPP
#include "magpie.h"
namespace magpie::sdk {
inline constexpr uint32_t kAbiVersion = 1;
using Status = MagpieStatusV1;
inline Status fsrCpu(const MagpieRgba8ViewV1& src, MagpieRgba8OutputV1& dst, float sharpnessStops) {
    return magpie_fsr_cpu_process_rgba8_v1(&src, &dst, sharpnessStops);
}
}
#endif
