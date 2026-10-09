#pragma once

#include "contracts/time.hpp"

#include <cstdint>
#include <stdexcept>

namespace moorhuhn::app {

// Carry fractional frames so repeated small steps match one long step.
class AudioFrameClock final {
public:
    [[nodiscard]] std::uint64_t advance(contracts::TimePoint now) {
        if (now.microseconds < previous_.microseconds) {
            throw std::invalid_argument("Audio clock must not move backward");
        }

        const auto elapsed = static_cast<std::uint64_t>(now.microseconds - previous_.microseconds);
        previous_ = now;
        const auto partial = (elapsed % 1'000'000) * 48'000 + remainder_;
        const auto frames = (elapsed / 1'000'000) * 48'000 + partial / 1'000'000;
        remainder_ = partial % 1'000'000;

        return frames;
    }

private:
    contracts::TimePoint previous_{};
    std::uint64_t remainder_{};
};

} // namespace moorhuhn::app
