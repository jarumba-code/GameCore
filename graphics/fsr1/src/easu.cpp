// Scalar CPU reference port of AMD FidelityFX FSR 1.0.2 EASU.
// Copyright (c) 2021 Advanced Micro Devices, Inc. All rights reserved.
// Adapted from upstream; complete MIT notice: third_party/amd-fsr1/LICENSE.txt.
#include "magpie/fsr1/easu.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace magpie::fsr1 {
namespace {

struct V2 { float x, y; };
struct V3 { float r, g, b; };

V3 rgb(Rgb c) { return {c.r, c.g, c.b}; }
V3 operator+(V3 a, V3 b) { return {a.r+b.r, a.g+b.g, a.b+b.b}; }
V3 operator*(V3 a, float s) { return {a.r*s, a.g*s, a.b*s}; }
V3 minimum(V3 a, V3 b) { return {std::min(a.r,b.r),std::min(a.g,b.g),std::min(a.b,b.b)}; }
V3 maximum(V3 a, V3 b) { return {std::max(a.r,b.r),std::max(a.g,b.g),std::max(a.b,b.b)}; }

Rgb sample(const Image& image, int x, int y) {
    x = std::clamp(x, 0, image.width() - 1);
    y = std::clamp(y, 0, image.height() - 1);
    return image.at(x, y);
}
float luma2(Rgb c) { return c.b * 0.5f + (c.r * 0.5f + c.g); }
float sat(float x) { return std::clamp(x, 0.0f, 1.0f); }

void setDirection(V2& direction, float& length, V2 pp,
                  bool topLeft, bool topRight, bool bottomLeft, bool bottomRight,
                  float a, float b, float c, float d, float e) {
    float w = 0.0f;
    if (topLeft) w = (1.0f-pp.x)*(1.0f-pp.y);
    if (topRight) w = pp.x*(1.0f-pp.y);
    if (bottomLeft) w = (1.0f-pp.x)*pp.y;
    if (bottomRight) w = pp.x*pp.y;

    const float dc = d-c, cb = c-b;
    const float dirX = d-b;
    const float denX = std::max(std::abs(dc), std::abs(cb));
    direction.x += dirX*w;
    if (denX > 0.0f) {
        const float lenX = sat(std::abs(dirX)/denX);
        length += lenX*lenX*w;
    }

    const float ec = e-c, ca = c-a;
    const float dirY = e-a;
    const float denY = std::max(std::abs(ec), std::abs(ca));
    direction.y += dirY*w;
    if (denY > 0.0f) {
        const float lenY = sat(std::abs(dirY)/denY);
        length += lenY*lenY*w;
    }
}

void tap(V3& color, float& weight, V2 offset, V2 direction, V2 anisotropy,
         float lobe, float clip, V3 value) {
    const float vx = (offset.x*direction.x + offset.y*direction.y)*anisotropy.x;
    const float vy = (offset.x*-direction.y + offset.y*direction.x)*anisotropy.y;
    const float d2 = std::min(vx*vx + vy*vy, clip);
    float wb = (0.4f*d2 - 1.0f); wb *= wb;
    float wa = (lobe*d2 - 1.0f); wa *= wa;
    wb = (25.0f/16.0f)*wb - (25.0f/16.0f - 1.0f);
    const float w = wb*wa;
    color = color + value*w;
    weight += w;
}

}  // namespace

Image easu(const Image& input, int outputWidth, int outputHeight) {
    if (input.width() <= 0 || input.height() <= 0 || outputWidth < input.width() || outputHeight < input.height())
        throw std::invalid_argument("EASU requires positive dimensions and non-downscaling output");
    const double areaScale = static_cast<double>(outputWidth) * outputHeight /
                             (static_cast<double>(input.width()) * input.height());
    if (areaScale > 4.0 + 1e-6) throw std::invalid_argument("FSR 1 EASU supports up to 4x area scaling");

    Image output(outputWidth, outputHeight);
    const float sx = static_cast<float>(input.width()) / static_cast<float>(outputWidth);
    const float sy = static_cast<float>(input.height()) / static_cast<float>(outputHeight);
    for (int y=0; y<outputHeight; ++y) for (int x=0; x<outputWidth; ++x) {
        // FsrEasuCon mapping from destination pixel to the fractional source position.
        const float px = static_cast<float>(x)*sx + 0.5f*sx - 0.5f;
        const float py = static_cast<float>(y)*sy + 0.5f*sy - 0.5f;
        const int fx = static_cast<int>(std::floor(px));
        const int fy = static_cast<int>(std::floor(py));
        const V2 pp{px-fx, py-fy};

        // Names and neighborhoods match the twelve taps in FsrEasuF.
        const Rgb b=sample(input,fx,fy-1), c=sample(input,fx+1,fy-1);
        const Rgb e=sample(input,fx-1,fy), f=sample(input,fx,fy);
        const Rgb g=sample(input,fx+1,fy), h=sample(input,fx+2,fy);
        const Rgb i=sample(input,fx-1,fy+1), j=sample(input,fx,fy+1);
        const Rgb k=sample(input,fx+1,fy+1), l=sample(input,fx+2,fy+1);
        const Rgb n=sample(input,fx,fy+2), o=sample(input,fx+1,fy+2);

        V2 direction{0,0}; float length=0;
        setDirection(direction,length,pp,true,false,false,false,luma2(b),luma2(e),luma2(f),luma2(g),luma2(j));
        setDirection(direction,length,pp,false,true,false,false,luma2(c),luma2(f),luma2(g),luma2(h),luma2(k));
        setDirection(direction,length,pp,false,false,true,false,luma2(f),luma2(i),luma2(j),luma2(k),luma2(n));
        setDirection(direction,length,pp,false,false,false,true,luma2(g),luma2(j),luma2(k),luma2(l),luma2(o));

        const float dir2=direction.x*direction.x + direction.y*direction.y;
        if (dir2 < 1.0f/32768.0f) direction={1,0};
        else { const float inv=1.0f/std::sqrt(dir2); direction.x*=inv; direction.y*=inv; }
        length *= 0.5f; length *= length;
        const float normalizedDir2=direction.x*direction.x + direction.y*direction.y;
        const float major=std::max(std::abs(direction.x),std::abs(direction.y));
        const float stretch=major > 0 ? normalizedDir2/major : 1.0f;
        const V2 anisotropy{1.0f+(stretch-1.0f)*length, 1.0f-0.5f*length};
        const float lobe=0.5f+((0.25f-0.04f)-0.5f)*length;
        const float clip=lobe > 0 ? 1.0f/lobe : 0.0f;

        const V3 min4=minimum(minimum(minimum(rgb(f),rgb(g)),rgb(j)),rgb(k));
        const V3 max4=maximum(maximum(maximum(rgb(f),rgb(g)),rgb(j)),rgb(k));
        V3 accum{0,0,0}; float total=0;
        auto add=[&](float ox,float oy,Rgb color) mutable {
            tap(accum,total,{ox-pp.x,oy-pp.y},direction,anisotropy,lobe,clip,rgb(color));
        };
        add(0,-1,b); add(1,-1,c); add(-1,1,i); add(0,1,j); add(0,0,f); add(-1,0,e);
        add(1,1,k); add(2,1,l); add(2,0,h); add(1,0,g); add(1,2,o); add(0,2,n);
        const float reciprocal=std::abs(total)>1e-20f ? 1.0f/total : 0.0f;
        V3 result=minimum(max4,maximum(min4,accum*reciprocal));
        output.at(x,y)={result.r,result.g,result.b};
    }
    return output;
}

}  // namespace magpie::fsr1
