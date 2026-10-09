#include "ui/hud.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <stdexcept>

namespace moorhuhn::ui {
namespace {
void glyph(std::vector<contracts::DrawCommand>& draws, unsigned char c, int x, int y) {
    contracts::SpriteDraw draw;
    draw.frame = {{"bigfont"}, c == ':' ? 123U : static_cast<std::uint32_t>(c) + 77};
    draw.destination = {x, y};
    draw.transparent_index = 18;
    draws.emplace_back(std::move(draw));
}

std::string formatted(int value, int width, bool zero) {
    char text[32];
    std::snprintf(text, sizeof(text), zero ? "%0*i" : "%*i", width, value);

    return text;
}
} // namespace

Shells::Shells() {
    reset();
}

void Shells::reset() {
    for (std::size_t i = 0; i < shells_.size(); ++i) {
        shells_[i] = {0, 8, 0.0F, 370.0F + 30.0F * static_cast<float>(i), 400.0F};
    }
}

void Shells::shot(std::uint32_t slot) {
    if (slot >= shells_.size()) {
        throw std::out_of_range("Shell slot outside 0..7");
    }

    shells_[slot].state = 1;
}

void Shells::reload() {
    reset();
}

void Shells::advance_loop() {
    for (std::size_t i = 0; i < shells_.size(); ++i) {
        auto& shell = shells_[i];

        if (shell.state != 1) {
            continue;
        }

        shell.frame = (shell.frame + 1) % 20;
        // Original sine uses the promoted product; cosine first stores binary32.
        const double angle = static_cast<double>(shell.phase) * 57.29579162597656;
        const float cos_angle = shell.phase * 57.29579162597656F;
        shell.x = static_cast<float>(560.0 + 30.0 * static_cast<double>(i) + std::sin(angle) * 200.0);
        shell.y = static_cast<float>(400.0 - std::cos(static_cast<double>(cos_angle)) * 100.0);

        if (shell.x > 640.0F) {
            shell.state = 2;
        }

        shell.phase = shell.phase + 0.001F;
    }
}

contracts::DrawList draw_hud(const HudModel& model, const Shells& shells) {
    std::vector<contracts::DrawCommand> draws;

    for (const auto& shell : shells.snapshot()) {
        if (shell.state == 2) {
            continue;
        }

        contracts::SpriteDraw draw;
        draw.frame = {{"huls"}, shell.frame};
        draw.destination = {static_cast<int>(shell.x), static_cast<int>(shell.y)};
        draw.transparent_index = 18;
        draws.emplace_back(std::move(draw));
    }

    const auto score = formatted(model.score, 4, false);

    for (int i = 0; i < 4; ++i) {
        glyph(draws, static_cast<unsigned char>(score[static_cast<std::size_t>(i)]), 544 + 24 * i, 8);
    }

    if (model.remaining_ms >= 10000 || (model.elapsed_ms & 511) < 256) {
        const auto remaining = model.remaining_ms;
        const auto minutes = formatted(remaining / 60000, 2, true);
        const auto seconds = formatted((remaining % 60000) / 1000, 2, true);
        glyph(draws, static_cast<unsigned char>(minutes[0]), 0, 8);
        glyph(draws, static_cast<unsigned char>(minutes[1]), 24, 8);
        glyph(draws, ':', 48, 8);
        glyph(draws, static_cast<unsigned char>(seconds[0]), 72, 8);
        glyph(draws, static_cast<unsigned char>(seconds[1]), 96, 8);
    }

    return contracts::DrawList(std::move(draws));
}

std::optional<std::string> TimerSounds::update(std::int32_t remaining_ms) {
    const auto minutes = remaining_ms / 60000;
    const auto seconds = (remaining_ms % 60000) / 1000;

    if (minutes >= 1 || seconds >= 10) {
        return std::nullopt;
    }

    if (seconds == previous_) {
        return std::nullopt;
    }

    previous_ = seconds;

    if (seconds == 0) {
        return "over22";
    }

    return "timeup22";
}
} // namespace moorhuhn::ui
