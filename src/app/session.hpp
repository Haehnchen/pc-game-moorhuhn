#pragma once

#include "game/specials.hpp"
#include "ui/hud.hpp"

namespace moorhuhn::app {

// SDL-free composition of the accepted gameplay modules.
class Session final : private game::RoundHooks {
public:
    Session(std::span<const contracts::AssetView>, bool focused, std::uint32_t seed);
    [[nodiscard]] contracts::UpdateOutput update(const contracts::InputFrame&);
    [[nodiscard]] contracts::UpdateOutput start(contracts::TimePoint, std::uint32_t seed);

    [[nodiscard]] game::Snapshot snapshot() const {
        return simulation_.snapshot();
    }

    [[nodiscard]] contracts::DrawList draw() const;

    [[nodiscard]] std::uint32_t menu_random() {
        return simulation_.draw_between_rounds();
    }

    [[nodiscard]] const ui::Shells& shells() const noexcept {
        return shells_;
    }

private:
    void initialize(game::RoundContext&) override;
    void camera_and_spawn(game::RoundContext&) override;
    void combat(game::RoundContext&) override;
    void timer(game::RoundContext&) override;
    void update_world(game::RoundContext&) override;
    void select_target(game::RoundContext&) override;
    void compose(const game::RoundModel&);
    std::vector<contracts::AssetView> assets_;
    game::IndexedWorldAssets indexed_;
    game::Simulation simulation_;
    game::SpecialsHandler specials_;
    ui::Shells shells_;
    ui::TimerSounds timer_;
    contracts::DrawList playfield_;
};

} // namespace moorhuhn::app
