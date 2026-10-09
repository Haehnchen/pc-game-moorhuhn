#pragma once

#include "contracts/time.hpp"

#include <cstdint>

namespace moorhuhn::platform {

class MonotonicClock {
public:
    MonotonicClock();
    [[nodiscard]] contracts::TimePoint now() const;
    [[nodiscard]] contracts::TimePoint from_sdl_timestamp(std::uint64_t nanoseconds) const;

private:
    std::uint64_t epoch_nanoseconds_{};
};

} // namespace moorhuhn::platform
