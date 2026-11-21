#pragma once

#include "magpie/fsr1/image.h"

namespace magpie::fsr1 {

// CPU reference port of the FSR 1 RCAS pass. `sharpnessStops` maps to the
// upstream FsrRcasCon parameter; zero is maximum sharpening, larger values
// reduce the lobe. RCAS preserves dimensions and must receive EASU output.
Image rcas(const Image& input, float sharpnessStops = 2.0f);

}  // namespace magpie::fsr1
