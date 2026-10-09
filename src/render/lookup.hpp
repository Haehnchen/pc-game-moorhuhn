#pragma once

#include "contracts/assets.hpp"

namespace moorhuhn::render {

struct IndexTableView {
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t row_stride{};
    std::span<const std::uint8_t> indices;
};

// Callers choose table axes and palette channels. These functions infer no effects.
[[nodiscard]] std::uint8_t lookup_index(IndexTableView table, std::uint32_t x, std::uint32_t y);
[[nodiscard]] contracts::RgbaPixel palette_color(std::span<const contracts::RgbaPixel> palette, std::uint8_t index);
// Straight-alpha sRGB byte arithmetic, round to nearest. Zero source alpha is inert.
[[nodiscard]] contracts::RgbaPixel source_over(contracts::RgbaPixel source, contracts::RgbaPixel destination);

} // namespace moorhuhn::render
