#pragma once

#include "contracts/game.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace moorhuhn::game {

struct ShotId {
    std::uint64_t round{};
    std::uint64_t event{}; // Sequence of combat.shot.

    bool operator==(const ShotId&) const = default;
};

enum class ScoreCause : std::uint8_t {
    object,
    shield
};

struct ScoreTransaction {
    ShotId shot;
    ScoreCause cause{};
    contracts::TimePoint at;
    std::optional<contracts::ObjectId> object;
    std::int32_t slot{-1};
    std::int32_t rectangle{-1};

    // Requested points can differ from the applied delta when the score is clamped.
    std::int32_t requested{};
    std::int32_t delta{};
    std::int32_t total{};

    bool operator==(const ScoreTransaction&) const = default;
};

struct ScorePopup {
    bool active{};
    contracts::ObjectId id{};
    float world_x{};
    float world_y{};
    float screen_x{};
    float screen_y{};
    std::int32_t layer{};
    std::int32_t score{};
    float elapsed_ms{};

    bool operator==(const ScorePopup&) const = default;
};

struct TreeHole {
    bool active{};
    contracts::ObjectId id{};
    std::int32_t world_x{};
    std::int32_t world_y{};

    bool operator==(const TreeHole&) const = default;
};

struct SpecialsState {
    std::array<ScorePopup, 16> popups{};
    std::array<TreeHole, 16> holes{};
    std::uint32_t next_hole{};

    // Remember handled shots so duplicate events cannot award points twice.
    std::vector<ShotId> processed_shots;
    std::vector<ScoreTransaction> ledger;

    bool operator==(const SpecialsState&) const = default;
};

} // namespace moorhuhn::game
