#pragma once

#include "contracts/game.hpp"

#include <optional>
#include <span>
#include <utility>
#include <variant>
#include <vector>

namespace moorhuhn::contracts {

enum class SpriteBlend : std::uint8_t {
    source_over,
    copy
};

// index_key skips the selected index; coverage scales alpha by the mask byte.
enum class MaskMode : std::uint8_t {
    index_key,
    coverage
};

// Explicit interpretation of an entry in AssetView::masks.
struct SpriteMask {
    std::uint32_t index{};
    MaskMode mode{MaskMode::index_key};
    std::uint8_t transparent_index{};
};

struct SpriteDraw {
    FrameId frame;
    ScreenPoint destination; // Top-left on the 640 x 480 logical screen.
    SpriteBlend blend{SpriteBlend::source_over};
    std::optional<std::uint8_t> transparent_index{};
    std::optional<SpriteMask> mask{};
    std::optional<AtlasRect> source_rect{}; // Relative to the frame; output keeps crop dimensions.
    bool mirror_x{}; // Mirror the selected source rectangle.
    bool mirror_y{};
};

struct FillDraw {
    ScreenPoint destination;
    std::int32_t width{};
    std::int32_t height{};
    RgbaPixel color; // Replace destination RGBA bytes inside the clipped rectangle.
};

using DrawCommand = std::variant<SpriteDraw, FillDraw>;

// Owned immutable commands. Vector order is painter order, first to last.
// Positions are screen coordinates after camera translation; SDL scales later.
class DrawList {
public:
    explicit DrawList(std::vector<DrawCommand> commands = {}) : commands_(std::move(commands)) {}

    [[nodiscard]] std::span<const DrawCommand> commands() const noexcept {
        return commands_;
    }

private:
    std::vector<DrawCommand> commands_;
};

// State and asset storage are read-only. Rendering cannot advance simulation.
using BuildDrawList = DrawList (*)(const GameState&, std::span<const AssetView>);

} // namespace moorhuhn::contracts
