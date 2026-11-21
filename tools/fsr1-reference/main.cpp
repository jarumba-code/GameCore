#include "magpie/fsr1/easu.h"
#include "magpie/fsr1/rcas.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#ifndef MAGPIE_TESTDATA_DIR
#define MAGPIE_TESTDATA_DIR "testdata"
#endif

using magpie::fsr1::Image;
using magpie::fsr1::Rgb;
using Clock = std::chrono::steady_clock;

namespace {

std::string token(std::istream& input) {
    std::string value;
    while (input >> value) {
        if (!value.empty() && value[0] == '#') {
            std::string ignored;
            std::getline(input, ignored);
            continue;
        }
        return value;
    }
    throw std::runtime_error("unexpected EOF in PPM header");
}

Image readPpm(const std::string& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw std::runtime_error("cannot open fixture: " + path);
    if (token(stream) != "P6") throw std::runtime_error("expected binary P6 PPM: " + path);
    const int width = std::stoi(token(stream));
    const int height = std::stoi(token(stream));
    if (std::stoi(token(stream)) != 255) throw std::runtime_error("fixture max value must be 255");
    stream.get(); // PPM header separator before pixel bytes.
    Image image(width, height);
    std::vector<unsigned char> bytes(image.size() * 3);
    stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (stream.gcount() != static_cast<std::streamsize>(bytes.size())) throw std::runtime_error("short PPM pixel data");
    for (int y=0;y<height;++y) for (int x=0;x<width;++x) {
        const auto i=(static_cast<std::size_t>(y)*width+x)*3;
        image.at(x,y)={bytes[i]/255.0f,bytes[i+1]/255.0f,bytes[i+2]/255.0f};
    }
    return image;
}

void writePpm(const std::string& path, const Image& image) {
    std::ofstream stream(path, std::ios::binary);
    if (!stream) throw std::runtime_error("cannot write output: " + path);
    stream << "P6\n" << image.width() << ' ' << image.height() << "\n255\n";
    for (int y=0;y<image.height();++y) for (int x=0;x<image.width();++x) {
        const auto p=image.at(x,y);
        for (float c : {p.r,p.g,p.b}) {
            const auto b=static_cast<unsigned char>(std::lround(std::clamp(c,0.0f,1.0f)*255.0f));
            stream.put(static_cast<char>(b));
        }
    }
}

Image bilinear(const Image& input, int width, int height) {
    Image out(width,height);
    for (int y=0;y<height;++y) for (int x=0;x<width;++x) {
        const float sx=(x+0.5f)*input.width()/width-0.5f;
        const float sy=(y+0.5f)*input.height()/height-0.5f;
        const int x0=static_cast<int>(std::floor(sx)), y0=static_cast<int>(std::floor(sy));
        const float fx=sx-x0, fy=sy-y0;
        const auto at=[&](int px,int py) { return input.at(std::clamp(px,0,input.width()-1),std::clamp(py,0,input.height()-1)); };
        const auto a=at(x0,y0),b=at(x0+1,y0),c=at(x0,y0+1),d=at(x0+1,y0+1);
        const auto channel=[&](float aa,float bb,float cc,float dd) {
            return (aa*(1-fx)+bb*fx)*(1-fy)+(cc*(1-fx)+dd*fx)*fy;
        };
        out.at(x,y)={channel(a.r,b.r,c.r,d.r),channel(a.g,b.g,c.g,d.g),channel(a.b,b.b,c.b,d.b)};
    }
    return out;
}

struct Quality { double psnr; double ssim; };
Quality compare(const Image& actual,const Image& reference) {
    if(actual.width()!=reference.width() || actual.height()!=reference.height()) throw std::runtime_error("quality image dimensions differ");
    double mse=0, meanA=0, meanB=0, varA=0, varB=0, cov=0;
    const double count=static_cast<double>(actual.size())*3.0;
    for(int y=0;y<actual.height();++y) for(int x=0;x<actual.width();++x) {
        const auto a=actual.at(x,y),b=reference.at(x,y);
        for(int ch=0;ch<3;++ch) {
            const double av=ch==0?a.r:(ch==1?a.g:a.b), bv=ch==0?b.r:(ch==1?b.g:b.b);
            mse+=(av-bv)*(av-bv); meanA+=av; meanB+=bv;
        }
    }
    mse/=count; meanA/=count; meanB/=count;
    for(int y=0;y<actual.height();++y) for(int x=0;x<actual.width();++x) {
        const auto a=actual.at(x,y),b=reference.at(x,y);
        for(int ch=0;ch<3;++ch) {
            const double av=(ch==0?a.r:(ch==1?a.g:a.b))-meanA;
            const double bv=(ch==0?b.r:(ch==1?b.g:b.b))-meanB;
            varA+=av*av; varB+=bv*bv; cov+=av*bv;
        }
    }
    varA/=count; varB/=count; cov/=count;
    const double c1=0.01*0.01,c2=0.03*0.03;
    const double ssim=((2*meanA*meanB+c1)*(2*cov+c2))/((meanA*meanA+meanB*meanB+c1)*(varA+varB+c2));
    return {mse==0?99.0:10.0*std::log10(1.0/mse),ssim};
}

bool withinGolden(const Image& actual,const Image& golden) {
    if(actual.width()!=golden.width()||actual.height()!=golden.height()) return false;
    constexpr float maxQuantizationError=1.0f/255.0f+1e-6f;
    for(int y=0;y<actual.height();++y) for(int x=0;x<actual.width();++x) {
        const auto a=actual.at(x,y),g=golden.at(x,y);
        if(std::abs(a.r-g.r)>maxQuantizationError||std::abs(a.g-g.g)>maxQuantizationError||std::abs(a.b-g.b)>maxQuantizationError) return false;
    }
    return true;
}

bool finite(const Image& image) {
    for(int y=0;y<image.height();++y) for(int x=0;x<image.width();++x) {
        const auto p=image.at(x,y);
        if(!std::isfinite(p.r)||!std::isfinite(p.g)||!std::isfinite(p.b)) return false;
    }
    return true;
}

void assertConstantPasses() {
    const Image solid(9,7,{0.25f,0.5f,0.75f});
    const auto up=magpie::fsr1::easu(solid,18,14);
    const auto sharp=magpie::fsr1::rcas(up,2.0f);
    if(up.width()!=18||up.height()!=14||sharp.width()!=18||sharp.height()!=14||!finite(sharp))
        throw std::runtime_error("pass dimensions or finite-value invariant failed");
    for(int y=0;y<sharp.height();++y) for(int x=0;x<sharp.width();++x) {
        const auto p=sharp.at(x,y);
        if(std::abs(p.r-0.25f)>1e-5f||std::abs(p.g-0.5f)>1e-5f||std::abs(p.b-0.75f)>1e-5f)
            throw std::runtime_error("uniform input was not preserved");
    }
    bool rejected=false;
    try { (void)magpie::fsr1::easu(solid,27,21); } catch(const std::invalid_argument&) { rejected=true; }
    if(!rejected) throw std::runtime_error("EASU accepted more than 4x area scaling");
}

}  // namespace

