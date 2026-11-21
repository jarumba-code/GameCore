#pragma once

#include <cstddef>
#include <vector>

namespace magpie::fsr1 {

struct Rgb {
    float r{};
    float g{};
    float b{};
};

class Image {
public:
    Image() = default;
    Image(int width, int height, Rgb fill = {});

    int width() const noexcept { return width_; }
    int height() const noexcept { return height_; }
    std::size_t size() const noexcept { return pixels_.size(); }
    const Rgb& at(int x, int y) const;
    Rgb& at(int x, int y);

private:
    int width_{};
    int height_{};
    std::vector<Rgb> pixels_;
};

}  // namespace magpie::fsr1
