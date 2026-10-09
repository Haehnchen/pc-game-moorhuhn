#pragma once

#include "contracts/time.hpp"

#include <compare>
#include <cstdint>
#include <span>
#include <string>

namespace moorhuhn::contracts {

// Exact manifest spelling. IDs are owned names, never paths or array ordinals.
// Image, sound, and palette registries have separate namespaces.
struct AssetId {
    std::string value;

    auto operator<=>(const AssetId&) const = default;
};

struct FrameId {
    AssetId image;
    std::uint32_t index{}; // Zero-based manifest frames[] index.

    auto operator<=>(const FrameId&) const = default;
};

struct RgbaPixel {
    std::uint8_t r{};
    std::uint8_t g{};
    std::uint8_t b{};
    std::uint8_t a{};

    auto operator<=>(const RgbaPixel&) const = default;
};

// Borrowed palette indices; the draw command selects their interpretation.
struct MaskView {
    std::int32_t width{};
    std::int32_t height{};
    std::uint32_t row_stride{}; // Bytes per row.
    std::span<const std::uint8_t> indices;
};

// Loader owns all buffers. Views expire when that asset store is destroyed.
// Color/index are roles of one image ID. Empty spans mean unavailable data.
struct AssetView {
    AssetId id;
    std::int32_t width{};
    std::int32_t height{};
    std::uint32_t row_stride{}; // Pixels per row in rgba and palette_indices.
    std::span<const AtlasRect> frames;
    std::span<const RgbaPixel> rgba;
    std::span<const std::uint8_t> palette_indices;
    std::span<const MaskView> masks; // Frame order when supplied.
};

} // namespace moorhuhn::contracts
