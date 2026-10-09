#pragma once

#include "contracts/assets.hpp"
#include "contracts/input.hpp"
#include "contracts/render.hpp"
#include "storage/highscores.hpp"

#include <array>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace moorhuhn::ui {
enum class Stage {
    title_in,
    title,
    title_exit,
    title_out,
    name_open,
    name_entry,
    name_close,
    round_out,
    gameplay,
    score_in,
    highscores,
    credits_in,
    credits,
    credits_out,
    finished
};

enum class CommandKind {
    start_round,
    end_round,
    save_scores,
    quit,
    play_sound,
    stop_sound
};

struct Command {
    CommandKind kind;
    std::string sound;
    float volume{1.0F};
    std::int32_t repeats{}; // -1 means explicit loop.
};

// Borrowed only by construction; Menu copies every required byte.
struct MenuAssets {
    contracts::AssetView main;
    contracts::AssetView score;
    contracts::AssetView credits;
    contracts::AssetView mask;

    std::span<const std::array<std::uint8_t, 4>> main_palette;
    std::span<const std::uint8_t> clut_trans;
    std::span<const std::uint32_t> tabel1;
    std::span<const std::uint32_t> tabel2;

    // Frozen title pixels survive outside the name restore rectangle.
    contracts::AssetView hole;
    contracts::AssetView guck;
};

class Menu final {
public:
    // The title consumes the session generator before round reseeding.
    explicit Menu(const MenuAssets&, storage::HighScores = {}, std::function<std::uint32_t()> random = {});
    ~Menu();
    Menu(Menu&&) noexcept;
    Menu& operator=(Menu&&) noexcept;
    Menu(const Menu&) = delete;
    Menu& operator=(const Menu&) = delete;

    // Call once per game loop. Render and snapshot reads have no side effects.
    [[nodiscard]] std::vector<Command> advance(const contracts::InputFrame&, std::uint32_t delta_ms, std::uint32_t elapsed_ms);
    // Only accepted in gameplay. Repeated notification has no effect.
    void round_finished(std::int32_t score);
    [[nodiscard]] Stage stage() const;

    [[nodiscard]] bool gameplay() const {
        return stage() == Stage::gameplay;
    }

    [[nodiscard]] bool text_input() const {
        return stage() == Stage::name_entry;
    }

    [[nodiscard]] const std::string& player() const;
    [[nodiscard]] const storage::HighScores& scores() const;

    [[nodiscard]] contracts::DrawList draw() const;
    // ui.surface and ui.credits own RGBA, frames, and palette indices.
    // Borrowed views expire on next advance/round_finished or Menu destruction.
    [[nodiscard]] contracts::AssetView image(std::string_view id) const;

    void storage_error(std::string message);
    [[nodiscard]] bool awaiting_save_ack() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace moorhuhn::ui
