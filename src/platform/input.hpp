#pragma once

#include "contracts/input.hpp"
#include "platform/clock.hpp"
#include "platform/display.hpp"

namespace moorhuhn::platform {

class InputAdapter {
public:
    InputAdapter(Display& display, const MonotonicClock& clock);
    [[nodiscard]] contracts::InputFrame poll();
    void set_text_input(bool enabled);
    void recenter(contracts::ScreenPoint point); // Align round-entry logical and window cursor state.

    [[nodiscard]] bool focused() const noexcept {
        return focused_;
    }

private:
    Display& display_;
    const MonotonicClock& clock_;
    contracts::TimePoint previous_end_{};
    std::uint64_t sequence_{};
    std::uint64_t action_order_{};
    bool focused_{};
    std::optional<Coordinate> window_pointer_;
};

} // namespace moorhuhn::platform
