#pragma once

#include "game/simulation.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace moorhuhn::game {

// Borrowed records in any input order. World code owns IDs and positions.
// Game sprites use full frames, zero anchors and separate directional artwork.
struct HitCandidate {
    std::uint32_t slot{};
    contracts::ObjectId id{};
    std::string kind;
    std::int32_t state{};
    bool active{};
    contracts::FrameId frame;
    const contracts::AssetView* image{};
    float screen_x{};
    float screen_y{};
    contracts::ScreenPoint anchor{};
    bool mirror_x{};
    bool mirror_y{};
};

// Indexed playfield before ammo, HUD and cursor, including score popups.
struct IndexedScene {
    std::span<const std::uint8_t> pixels;
    std::uint32_t row_stride{640};
};

struct HitResult {
    std::uint32_t slot{};
    contracts::ObjectId id{};
    std::string kind;
    std::int32_t state{};
    contracts::FrameId frame;
    contracts::ScreenPoint point; // Current cursor+(16,16); target comes from old cache.

    bool operator==(const HitResult&) const = default;
};

struct IndexedHit {
    HitResult target;
    contracts::ScreenPoint frame_pixel; // Frame coordinates after mirroring.
    std::uint8_t candidate_index{};
    std::uint8_t scene_index{};

    bool operator==(const IndexedHit&) const = default;
};

// Selection truncates projected screen positions. Apply these writes to world records.
struct QuantizedPosition {
    std::uint32_t slot{};
    contracts::ObjectId id{};
    float screen_x{};
    float screen_y{};

    bool operator==(const QuantizedPosition&) const = default;
};

struct SelectionResult {
    contracts::ScreenPoint point;
    std::optional<IndexedHit> hit;
    std::vector<QuantizedPosition> positions;

    bool operator==(const SelectionResult&) const = default;
};

enum class CombatAction : std::uint8_t {
    none,
    shot,
    empty,
    reloaded
};

struct CombatResult {
    CombatAction action{CombatAction::none};
    std::int32_t cached_slot{-1}; // Preserve even when no current identity exists.
    std::optional<HitResult> hit; // No active/state/pixel revalidation of the old slot.
    contracts::ScreenPoint point;
    std::uint32_t shake{}; // Loaded shot sets 8; others 0.
    std::optional<std::uint32_t> ejected_shell;

    bool operator==(const CombatResult&) const = default;
};

// Runs after combat.shot and before shell consumption.
// May update score and lifecycle, never the magazine.
class LoadedShotHandler {
public:
    virtual ~LoadedShotHandler() = default;
    virtual void loaded_shot(RoundContext&, const CombatResult&) = 0;
};

// Seed the scratch once with zeroes; reset only the round's magazine.
void initialize_combat(RoundContext&);
[[nodiscard]] CombatResult process_combat(RoundContext&, std::span<const HitCandidate>, LoadedShotHandler* handler = nullptr);

[[nodiscard]] contracts::ScreenPoint selector_hotspot(contracts::ScreenPoint cursor);
// Updates persistent scratch/cache. Slots 0..31 define priority, regardless of painter order.
[[nodiscard]] SelectionResult select_indexed_target(PersistentState&, contracts::ScreenPoint cursor, IndexedScene, std::span<const HitCandidate>);

} // namespace moorhuhn::game
