#pragma once
#include "../../../fixtures/commands.hpp"

namespace moorhuhn::tests {
inline CommandSequence simulation_commands() {
    using namespace contracts;
    using namespace game;
    CommandSequence result;
    result.initial.cached_target_slot = 7;
    result.initial.selector_state = {};
    result.initial.shield_variant = 1;
    result.initial.cursor = {320, 240};
    result.initial.held = {false, false, false, false, false, false, false, false, false, false, false};
    result.initial.focused = true;
    result.initial.next_round_id = 3;
    result.initial.next_object_id = 100;
    result.initial.next_event_sequence = 40;
    result.initial.last_input_sequence = 9;
    result.initial.last_input_end = {0};
    result.commands.emplace_back(RoundStart{{0}, 100U, 255U});
    result.commands.emplace_back(RecordedUpdate{
        {16000}, 116U, {10, {0}, {16000}, ScreenPoint{320, 240}, {{{16000}, 1, static_cast<InputControl>(0), static_cast<InputEdge>(0), ScreenPoint{320, 240}, ""}}, false, true}});
    result.commands.emplace_back(RecordedUpdate{{750000}, 850U,
        {11, {16000}, {750000}, ScreenPoint{320, 240},
            {{{750000}, 1, static_cast<InputControl>(0), static_cast<InputEdge>(1), ScreenPoint{320, 240}, ""},
                {{750000}, 2, static_cast<InputControl>(1), static_cast<InputEdge>(0), ScreenPoint{320, 240}, ""}},
            false, true}});
    result.commands.emplace_back(RecordedUpdate{{751000}, 851U,
        {12, {750000}, {751000}, ScreenPoint{320, 240}, {{{751000}, 1, static_cast<InputControl>(1), static_cast<InputEdge>(1), ScreenPoint{320, 240}, ""}}, false, true}});
    result.commands.emplace_back(RecordedUpdate{{800000}, 900U,
        {13, {751000}, {800000}, ScreenPoint{320, 240},
            {{{800000}, 1, static_cast<InputControl>(0), static_cast<InputEdge>(0), ScreenPoint{320, 240}, ""},
                {{800000}, 2, static_cast<InputControl>(1), static_cast<InputEdge>(0), ScreenPoint{320, 240}, ""}},
            false, true}});
    result.commands.emplace_back(RecordedUpdate{{801000}, 901U,
        {14, {800000}, {801000}, ScreenPoint{320, 240},
            {{{801000}, 1, static_cast<InputControl>(0), static_cast<InputEdge>(1), ScreenPoint{320, 240}, ""},
                {{801000}, 2, static_cast<InputControl>(1), static_cast<InputEdge>(1), ScreenPoint{320, 240}, ""},
                {{801000}, 3, static_cast<InputControl>(4), static_cast<InputEdge>(0), std::nullopt, ""}},
            false, true}});
    result.commands.emplace_back(RecordedUpdate{{1500000}, 1600U,
        {15, {801000}, {1500000}, ScreenPoint{320, 240},
            {{{1500000}, 1, static_cast<InputControl>(8), static_cast<InputEdge>(1), std::nullopt, ""},
                {{1500000}, 2, static_cast<InputControl>(8), static_cast<InputEdge>(0), std::nullopt, ""}},
            false, true}});
    result.commands.emplace_back(RecordedUpdate{{89999000}, 90099U, {16, {1500000}, {89999000}, ScreenPoint{320, 240}, {}, false, true}});
    result.commands.emplace_back(RecordedUpdate{{90000000}, 90100U,
        {17, {89999000}, {90000000}, ScreenPoint{320, 240}, {{{90000000}, 1, static_cast<InputControl>(0), static_cast<InputEdge>(0), ScreenPoint{320, 240}, ""}}, false, true}});
    result.commands.emplace_back(RecordedUpdate{{90001000}, 90101U,
        {18, {90000000}, {90001000}, ScreenPoint{320, 240}, {{{90001000}, 1, static_cast<InputControl>(0), static_cast<InputEdge>(1), ScreenPoint{320, 240}, ""}}, false, true}});
    result.commands.emplace_back(RoundStart{{90002000}, 4294967295U, 511U});
    result.commands.emplace_back(RecordedUpdate{{90003000}, 0U, {19, {90001000}, {90003000}, ScreenPoint{320, 240}, {}, false, true}});
    return result;
}
} // namespace moorhuhn::tests
