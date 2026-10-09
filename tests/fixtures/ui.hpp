#pragma once
#include "ui/menu.hpp"

#include <iostream>
#include <stdexcept>
#include <string_view>

namespace test {
inline unsigned checks{};

inline void require(bool condition, std::string_view label) {
    ++checks;
    if (!condition) {
        throw std::runtime_error(std::string(label));
    }
}

struct Assets {
    std::array<moorhuhn::contracts::AtlasRect, 1> full{{{0, 0, 640, 480}}};
    std::array<moorhuhn::contracts::AtlasRect, 14> frames;
    std::array<moorhuhn::contracts::AtlasRect, 1> hole_frame{{{0, 0, 24, 24}}};
    std::array<moorhuhn::contracts::AtlasRect, 14> guck_frames;
    std::vector<std::uint8_t> main = std::vector<std::uint8_t>(640 * 480, 39);
    std::vector<std::uint8_t> score = std::vector<std::uint8_t>(640 * 480, 40);
    std::vector<std::uint8_t> credits = std::vector<std::uint8_t>(640 * 480, 41);
    std::vector<std::uint8_t> mask = std::vector<std::uint8_t>(448 * 32, 0);
    std::vector<std::uint8_t> hole = std::vector<std::uint8_t>(24 * 24, 42);
    std::vector<std::uint8_t> guck = std::vector<std::uint8_t>(48 * 14 * 72, 43);
    std::array<std::array<std::uint8_t, 4>, 256> palette{};
    std::array<std::uint8_t, 256> clut{};
    std::array<std::uint32_t, 600> table1{}, table2{};

    Assets() {
        for (std::size_t i = 0; i < 256; ++i) {
            palette[i] = {static_cast<std::uint8_t>(i), 0, 0, 7};
            clut[i] = static_cast<std::uint8_t>(255 - i);
        }
        for (std::size_t i = 0; i < 300; ++i) {
            table1[i] = table2[i] = static_cast<std::uint32_t>((i % 20) * 32);
            table1[300 + i] = table2[300 + i] = static_cast<std::uint32_t>((i / 20) * 32);
        }
        for (std::size_t i = 0; i < 14; ++i) {
            frames[i] = {static_cast<int>(i) * 32, 0, 32, 32};
        }
        for (std::size_t i = 0; i < 14; ++i) {
            guck_frames[i] = {static_cast<int>(i) * 48, 0, 48, 72};
        }
        // Sparse early phases, complete final phase.
        for (int y = 0; y < 32; ++y) {
            for (int x = 0; x < 32; ++x) {
                mask[static_cast<std::size_t>(y * 448 + 13 * 32 + x)] = 50;
            }
        }
    }

    moorhuhn::ui::MenuAssets view() const {
        using moorhuhn::contracts::AssetView;
        return {AssetView{{"main"}, 640, 480, 640, full, {}, main, {}}, AssetView{{"score"}, 640, 480, 640, full, {}, score, {}},
            AssetView{{"credits"}, 640, 480, 640, full, {}, credits, {}}, AssetView{{"mask"}, 448, 32, 448, frames, {}, mask, {}}, palette, clut, table1, table2,
            AssetView{{"hole"}, 24, 24, 24, hole_frame, {}, hole, {}}, AssetView{{"guck"}, 672, 72, 672, guck_frames, {}, guck, {}}};
    }
};

inline moorhuhn::contracts::InputFrame input(std::vector<moorhuhn::contracts::TimedAction> actions = {}) {
    return {1, {0}, {0}, {}, std::move(actions), false, true};
}

inline moorhuhn::contracts::TimedAction key(
    moorhuhn::contracts::InputControl c, moorhuhn::contracts::InputEdge edge = moorhuhn::contracts::InputEdge::pressed, std::string text = {}) {
    return {{0}, 1, c, edge, {}, std::move(text)};
}

inline bool contains(std::span<const moorhuhn::ui::Command> commands, moorhuhn::ui::CommandKind kind) {
    for (const auto& command : commands) {
        if (command.kind == kind) {
            return true;
        }
    }
    return false;
}
} // namespace test
