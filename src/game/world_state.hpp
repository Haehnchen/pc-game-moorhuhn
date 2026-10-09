#pragma once

#include "contracts/game.hpp"

#include <array>
#include <cstdint>

namespace moorhuhn::game {

// Hit selection accepts only codes below 20.
enum class MotionState : std::int32_t {
    arc_left = 0,
    arc_right = 1,
    fly_right = 2,
    fly_left = 3,
    close = 4,
    peek = 5,
    balloon_right = 6,
    balloon_left = 7,
    leaf_near = 8,
    leaf_far = 9,
    wing = 10,
    hat = 11,
    plane = 12,
    banner = 13,
    dropped_banner = 14,
    scare = 15,

    // Hit chickens fall in the same size and direction order as their flying states.
    fall_large_arc_left = 20,
    fall_large_arc_right = 21,
    fall_large_left = 22,
    fall_large_right = 23,
    fall_medium_arc_left = 24,
    fall_medium_arc_right = 25,
    fall_medium_left = 26,
    fall_medium_right = 27,
    fall_small_arc_left = 28,
    fall_small_arc_right = 29,
    fall_small_left = 30,
    fall_small_right = 31,
    close_hit = 32,
    peek_hit = 33,
    balloon_hit = 34,
    leaf_near_hit = 35,
    leaf_far_hit = 36,

    windmill = 50,
    wing_hit = 51,
    hat_swap_hit = 53,
    plane_hit = 54,
    banner_hit = 55,
    tiny = 56
};

enum class WorldAsset : std::uint8_t {
    chickl,
    chickl2,
    chickl3,
    chickr,
    chickr2,
    chickr3,
    chickd,
    chickd2,
    chickd3,
    big1,
    bighit,
    guck,
    balloon,
    balloon2,
    leaf,
    leafhit,
    scare,
    hat,
    windmill,
    wing,
    plane,
    sign,
    chickie
};

struct WorldObject {
    contracts::ObjectId id{};
    bool active{}; // Participates in drawing, motion and selection.
    WorldAsset asset{WorldAsset::chickl};
    MotionState state{MotionState::arc_left};
    std::int32_t layer{};

    // World coordinates persist; screen coordinates include camera motion and shake.
    float world_x{};
    float world_y{};
    float screen_x{};
    float screen_y{};

    float phase{}; // Plane links use linked_slot instead.
    float auxiliary{}; // Close-state phase scaled by 0.002.
    std::int32_t stage{}; // Truncated close-state branch selector.

    std::int32_t frame{};
    std::int32_t frame_count{19};
    float period_ms{62.5F};
    float animation_ms{};

    std::int32_t score{};
    bool ping_forward{true};
    std::int32_t linked_slot{-1};

    bool operator==(const WorldObject&) const = default;
};

struct WorldSnapshot {
    std::array<WorldObject, 32> objects{};
    std::int32_t camera{640};
    std::int32_t shake{};
    std::int32_t pool_limit{20};
    bool plane_present{};

    std::int32_t shield_flag{}; // Reset each round; the visible variant may survive.
    std::int32_t shield_variant{1}; // Visible shield strip, copied to persistent state after hits.

    bool operator==(const WorldSnapshot&) const = default;
};

} // namespace moorhuhn::game
