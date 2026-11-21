#include "magpie/fsr1/image.h"

#include <stdexcept>

namespace magpie::fsr1 {

Image::Image(int width, int height, Rgb fill) : width_(width), height_(height) {
    if (width <= 0 || height <= 0) throw std::invalid_argument("image dimensions must be positive");
    pixels_.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height), fill);
}

const Rgb& Image::at(int x, int y) const {
    if (x < 0 || y < 0 || x >= width_ || y >= height_) throw std::out_of_range("pixel coordinate");
    return pixels_[static_cast<std::size_t>(y) * static_cast<std::size_t>(width_) + static_cast<std::size_t>(x)];
}

Rgb& Image::at(int x, int y) {
    if (x < 0 || y < 0 || x >= width_ || y >= height_) throw std::out_of_range("pixel coordinate");
    return pixels_[static_cast<std::size_t>(y) * static_cast<std::size_t>(width_) + static_cast<std::size_t>(x)];
}

}  // namespace magpie::fsr1
