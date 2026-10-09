#include "render/framebuffer.hpp"

#include <stdexcept>
#include <type_traits>

namespace moorhuhn::render {

static_assert(sizeof(contracts::RgbaPixel) == 4);
static_assert(std::is_trivially_copyable_v<contracts::RgbaPixel>);

Framebuffer::Framebuffer(contracts::RgbaPixel clear) : pixels_(static_cast<std::size_t>(width) * height, clear) {}

const contracts::RgbaPixel& Framebuffer::at(contracts::ScreenPoint point) const {
    if (point.x < 0 || point.y < 0 || point.x >= width || point.y >= height) {
        throw std::out_of_range("Framebuffer pixel is outside 640 x 480");
    }

    return pixels_[static_cast<std::size_t>(point.y) * width + point.x];
}

std::span<const contracts::RgbaPixel> Framebuffer::pixels() const noexcept {
    return pixels_;
}

std::span<const std::uint8_t> Framebuffer::bytes() const noexcept {
    return {reinterpret_cast<const std::uint8_t*>(pixels_.data()), pixels_.size() * sizeof(contracts::RgbaPixel)};
}

} // namespace moorhuhn::render
