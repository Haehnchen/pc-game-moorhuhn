#pragma once

#include "contracts/assets.hpp"
#include "contracts/input.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace moorhuhn::contracts {

using ObjectId = std::uint64_t; // Zero is invalid; IDs remain stable in a round.

// Random state is explicit; the algorithm defines the word layout.
struct RandomState {
    std::string algorithm;
    std::vector<std::uint64_t> words;

    bool operator==(const RandomState&) const = default;
};

struct RoundState {
    std::uint64_t id{};
    std::string phase; // Defined by the owning simulation/UI module.
    TimePoint started_at;

    bool operator==(const RoundState&) const = default;
};

struct MagazineState {
    std::uint32_t loaded{};
    std::uint32_t capacity{};
    bool reloading{};

    bool operator==(const MagazineState&) const = default;
};

// Owned object projection for renderers and external consumers.
struct ObjectState {
    ObjectId id{};
    std::string kind;
    WorldPoint position;
    FrameId frame;
    Duration animation_time;

    bool operator==(const ObjectState&) const = default;
};

struct GameState {
    RoundState round;
    TimePoint now;
    Duration elapsed; // Most recent simulation update duration.
    MagazineState magazine;
    std::int64_t score{};
    WorldPoint camera;
    std::vector<ObjectState> objects; // Simulation-owned stable order.
    RandomState random;

    bool operator==(const GameState&) const = default;
};

// Caller passes elapsed time, input, and random state for every update.
// random must reference state.random; retaining context references is forbidden.
struct UpdateContext {
    TimePoint now;
    Duration elapsed;
    const InputFrame& input;
    RandomState& random;
};

} // namespace moorhuhn::contracts