int main(int argc,char** argv) try {
    int iterations=12;
    std::string outputDir;
    for(int i=1;i<argc;++i) {
        const std::string arg=argv[i];
        if(arg=="--iterations"&&i+1<argc) iterations=std::stoi(argv[++i]);
        else if(arg=="--output-dir"&&i+1<argc) outputDir=argv[++i];
        else throw std::invalid_argument("usage: fsr1_reference [--iterations N] [--output-dir DIR]");
    }
    if(iterations<1||iterations>10000) throw std::invalid_argument("iterations must be 1..10000");
    assertConstantPasses();
    std::cout << "mode=CPU-reference; backend=scalar-C++; source=AMD-FSR1-v1.0.2; iterations=" << iterations << '\n';
    for(const std::string name:{"diagonal","checker","shape"}) {
        const std::string base=std::string(MAGPIE_TESTDATA_DIR)+"/"+name;
        const auto input=readPpm(base+"_input_160x90.ppm");
        const auto reference=readPpm(base+"_reference_320x180.ppm");
        const auto golden=readPpm(base+"_golden_fsr1_320x180.ppm");
        auto up=magpie::fsr1::easu(input,reference.width(),reference.height());
        auto combined=magpie::fsr1::rcas(up,2.0f);
        const auto linear=bilinear(input,reference.width(),reference.height());
        const auto qLinear=compare(linear,reference), qEasu=compare(up,reference), qCombined=compare(combined,reference);
        if(!outputDir.empty()) writePpm(outputDir+"/"+name+"_easu_rcas.ppm",combined);
        const double minPsnr = name=="diagonal" ? 31.0 : (name=="checker" ? 16.5 : 24.5);
        const double minSsim = name=="diagonal" ? 0.98 : (name=="checker" ? 0.90 : 0.70);
        if(!finite(up)||!finite(combined)||!withinGolden(combined,golden)||qCombined.psnr<minPsnr||qCombined.ssim<minSsim)
            throw std::runtime_error(name+": numerical or quality floor failed (golden="+std::string(withinGolden(combined,golden)?"match":"mismatch")+", PSNR="+std::to_string(qCombined.psnr)+", global SSIM="+std::to_string(qCombined.ssim)+")");

        // Warm up, then time each pass independently on the target CPU.
        for(int i=0;i<3;++i) { up=magpie::fsr1::easu(input,reference.width(),reference.height()); combined=magpie::fsr1::rcas(up,2.0f); }
        auto start=Clock::now();
        for(int i=0;i<iterations;++i) up=magpie::fsr1::easu(input,reference.width(),reference.height());
        const double easuMs=std::chrono::duration<double,std::milli>(Clock::now()-start).count()/iterations;
        start=Clock::now();
        for(int i=0;i<iterations;++i) combined=magpie::fsr1::rcas(up,2.0f);
        const double rcasMs=std::chrono::duration<double,std::milli>(Clock::now()-start).count()/iterations;
        const auto measured=compare(combined,reference);
        std::cout << std::fixed << std::setprecision(4) << name
                  << " easu_ms=" << easuMs << " rcas_ms=" << rcasMs
                  << " bilinear_psnr_db=" << qLinear.psnr << " easu_psnr_db=" << qEasu.psnr
                  << " easu_rcas_psnr_db=" << measured.psnr
                  << " bilinear_ssim=" << qLinear.ssim << " easu_rcas_ssim=" << measured.ssim << '\n';
    }
    std::cout << "PASS: deterministic fixture checks, pass ordering, golden-output match, finite output, dimensions, scale bounds, PSNR/SSIM floors\n";
    return 0;
} catch(const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
}
