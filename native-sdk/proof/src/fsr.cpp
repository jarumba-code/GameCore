#include "magpie/magpie.h"
#include "magpie/fsr1/easu.h"
#include "magpie/fsr1/rcas.h"
#include <cmath>
#include <limits>
#include <new>
namespace {
bool valid(const MagpieRgba8ViewV1& x) {
    return x.pixels && x.width && x.height && x.width <= 16384 && x.height <= 16384 && x.row_stride_bytes >= static_cast<size_t>(x.width)*4 && x.row_stride_bytes <= std::numeric_limits<size_t>::max()/x.height;
}
}
extern "C" MAGPIE_API MagpieStatusV1 magpie_fsr_cpu_process_rgba8_v1(const MagpieRgba8ViewV1* src, MagpieRgba8OutputV1* dst, float sharpness) {
    if (!src || !dst || !valid(*src) || !dst->pixels || !dst->width || !dst->height || dst->width < src->width || dst->height < src->height ||
        dst->width > 16384 || dst->height > 16384 || dst->row_stride_bytes < static_cast<size_t>(dst->width)*4 ||
        dst->row_stride_bytes > std::numeric_limits<size_t>::max()/dst->height || !std::isfinite(sharpness) || sharpness < 0 || sharpness > 8) return MAGPIE_INVALID_ARGUMENT;
    const double area=static_cast<double>(dst->width)*dst->height/(static_cast<double>(src->width)*src->height);
    if (area > 4.000001) return MAGPIE_INVALID_ARGUMENT;
    try {
        magpie::fsr1::Image input(static_cast<int>(src->width), static_cast<int>(src->height));
        for (uint32_t y=0;y<src->height;++y) for(uint32_t x=0;x<src->width;++x) {
            const uint8_t* p=src->pixels+y*src->row_stride_bytes+x*4;
            input.at(static_cast<int>(x),static_cast<int>(y))={p[0]/255.f,p[1]/255.f,p[2]/255.f};
        }
        auto mid=magpie::fsr1::easu(input,static_cast<int>(dst->width),static_cast<int>(dst->height));
        auto out=magpie::fsr1::rcas(mid,sharpness);
        for(uint32_t y=0;y<dst->height;++y) for(uint32_t x=0;x<dst->width;++x) {
            auto c=out.at(static_cast<int>(x),static_cast<int>(y)); uint8_t* p=dst->pixels+y*dst->row_stride_bytes+x*4;
            p[0]=static_cast<uint8_t>(c.r*255.f+0.5f); p[1]=static_cast<uint8_t>(c.g*255.f+0.5f); p[2]=static_cast<uint8_t>(c.b*255.f+0.5f); p[3]=255;
        }
        return MAGPIE_OK;
    } catch(const std::bad_alloc&) { return MAGPIE_INTERNAL_ERROR; }
      catch(...) { return MAGPIE_INTERNAL_ERROR; }
}
