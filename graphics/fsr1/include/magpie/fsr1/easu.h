#pragma once

#include "magpie/fsr1/image.h"

namespace magpie::fsr1 {

// CPU reference port of the FSR 1 EASU pass. Input and output use normalized
// linear RGB values and clamp-to-edge sampling. EASU does not sharpen; RCAS is
// a separate pass. The supported area scale is 1x through 4x.
Image easu(const Image& input, int outputWidth, int outputHeight);

}  // namespace magpie::fsr1
