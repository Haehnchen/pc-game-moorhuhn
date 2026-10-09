#include "platform/clock.hpp"

#include <SDL3/SDL_timer.h>

#include <algorithm>
#include <limits>

namespace moorhuhn::platform {

MonotonicClock::MonotonicClock() : epoch_nanoseconds_(SDL_GetTicksNS()) {}

contracts::TimePoint MonotonicClock::now() const {
    return from_sdl_timestamp(SDL_GetTicksNS());
}

contracts::TimePoint MonotonicClock::from_sdl_timestamp(std::uint64_t nanoseconds) const {
    if (nanoseconds <= epoch_nanoseconds_) {
        return {};
    }

    const auto microseconds = (nanoseconds - epoch_nanoseconds_) / 1000;
    const auto maximum = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());

    return {static_cast<std::int64_t>(std::min(microseconds, maximum))};
}

} // namespace moorhuhn::platform
