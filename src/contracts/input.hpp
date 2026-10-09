#pragma once

#include "contracts/time.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace moorhuhn::contracts {

// Controls carry edges. Their game meaning belongs to the receiving module.
enum class InputControl : std::uint8_t {
    primary,
    secondary,
    accept,
    cancel,
    left,
    right,
    up,
    down,
    focus, // pressed = gained, released = lost; preserves order within one poll.
    space,
    backspace,
    text
};

enum class InputEdge : std::uint8_t {
    pressed,
    released
};

struct TimedAction {
    TimePoint at;
    std::uint64_t order{}; // Strictly increasing within one input frame.
    InputControl control{};
    InputEdge edge{};
    // Null outside the playfield or when no pointer position is available.
    std::optional<ScreenPoint> pointer;
    std::string text{}; // Owned UTF-8 for text/pressed only; UI chooses accepted characters.

    auto operator<=>(const TimedAction&) const = default;
};

struct InputFrame {
    std::uint64_t sequence{};
    TimePoint begin;
    TimePoint end;
    std::optional<ScreenPoint> pointer;
    // Owned actions in arrival order. Times are nondecreasing in [begin, end].
    // Equal timestamps retain order; modules must not sort by control type.
    std::vector<TimedAction> actions;
    bool quit_requested{}; // True in the poll frame that receives a quit notice.
    bool focused{true}; // Latest window focus; gameplay handling is not defined here.

    bool operator==(const InputFrame&) const = default;
};

} // namespace moorhuhn::contracts
