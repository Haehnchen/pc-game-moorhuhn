#pragma once

#include "contracts/time.hpp"

#include <optional>

namespace moorhuhn::platform {

struct Coordinate {
    double x{};
    double y{};
};

struct Viewport {
    int window_width{};
    int window_height{};
    int pixel_width{};
    int pixel_height{};
    double x{};
    double y{};
    double width{};
    double height{};
    double scale{};

    [[nodiscard]] static Viewport fit(int window_width, int window_height, int pixel_width, int pixel_height);
    [[nodiscard]] bool drawable() const noexcept;
    // SDL mouse coordinates use window units; the viewport uses output pixels.
    [[nodiscard]] std::optional<contracts::ScreenPoint> window_to_logical(Coordinate point) const;
    [[nodiscard]] std::optional<contracts::ScreenPoint> output_to_logical(Coordinate point) const;
    // Map the center of a logical pixel back to window units.
    [[nodiscard]] std::optional<Coordinate> logical_to_window(contracts::ScreenPoint point) const;
};

} // namespace moorhuhn::platform
