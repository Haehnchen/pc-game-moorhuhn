#include "render/lookup.hpp"

#include <stdexcept>

namespace moorhuhn::render {

std::uint8_t lookup_index(IndexTableView table, std::uint32_t x, std::uint32_t y) {
    if (table.width == 0 || table.height == 0 || table.row_stride < table.width) {
        throw std::invalid_argument("Index table dimensions or stride are invalid");
    }

    const auto required = static_cast<std::uint64_t>(table.height - 1) * table.row_stride + table.width;

    if (required > table.indices.size()) {
        throw std::invalid_argument("Index table storage is truncated");
    }

    if (x >= table.width || y >= table.height) {
        throw std::out_of_range("Index table coordinate is outside its bounds");
    }

    return table.indices[static_cast<std::size_t>(y) * table.row_stride + x];
}

contracts::RgbaPixel palette_color(std::span<const contracts::RgbaPixel> palette, std::uint8_t index) {
    if (index >= palette.size()) {
        throw std::out_of_range("Palette index is unavailable");
    }

    return palette[index];
}

contracts::RgbaPixel source_over(contracts::RgbaPixel source, contracts::RgbaPixel destination) {
    if (source.a == 0) {
        return destination;
    }

    if (source.a == 255 || destination.a == 0) {
        return source;
    }

    const std::uint32_t inverse = 255 - source.a;
    const std::uint32_t alpha = source.a * 255U + destination.a * inverse;
    auto channel = [&](std::uint8_t foreground, std::uint8_t background) {
        const auto numerator = foreground * source.a * 255U + background * destination.a * inverse;

        return static_cast<std::uint8_t>((numerator + alpha / 2) / alpha);
    };

    return {channel(source.r, destination.r), channel(source.g, destination.g), channel(source.b, destination.b), static_cast<std::uint8_t>((alpha + 127) / 255)};
}

} // namespace moorhuhn::render
