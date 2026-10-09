#pragma once

#include "game/simulation.hpp"

#include <iomanip>
#include <sstream>
#include <type_traits>
#include <variant>

namespace moorhuhn::tests {

using Command = std::variant<game::RoundStart, game::RecordedUpdate>;

struct CommandSequence {
    game::PersistentState initial;
    std::vector<Command> commands;
};

inline contracts::UpdateOutput apply(game::Simulation& simulation, const Command& command, game::RoundHooks& hooks) {
    return std::visit(
        [&](const auto& value) {
            if constexpr (std::is_same_v<std::decay_t<decltype(value)>, game::RoundStart>) {
                return simulation.start_round(value, hooks);
            } else {
                return simulation.update(value, hooks);
            }
        },
        command);
}

inline std::string event_value(const contracts::GameEvent& event) {
    std::ostringstream value;
    value << event.sequence << ' ' << event.at.microseconds << ' ';
    std::visit(
        [&](const auto& payload) {
            if constexpr (std::is_same_v<std::decay_t<decltype(payload)>, contracts::PlaySound>) {
                value << "sound " << std::quoted(payload.sound.value);
            } else {
                value << "effect " << std::quoted(payload.name) << ' ' << payload.object.has_value();
                if (payload.object) {
                    value << ' ' << *payload.object;
                }
                value << ' ' << payload.position.has_value();
                if (payload.position) {
                    value << ' ' << payload.position->x << ' ' << payload.position->y;
                }
            }
        },
        event.payload);
    return value.str();
}

} // namespace moorhuhn::tests
