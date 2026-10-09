#pragma once

#include "game/combat.hpp"
#include "game/specials_state.hpp"
#include "game/world.hpp"

namespace moorhuhn::game {

// Reset only round-owned pools and ledger. Shield artwork stays session-owned.
void initialize_specials(RoundContext&);
// Run after advance_world, once per recorded simulation update.
void advance_specials(RoundContext&);

class SpecialsHandler final : public LoadedShotHandler {
public:
    void loaded_shot(RoundContext&, const CombatResult&) override;
    // Explicit identity ignores repeat delivery of the same shot.
    void loaded_shot(RoundContext&, const CombatResult&, ShotId);
};

// Owned commands; keep this value alive while its borrowed extras are consumed.
struct SpecialsDrawData {
    std::array<std::vector<contracts::DrawCommand>, 6> after_objects;
    std::vector<contracts::DrawCommand> tree_holes;
    [[nodiscard]] WorldDrawExtras extras() const noexcept;
};

[[nodiscard]] SpecialsDrawData specials_draw_data(const WorldSnapshot&, const SpecialsState&);

} // namespace moorhuhn::game
