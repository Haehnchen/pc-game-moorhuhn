#include "ui/menu.hpp"

#include "../fixtures/ui.hpp"

#include <algorithm>

namespace {
using namespace moorhuhn::ui;
using namespace moorhuhn::contracts;

unsigned count(std::span<const Command> commands, CommandKind kind, std::string_view sound = {}) {
    return static_cast<unsigned>(std::count_if(commands.begin(), commands.end(), [&](const auto& command) {
        return command.kind == kind && (sound.empty() || command.sound == sound);
    }));
}

std::vector<Command> until(Menu& menu, Stage target, std::uint32_t dt = 16) {
    std::vector<Command> commands;
    for (unsigned i = 0; i < 1000 && menu.stage() != target; ++i) {
        auto next = menu.advance(test::input(), dt, i * dt);
        commands.insert(commands.end(), next.begin(), next.end());
    }
    test::require(menu.stage() == target, "Reach documented next UI stage");
    return commands;
}
} // namespace

int main() {
    try {
        test::Assets assets;
        unsigned draws = 0;
        Menu menu(assets.view(), {}, [&] {
            ++draws;
            return 50U;
        });
        // Own the remapped pixels; palette byte3 cannot become RGBA alpha.
        test::require(menu.image("ui.credits").rgba[0] == RgbaPixel{41, 0, 0, 255}, "Credits use main palette with opaque alpha");
        assets.credits[0] = 200;
        assets.palette[41] = {0, 255, 0, 0};
        test::require(menu.image("ui.credits").rgba[0] == RgbaPixel{41, 0, 0, 255}, "Menu owns immutable copies beyond input lifetime");
        for (unsigned i = 0; i < 169; ++i) {
            (void)menu.advance(test::input(), 16, i * 16);
        }
        test::require(menu.stage() == Stage::title_in, "Transition incomplete at call169");
        (void)menu.advance(test::input(), 16, 2704);
        test::require(menu.stage() == Stage::title, "Transition completes on exact170th call");
        test::require(std::all_of(menu.image("ui.surface").palette_indices.begin(), menu.image("ui.surface").palette_indices.end(),
                          [](auto i) {
                              return i == 39;
                          }),
            "Phase13 writes every tile; phase14 cannot corrupt pixels");
        unsigned title_shots = 0;
        for (unsigned i = 0; i < 33; ++i) {
            title_shots += count(menu.advance(test::input(), 16, i * 16), CommandKind::play_sound, "shoot22");
        }
        test::require(title_shots == 1, "First title shot is after zero elapsed threshold when entrance completes");
        (void)menu.advance(test::input({test::key(InputControl::primary)}), 16, 600);
        test::require(menu.stage() == Stage::title, "Title pointer clicks have no invented action");
        unsigned intro = 0;
        for (unsigned i = 0; i < 150; ++i) {
            const auto commands = menu.advance(test::input(), 16, 700 + i * 16);
            title_shots += count(commands, CommandKind::play_sound, "shoot22");
            intro += count(commands, CommandKind::play_sound, "intro22");
        }
        test::require(title_shots == 8 && intro == 1, "Eight holes and explicit intro loop once");
        const auto title = menu.draw();
        test::require(title.commands().size() == 12, "Main, two overlays, eight holes and guck");
        const auto& hole = std::get<SpriteDraw>(title.commands()[3]);
        test::require(hole.destination == ScreenPoint{257, 123}, "Original fixed hole coordinate");
        const auto old_draws = draws;
        (void)menu.draw();
        (void)menu.draw();
        test::require(draws == old_draws, "Draw reads consume no random values");
        for (unsigned i = 0; i < 100; ++i) {
            (void)menu.advance(test::input(), 16, 3100 + i * 16);
        }
        test::require(draws > 0, "Title uses shared random state after entrance");
        (void)menu.advance(test::input({test::key(InputControl::space), test::key(InputControl::text, InputEdge::pressed, "LEAK")}), 16, 4000);
        test::require(menu.stage() == Stage::title_exit && menu.player().empty(), "Stage boundary drains queued text");
        (void)menu.advance(test::input({test::key(InputControl::space, InputEdge::released)}), 16, 4016);
        const auto name_commands = until(menu, Stage::name_entry);
        test::require(count(name_commands, CommandKind::stop_sound, "intro22") == 1 && menu.text_input(), "Title exit stops loop and enables text only after name box opens");
        const auto view = menu.image("ui.surface");
        test::require(
            view.palette_indices[210 * 640 + 180] == 216 && view.palette_indices[210 * 640 + 179] == 39, "Name rectangle maps immutable source through CLUT in half-open bounds");
        test::require(view.palette_indices[115 * 640 + 230] == 43 && view.palette_indices[123 * 640 + 287] == 42,
            "Frozen guck and hole pixels survive outside original name restore rectangle");
        const auto name = menu.draw();
        const auto& prompt = std::get<SpriteDraw>(name.commands()[1]);
        test::require(
            prompt.frame == FrameId{{"font3"}, 37} && prompt.destination == ScreenPoint{256, 213} && prompt.transparent_index == 0, "Small-font prompt mapping and center");
        auto typing = menu.advance(test::input({test::key(InputControl::text, InputEdge::pressed, "AB\xc3\xa9 C\x7f"), test::key(InputControl::backspace)}), 16, 128);
        test::require(menu.player() == "AB C" && count(typing, CommandKind::play_sound, "typo22") == 7,
            "ASCII space/DEL accepted, UTF-8 character rejected once, backspace removes one source byte");
        (void)menu.advance(test::input({test::key(InputControl::text, InputEdge::pressed, "0123456789012345")}), 16, 256);
        test::require(menu.player() == "AB C012345", "Name capped at ten original bytes");
        auto confirm = menu.advance(test::input({test::key(InputControl::cancel), test::key(InputControl::text, InputEdge::pressed, "X")}), 16, 272);
        test::require(
            menu.stage() == Stage::name_close && !menu.text_input() && count(confirm, CommandKind::play_sound, "ready22") == 1, "Escape confirms and drains later actions");
        const auto starts = until(menu, Stage::gameplay);
        test::require(count(starts, CommandKind::start_round) == 1 && menu.player() == "AB C012345", "One start command after name close and transition out");
        const auto cancel = menu.advance(test::input({test::key(InputControl::cancel)}), 16, 4000);
        const auto cancel_again = menu.advance(test::input({test::key(InputControl::cancel)}), 16, 4016);
        test::require(count(cancel, CommandKind::end_round) == 1 && !test::contains(cancel_again, CommandKind::end_round), "End request exactly once");
        menu.round_finished(600);
        const auto scores = menu.scores();
        menu.round_finished(700);
        test::require(menu.scores() == scores && menu.scores().entries[0].score == 600, "Repeated end notification cannot insert again");
        const auto results = until(menu, Stage::highscores);
        test::require(count(results, CommandKind::play_sound, "intro22") == 1, "Highscores explicit intro loop once");
        unsigned score_shots = 0, falls = 0;
        for (unsigned i = 0; i < 140; ++i) {
            const auto commands = menu.advance(test::input(), 64, 256);
            score_shots += count(commands, CommandKind::play_sound, "shoot22");
            falls += count(commands, CommandKind::play_sound, "fall22");
        }
        test::require(score_shots == 3 && falls == 2, "Assembly-confirmed three beats and fall sounds at12/21");
        const auto rows = menu.draw();
        // Inserted row visible at elapsed256. Score overwrites name columns10/11.
        bool column_seen = false;
        for (const auto& command : rows.commands()) {
            if (const auto* sprite = std::get_if<SpriteDraw>(&command)) {
                if (sprite->frame.image.value == "bigfont" && sprite->destination == ScreenPoint{368, 120}) {
                    test::require(sprite->frame.index == 0, "Score first padding glyph overwrites ninth name byte");
                    column_seen = true;
                }
            }
        }
        test::require(column_seen, "Highscore row snaps to128 and uses sixteen fixed columns");
        (void)menu.advance(test::input({test::key(InputControl::space)}), 16, 9000);
        (void)until(menu, Stage::title_in);
        (void)menu.advance(test::input({test::key(InputControl::space, InputEdge::released)}), 16, 9016);
        (void)until(menu, Stage::title);
        for (unsigned i = 0; i < 33; ++i) {
            (void)menu.advance(test::input(), 16, i * 16);
        }
        (void)menu.advance(test::input({test::key(InputControl::space)}), 16, 9600);
        (void)menu.advance(test::input({test::key(InputControl::space, InputEdge::released)}), 16, 9616);
        (void)until(menu, Stage::name_entry);
        (void)menu.advance(test::input({test::key(InputControl::accept)}), 16, 9700);
        (void)until(menu, Stage::gameplay);
        menu.round_finished(999);
        (void)until(menu, Stage::highscores);
        for (unsigned i = 0; i < 28; ++i) {
            (void)menu.advance(test::input(), 0, 9800);
        }
        for (unsigned i = 0; i < 3; ++i) {
            (void)menu.advance(test::input(), 201, 9900);
        }
        (void)menu.advance(test::input(), 31, 9931);
        test::require(std::get<SpriteDraw>(menu.draw().commands()[3]).frame.index == 1, "Highfly retains31.5ms remainder across score visits");
        (void)menu.advance(test::input({test::key(InputControl::space)}), 16, 9950);
        (void)until(menu, Stage::title_in);
        (void)menu.advance(test::input({test::key(InputControl::space, InputEdge::released)}), 16, 9966);
        (void)until(menu, Stage::title);
        for (unsigned i = 0; i < 33; ++i) {
            (void)menu.advance(test::input(), 16, i * 16);
        }
        (void)menu.advance(test::input({test::key(InputControl::cancel)}), 16, 10000);
        (void)until(menu, Stage::title_out);
        test::require(std::get<SpriteDraw>(menu.draw().commands()[0]).frame.image.value == "ui.surface", "Title Escape fades frozen title out before credits in");
        (void)until(menu, Stage::credits);
        test::require(std::get<SpriteDraw>(menu.draw().commands()[0]).frame.image.value == "ui.credits", "Outer quit reaches remapped credits");
        (void)menu.advance(test::input(), 5000, 15000);
        test::require(menu.stage() == Stage::credits, "Credits wait is strictly greater than5000");
        (void)menu.advance(test::input(), 1, 15001);
        const auto exits = until(menu, Stage::finished);
        test::require(count(exits, CommandKind::save_scores) == 1 && count(exits, CommandKind::quit) == 1, "Save and quit once after credits out");
        test::require(menu.advance(test::input(), 1, 15002).empty(), "Finished reads/advances cannot repeat commands");
        const auto final_scores = menu.scores();
        menu.storage_error("write failed");
        test::require(menu.awaiting_save_ack() && !menu.draw().commands().empty(), "Failed final save blocks exit and shows acknowledgement prompt");
        test::require(menu.advance(test::input(), 5000, 20002).empty() && menu.awaiting_save_ack(), "Save error stays visible without timeout");
        test::require(menu.advance(test::input({test::key(InputControl::accept, InputEdge::released)}), 1, 20003).empty(), "Key release cannot acknowledge error");
        const auto acknowledged = menu.advance(test::input({test::key(InputControl::accept)}), 1, 20004);
        test::require(count(acknowledged, CommandKind::quit) == 1 && !menu.awaiting_save_ack(), "Fresh Enter acknowledges failed save once");
        test::require(menu.advance(test::input(), 1, 20005).empty() && menu.scores() == final_scores, "Acknowledgement preserves scores and cannot repeat exit");
        menu.storage_error("another failed save");
        test::require(count(menu.advance(test::input({test::key(InputControl::cancel)}), 1, 20006), CommandKind::quit) == 1, "Escape acknowledges failure");
        menu.storage_error("failed save");
        auto close = test::input();
        close.quit_requested = true;
        test::require(count(menu.advance(close, 1, 20007), CommandKind::quit) == 1, "Fresh window close acknowledges failure");
        menu.storage_error("old load error");
        menu.storage_error({});
        test::require(!menu.awaiting_save_ack(), "Successful save clears an earlier storage error");
        std::cout << "PASS menu " << test::checks << " checks\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
