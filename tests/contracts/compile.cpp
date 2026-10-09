#include "contracts/contracts.hpp"

#include <array>
#include <concepts>
#include <type_traits>

using namespace moorhuhn::contracts;

static_assert(__cplusplus > 202002L);
static_assert(!std::same_as<WorldPoint, ScreenPoint>);
static_assert(std::same_as<decltype(AssetView{}.rgba)::element_type, const RgbaPixel>);
static_assert(std::same_as<decltype(DrawList{}.commands())::element_type, const DrawCommand>);
static_assert(std::is_invocable_r_v<DrawList, BuildDrawList, const GameState&, std::span<const AssetView>>);

int main() {
    const std::array pixels{RgbaPixel{10, 20, 30, 255}};
    const std::array rectangles{AtlasRect{0, 0, 1, 1}};
    const AssetView image{AssetId{"synthetic"}, 1, 1, 1, rectangles, pixels, {}, {}};
    const DrawList draws{{SpriteDraw{FrameId{AssetId{"synthetic"}, 0}, {12, 34}}, FillDraw{{0, 0}, 1, 1, pixels[0]}}};
    GameState state;
    InputFrame input;
    const UpdateContext context{{100}, {100}, input, state.random};
    const UpdateOutput effects{{GameEvent{1, context.now, PlaySound{AssetId{"synthetic_sound"}}}}};
    return image.frames[0].width == 1 && draws.commands().size() == 2 && effects.events[0].sequence == 1 && &context.random == &state.random ? 0 : 1;
}
