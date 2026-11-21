// Scalar CPU reference port of AMD FidelityFX FSR 1.0.2 RCAS.
// Copyright (c) 2021 Advanced Micro Devices, Inc. All rights reserved.
// Adapted from upstream; complete MIT notice: third_party/amd-fsr1/LICENSE.txt.
#include "magpie/fsr1/rcas.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace magpie::fsr1 {
namespace {

constexpr float kRcasLimit = 0.25f - 1.0f/16.0f;

Rgb sample(const Image& image, int x, int y) {
    x = std::clamp(x, 0, image.width() - 1);
    y = std::clamp(y, 0, image.height() - 1);
    return image.at(x, y);
}
float min3(float a,float b,float c) { return std::min(a,std::min(b,c)); }
float max3(float a,float b,float c) { return std::max(a,std::max(b,c)); }
float component(Rgb c,int i) { return i==0?c.r:(i==1?c.g:c.b); }

}  // namespace

Image rcas(const Image& input, float sharpnessStops) {
    if (input.width() <= 0 || input.height() <= 0) throw std::invalid_argument("RCAS requires a non-empty image");
    if (!std::isfinite(sharpnessStops) || sharpnessStops < 0.0f || sharpnessStops > 8.0f)
        throw std::invalid_argument("RCAS sharpness must be finite and between 0 and 8 stops");

    // FsrRcasCon(): sharpness is represented as a power-of-two multiplier.
    const float sharpness=std::exp2(-sharpnessStops);
    Image output(input.width(),input.height());
    for (int y=0;y<input.height();++y) for (int x=0;x<input.width();++x) {
        const Rgb b=sample(input,x,y-1), d=sample(input,x-1,y), e=sample(input,x,y);
        const Rgb f=sample(input,x+1,y), h=sample(input,x,y+1);
        // The upstream FSR_RCAS_DENOISE permutation is disabled by default.

        Rgb result{};
        for (int channel=0;channel<3;++channel) {
            const float bv=component(b,channel), dv=component(d,channel), ev=component(e,channel);
            const float fv=component(f,channel), hv=component(h,channel);
            const float mn4=std::min(min3(bv,dv,fv),hv);
            const float mx4=std::max(max3(bv,dv,fv),hv);
            const float hitMin=(std::min(mn4,ev)) / (4.0f*mx4 == 0.0f ? 1.0f : 4.0f*mx4);
            const float hitMax=(1.0f-std::max(mx4,ev)) / (4.0f*mn4-4.0f == 0.0f ? 1.0f : 4.0f*mn4-4.0f);
            const float lobe=std::max(-kRcasLimit,std::min(std::max(-hitMin,hitMax),0.0f))*sharpness;
            const float denom=4.0f*lobe+1.0f;
            const float value=denom != 0.0f ? (lobe*(bv+dv+fv+hv)+ev)/denom : ev;
            if (channel==0) result.r=value;
            else if (channel==1) result.g=value;
            else result.b=value;
        }
        output.at(x,y)=result;
    }
    return output;
}

}  // namespace magpie::fsr1
