#pragma once

#include "contracts/render.hpp"
#include "game/world_state.hpp"
#include "render/renderer.hpp"

#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace moorhuhn::game {

struct RoundContext;
struct HitCandidate;

struct ObjectSpec {
    std::string_view asset;
    std::int32_t width;
    std::int32_t height;
    std::int32_t frame_count;
    float period_ms;
};

[[nodiscard]] ObjectSpec object_spec(WorldAsset);
[[nodiscard]] contracts::FrameId object_frame(const WorldObject&);
[[nodiscard]] bool drawable(MotionState) noexcept;
[[nodiscard]] std::int32_t projected_integer(float);

void initialize_world(RoundContext&);
void camera_and_spawn_world(RoundContext&);
void advance_world(RoundContext&);
// -1 consumes a random type; 0..7 is an explicit type. Full pools draw nothing.
[[nodiscard]] bool spawn_world(RoundContext&, std::int32_t type = -1);

struct WorldHitChange {
    bool changed{};
    std::int32_t slot{-1};
    contracts::ObjectId object{};
    MotionState before{};
    MotionState after{}; // State of the selected identity after a possible slot swap.

    bool operator==(const WorldHitChange&) const = default;
};

// Lifecycle only. Specials apply score, popups and hit sounds.
[[nodiscard]] WorldHitChange apply_world_hit(WorldSnapshot&, std::int32_t slot);
// Apply the selector's screen-position truncation to the same object identity.
void quantize_world_position(WorldSnapshot&, std::int32_t slot, contracts::ObjectId);

// Popups follow their object layer; tree holes follow layer 5 before objects 0.
// HUD and cursor are added after hit selection.
struct WorldDrawExtras {
    std::array<std::span<const contracts::DrawCommand>, 6> after_objects{};
    std::span<const contracts::DrawCommand> tree_holes;
};

[[nodiscard]] contracts::DrawList world_draw_list(const WorldSnapshot&, const WorldDrawExtras& extras = {});
[[nodiscard]] std::vector<HitCandidate> world_hit_candidates(const WorldSnapshot&, std::span<const contracts::AssetView>);

// Own the immutable index projection once; it does not borrow loader buffers.
class IndexedWorldAssets {
public:
    explicit IndexedWorldAssets(std::span<const contracts::AssetView>);
    // The returned scene is owned by this object and is updated by the next call.
    [[nodiscard]] const std::vector<std::uint8_t>& compose(const contracts::DrawList&, std::optional<contracts::AtlasRect> region = {}) const;

private:
    struct Image {
        contracts::AssetId id;
        std::int32_t width{};
        std::int32_t height{};
        std::uint32_t stride{};
        std::vector<contracts::AtlasRect> frames;
        std::vector<contracts::RgbaPixel> rgba;
        std::vector<std::uint8_t> indices;
    };

    std::vector<Image> images_;
    mutable render::Renderer renderer_;
    mutable std::vector<std::uint8_t> indices_;
};

void validate_world_assets(std::span<const contracts::AssetView>);

} // namespace moorhuhn::game
