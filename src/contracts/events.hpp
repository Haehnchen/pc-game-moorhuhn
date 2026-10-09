#pragma once

#include "contracts/game.hpp"

#include <optional>
#include <variant>
#include <vector>

namespace moorhuhn::contracts {

struct PlaySound {
    AssetId sound;
};

// Names identify presentation work emitted during simulation updates.
struct PresentationEffect {
    std::string name;
    std::optional<ObjectId> object;
    std::optional<ScreenPoint> position;
};

using EventPayload = std::variant<PlaySound, PresentationEffect>;

struct GameEvent {
    std::uint64_t sequence{}; // Nonzero, strictly increasing within the session.
    TimePoint at;
    EventPayload payload;
};

// Producer returns a fresh owned batch per update, including an empty batch.
// Consume in sequence order once. Redrawing state never recreates these events.
// Each consumer tracks its last sequence to avoid duplicate events.
struct UpdateOutput {
    std::vector<GameEvent> events;
};

using AdvanceSimulation = UpdateOutput (*)(GameState&, const UpdateContext&);

} // namespace moorhuhn::contracts
