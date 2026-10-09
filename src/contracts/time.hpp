#pragma once

#include <compare>
#include <cstdint>

namespace moorhuhn::contracts {

// Signed microseconds from one session epoch shared by input and simulation.
// Transport time is separate from the simulation's raw clock samples.
struct TimePoint {
    std::int64_t microseconds{};

    auto operator<=>(const TimePoint&) const = default;
};

struct Duration {
    std::int64_t microseconds{};

    auto operator<=>(const Duration&) const = default;
};

// Integer logical pixels. Atlas, screen, and world positions are distinct types.
struct ScreenPoint {
    std::int32_t x{};
    std::int32_t y{};

    auto operator<=>(const ScreenPoint&) const = default;
};

struct WorldPoint {
    std::int32_t x{};
    std::int32_t y{};

    auto operator<=>(const WorldPoint&) const = default;
};

struct AtlasRect {
    std::int32_t x{};
    std::int32_t y{};
    std::int32_t width{};
    std::int32_t height{};

    auto operator<=>(const AtlasRect&) const = default;
};

inline constexpr std::int32_t logical_width = 640;
inline constexpr std::int32_t logical_height = 480;

} // namespace moorhuhn::contracts
