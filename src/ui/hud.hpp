#pragma once

#include "contracts/render.hpp"

#include <array>
#include <optional>
#include <string>

namespace moorhuhn::ui {
struct HudModel {
    std::int32_t score{};
    std::int32_t remaining_ms{90000};
    std::uint32_t elapsed_ms{};
};

struct Shell {
    std::uint8_t state{}; // 0 loaded, 1 ejecting, 2 hidden.
    std::uint32_t frame{};
    float phase{};
    float x{};
    float y{400.0F};

    bool operator==(const Shell&) const = default;
};

class Shells final {
public:
    Shells();
    void reset();
    void shot(std::uint32_t slot);
    void reload();
    void advance_loop(); // One step per simulation loop, independent of elapsed time.

    [[nodiscard]] const std::array<Shell, 8>& snapshot() const {
        return shells_;
    }

private:
    std::array<Shell, 8> shells_;
};

[[nodiscard]] contracts::DrawList draw_hud(const HudModel&, const Shells&);

class TimerSounds final {
public:
    // The first displayed digit is already cached.
    void reset() {
        previous_ = 4;
    }

    [[nodiscard]] std::optional<std::string> update(std::int32_t remaining_ms);

private:
    std::int32_t previous_{4};
};
} // namespace moorhuhn::ui
