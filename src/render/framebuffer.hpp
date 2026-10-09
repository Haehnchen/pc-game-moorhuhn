#pragma once

#include "contracts/assets.hpp"

#include <cstdint>
#include <span>
#include <vector>

namespace moorhuhn::render {

class Renderer;

class Framebuffer {
public:
    explicit Framebuffer(contracts::RgbaPixel clear = {0, 0, 0, 255});
    [[nodiscard]] const contracts::RgbaPixel& at(contracts::ScreenPoint point) const;
    [[nodiscard]] std::span<const contracts::RgbaPixel> pixels() const noexcept;
    [[nodiscard]] std::span<const std::uint8_t> bytes() const noexcept;
    static constexpr int width = contracts::logical_width;
    static constexpr int height = contracts::logical_height;

private:
    friend class Renderer;
    std::vector<contracts::RgbaPixel> pixels_;
};

} // namespace moorhuhn::render
