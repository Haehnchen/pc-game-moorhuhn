#include "ui/hud.hpp"

#include "../fixtures/ui.hpp"

#include <bit>
#include <cmath>

int main() {
    try {
        using namespace moorhuhn::ui;
        using namespace moorhuhn::contracts;
        Shells shells;
        const auto before = shells.snapshot();
        for (const auto& shell : before) {
            test::require(shell.state == 0 && shell.frame == 8, "Original loaded shell uses frame8");
        }
        auto draw = draw_hud({12345, 89999, 1}, shells);
        test::require(draw.commands().size() == 17, "Eight shells, four score, five timer glyphs");
        test::require(shells.snapshot() == before, "HUD read does not mutate shells");
        for (std::size_t i = 0; i < 8; ++i) {
            test::require(std::get<SpriteDraw>(draw.commands()[i]).frame.index == 8, "Loaded HUD draws huls frame8");
        }
        const std::array<std::uint32_t, 9> frames{126, 127, 128, 129, 125, 126, 123, 127, 134};
        const std::array<int, 9> xs{544, 568, 592, 616, 0, 24, 48, 72, 96};
        for (std::size_t i = 0; i < 9; ++i) {
            const auto& sprite = std::get<SpriteDraw>(draw.commands()[8 + i]);
            test::require(sprite.frame.image.value == "bigfont" && sprite.frame.index == frames[i] && sprite.destination == ScreenPoint{xs[i], 8} && sprite.transparent_index == 18,
                "Score and time digit frames and positions");
        }
        test::require(draw_hud({0, 10000, 256}, shells).commands().size() == 17, "10000ms does not blink");
        test::require(draw_hud({0, 9999, 255}, shells).commands().size() == 17, "Timer visible blink half");
        test::require(draw_hud({0, 9999, 256}, shells).commands().size() == 12, "Timer hidden blink half");
        test::require(draw_hud({0, 0, 512}, shells).commands().size() == 17, "Zero timer reappears");
        shells.shot(0);
        shells.advance_loop();
        const auto& shell = shells.snapshot()[0];
        test::require(shell.frame == 9 && std::bit_cast<std::uint32_t>(shell.phase) == 0x3a83126f, "First ejection advances frame8 to9 with binary32 phase step");
        test::require(shell.x == 560.0F && shell.y == 300.0F, "First ejection uses old phase before increment");
        shells.advance_loop();
        test::require(
            std::abs(shells.snapshot()[0].x - 571.4529F) < 0.001F && std::abs(shells.snapshot()[0].y - 300.1641F) < 0.001F, "Second shell ejection reference coordinates");
        const auto negative = draw_hud({0, -30000, 512}, shells);
        test::require(
            std::get<SpriteDraw>(negative.commands()[negative.commands().size() - 2]).frame.index == 122 && std::get<SpriteDraw>(negative.commands().back()).frame.index == 128,
            "Long-gap signed terminal timer preserves first-two formatting");
        shells.shot(7);
        shells.advance_loop();
        test::require(shells.snapshot()[7].state == 2, "Shell outside right edge hidden");
        shells.reload();
        test::require(shells.snapshot() == before, "Reload restores eight shell records");
        for (const auto& loaded : shells.snapshot()) {
            test::require(loaded.frame == 8, "Reload restores loaded frame8");
        }
        TimerSounds timer;
        test::require(!timer.update(90000), "No normal-time sound");
        test::require(timer.update(9999) == "timeup22", "Nine-second sound");
        test::require(!timer.update(9000), "Same displayed second is consumed once");
        test::require(timer.update(8000) == "timeup22", "Next second sound");
        test::require(timer.update(0) == "over22" && !timer.update(-1), "Zero sound once");
        timer.reset();
        test::require(!timer.update(4000), "Initial displayed four matches original resident-loop cache");
        test::require(timer.update(3000) == "timeup22" && !timer.update(3000), "Changed displayed second sounds once");
        test::require(!timer.update(61000) && !timer.update(3000), "Minute gate leaves the previous-second cache intact");
        test::require(!timer.update(10000) && !timer.update(3000), "Ten-second gate leaves the cache intact");

        struct Gap {
            std::int32_t delta, remaining;
            const char* sound;
            std::array<std::uint32_t, 2> frames;
        };

        const std::array<Gap, 3> gaps{{{90999, -999, "over22", {125, 125}}, {91000, -1000, "timeup22", {122, 126}}, {120000, -30000, "timeup22", {122, 128}}}};
        for (const auto& gap : gaps) {
            timer.reset();
            test::require(90000 - gap.delta == gap.remaining, "Static gap vector budget");
            test::require(timer.update(gap.remaining) == gap.sound && !timer.update(gap.remaining), "Signed displayed gap second sounds once");
            const auto terminal = draw_hud({0, gap.remaining, 512}, shells);
            const auto count = terminal.commands().size();
            test::require(std::get<SpriteDraw>(terminal.commands()[count - 2]).frame.index == gap.frames[0]
                              && std::get<SpriteDraw>(terminal.commands()[count - 1]).frame.index == gap.frames[1],
                "Terminal HUD keeps first-two signed second glyphs");
            const auto blink = draw_hud({0, gap.remaining, static_cast<std::uint32_t>(gap.delta)}, shells);
            test::require(blink.commands().size() == ((gap.delta & 511) < 256 ? 17U : 12U), "Terminal HUD blink uses raw elapsed time");
        }
        timer.reset();
        test::require(timer.update(-61000) == "timeup22", "Negative minutes admit the signed remainder second");
        std::cout << "PASS HUD " << test::checks << " checks\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
