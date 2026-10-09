#include "ui/menu.hpp"

#include <algorithm>
#include <bit>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <utility>

namespace moorhuhn::ui {
namespace {
using namespace contracts;
constexpr std::size_t pixels = 640 * 480;
constexpr std::array<int, 8> hole_x{257, 250, 287, 321, 274, 290, 269, 220};
constexpr std::array<int, 8> hole_y{123, 102, 109, 122, 129, 142, 160, 135};

struct Image {
    std::string id;
    std::vector<std::uint8_t> indices;
    std::vector<RgbaPixel> rgba;
    std::array<AtlasRect, 1> frames{{{0, 0, 640, 480}}};

    [[nodiscard]] AssetView view() const {
        return {{id}, 640, 480, 640, frames, rgba, indices, {}};
    }
};

struct IndexedSprite {
    std::uint32_t stride{};
    std::vector<AtlasRect> frames;
    std::vector<std::uint8_t> indices;

    IndexedSprite(const AssetView& source, int width, int height, std::size_t minimum_frames)
        : stride(source.row_stride), frames(source.frames.begin(), source.frames.end()), indices(source.palette_indices.begin(), source.palette_indices.end()) {
        if (source.width <= 0 || source.height <= 0 || stride < static_cast<unsigned>(source.width)
            || indices.size() < static_cast<std::size_t>(stride) * static_cast<unsigned>(source.height) || frames.size() < minimum_frames) {
            throw std::invalid_argument("Missing indexed title art for retained name background");
        }

        for (const auto& frame : frames) {
            if (frame.width != width || frame.height != height || frame.x < 0 || frame.y < 0 || frame.x > source.width - width || frame.y > source.height - height) {
                throw std::invalid_argument("Title art frame outside indexed atlas");
            }
        }
    }

    void paint(std::vector<std::uint8_t>& destination, std::uint32_t index, int x0, int y0) const {
        const auto frame = frames.at(index);

        for (int y = 0; y < frame.height; ++y) {
            for (int x = 0; x < frame.width; ++x) {
                if (x0 + x < 0 || x0 + x >= 640 || y0 + y < 0 || y0 + y >= 480) {
                    continue;
                }

                const auto value = indices[static_cast<std::size_t>(frame.y + y) * stride + static_cast<unsigned>(frame.x + x)];

                if (value != 18) {
                    destination[static_cast<std::size_t>((y0 + y) * 640 + x0 + x)] = value;
                }
            }
        }
    }
};

std::vector<std::uint8_t> copy_image(const AssetView& source) {
    if (source.frames.empty() || source.frames[0].width != 640 || source.frames[0].height != 480 || source.row_stride < static_cast<unsigned>(source.width) || source.width <= 0
        || source.height <= 0 || source.palette_indices.size() < static_cast<std::size_t>(source.row_stride) * static_cast<unsigned>(source.height)) {
        throw std::invalid_argument("Menu image requires a validated 640x480 indexed frame");
    }

    std::vector<std::uint8_t> result(pixels);
    const auto frame = source.frames[0];

    if (frame.x < 0 || frame.y < 0 || frame.x > source.width - 640 || frame.y > source.height - 480) {
        throw std::invalid_argument("Menu image frame outside atlas");
    }

    for (int y = 0; y < 480; ++y) {
        std::copy_n(source.palette_indices.begin() + static_cast<std::ptrdiff_t>(static_cast<std::size_t>(frame.y + y) * source.row_stride + static_cast<unsigned>(frame.x)), 640,
            result.begin() + y * 640);
    }

    return result;
}

void sprite(std::vector<DrawCommand>& out, std::string id, std::uint32_t frame, int x, int y, std::optional<std::uint8_t> key = 18) {
    SpriteDraw draw;
    draw.frame = {{std::move(id)}, frame};
    draw.destination = {x, y};
    draw.transparent_index = key;

    if (!key) {
        draw.blend = SpriteBlend::copy;
    }

    out.emplace_back(std::move(draw));
}

void small_text(std::vector<DrawCommand>& out, std::string_view text, int x, int y) {
    if (x == -1) {
        x = (80 - static_cast<int>(text.size())) * 4;
    }

    const int origin = x;

    for (unsigned char c : text) {
        if (c == '\n') {
            y += 5;
            x = origin;
            continue;
        }

        if (c >= 32 && c <= 154 && c != ' ') {
            sprite(out, "font3", c - 32, x, y, 0);
        }

        x += 8;
    }
}

void big_text(std::vector<DrawCommand>& out, std::string_view text, int x, int y) {
    if (x == -1) {
        x = (640 - static_cast<int>(text.size()) * 24) / 2;
    }

    for (unsigned char c : text) {
        sprite(out, "bigfont", c - 32, x, y);
        x += 24;
    }
}
} // namespace

struct Menu::Impl {
    // Current screen, saved scores and the shared random source.
    Stage stage{Stage::title_in};
    storage::HighScores scores;
    std::function<std::uint32_t()> random;

    // Owned artwork and the two surfaces rebuilt by menu animations.
    std::array<RgbaPixel, 256> palette{};
    std::array<std::uint8_t, 256> clut{};
    std::array<std::uint32_t, 600> table1{};
    std::array<std::uint32_t, 600> table2{};
    std::vector<std::uint8_t> main;
    std::vector<std::uint8_t> score;
    std::vector<std::uint8_t> mask;
    IndexedSprite hole;
    IndexedSprite guck;
    Image surface{"ui.surface", std::vector<std::uint8_t>(pixels), std::vector<RgbaPixel>(pixels)};
    Image credits{"ui.credits", {}, std::vector<RgbaPixel>(pixels)};

    // Each transition channel animates one tile at a time.
    std::array<int, 32> phases{};
    std::array<int, 32> xs{};
    std::array<int, 32> ys{};
    int counter{};
    bool transition_out{};
    bool space_held{};
    bool end_requested{};
    bool quit_after_title{};

    std::uint64_t stage_elapsed{};
    std::uint64_t title_beats_elapsed{};
    std::uint64_t high_beats_elapsed{};
    std::uint32_t elapsed{};

    // Title entrance, bullet holes and the peeking chicken.
    int title_top{-75};
    int title_bottom{485};
    int holes{};
    int guck_state{};
    int guck_frame{};
    float guck_elapsed{};
    bool intro{};
    bool title_ready{};

    // Expanding rectangle around the name-entry screen.
    float left{320};
    float top{240};
    float right{321};
    float bottom{241};

    // High-score entrance and the highlighted row.
    int heading_y{-80};
    int target_y{485};
    int highfly_y{};
    int highfly_frame{};
    int high_stage{};
    int beats{};
    int inserted{-1};
    float highfly_elapsed{};
    std::array<int, 5> row_x{640, 680, 720, 760, 800};
    std::array<int, 5> row_state{1, 1, 1, 1, 1};
    std::array<std::uint64_t, 5> row_hold{};

    std::string error;

    explicit Impl(const MenuAssets& assets, storage::HighScores initial, std::function<std::uint32_t()> generator)
        : scores(std::move(initial)), random(std::move(generator)), main(copy_image(assets.main)), score(copy_image(assets.score)), hole(assets.hole, 24, 24, 1),
          guck(assets.guck, 48, 72, 12) {
        storage::validate(scores);

        if (assets.main_palette.size() != 256 || assets.clut_trans.size() != 256 || assets.tabel1.size() != 600 || assets.tabel2.size() != 600) {
            throw std::invalid_argument("Menu needs 256 palette/CLUT entries and two 600-entry tables");
        }

        if (!random) {
            throw std::invalid_argument("Menu needs an explicit random source");
        }

        for (std::size_t i = 0; i < 256; ++i) {
            const auto& color = assets.main_palette[i];
            palette[i] = {color[0], color[1], color[2], 255};
            clut[i] = assets.clut_trans[i];
        }

        std::copy(assets.tabel1.begin(), assets.tabel1.end(), table1.begin());
        std::copy(assets.tabel2.begin(), assets.tabel2.end(), table2.begin());

        for (const auto* table : {&table1, &table2}) {
            std::array<bool, 300> tiles{};

            for (std::size_t i = 0; i < 300; ++i) {
                const auto x = std::bit_cast<std::int32_t>((*table)[i]);
                const auto y = std::bit_cast<std::int32_t>((*table)[300 + i]);

                if (x < 0 || x > 608 || y < 0 || y > 448 || x % 32 || y % 32 || tiles[static_cast<std::size_t>((y / 32) * 20 + x / 32)]) {
                    throw std::invalid_argument("Menu transition table is not a complete tile permutation");
                }

                tiles[static_cast<std::size_t>((y / 32) * 20 + x / 32)] = true;
            }

            // Channel zero starts at (0,0), not table[0].
            if ((*table)[0] != 0 || (*table)[300] != 0) {
                throw std::invalid_argument("Transition first tile must be (0,0)");
            }
        }

        if (assets.mask.frames.size() != 14 || assets.mask.row_stride < static_cast<unsigned>(assets.mask.width) || assets.mask.width <= 0 || assets.mask.height <= 0
            || assets.mask.palette_indices.size() < static_cast<std::size_t>(assets.mask.row_stride) * static_cast<unsigned>(assets.mask.height)) {
            throw std::invalid_argument("Menu transition mask needs 14 indexed 32x32 frames");
        }

        mask.resize(14 * 1024);

        for (std::size_t f = 0; f < 14; ++f) {
            const auto rect = assets.mask.frames[f];

            if (rect.width != 32 || rect.height != 32 || rect.x < 0 || rect.y < 0 || rect.x > assets.mask.width - 32 || rect.y > assets.mask.height - 32) {
                throw std::invalid_argument("Menu transition mask frame outside atlas");
            }

            for (int y = 0; y < 32; ++y) {
                std::copy_n(assets.mask.palette_indices.begin()
                                + static_cast<std::ptrdiff_t>(static_cast<std::size_t>(rect.y + y) * assets.mask.row_stride + static_cast<unsigned>(rect.x)),
                    32, mask.begin() + static_cast<std::ptrdiff_t>(f * 1024 + y * 32));
            }
        }

        if (!std::all_of(mask.begin() + 13 * 1024, mask.end(), [](auto index) {
                return index == 50;
            })) {
            throw std::invalid_argument("Transition phase 13 must cover its complete tile");
        }

        credits.indices = copy_image(assets.credits);
        colorize(credits);
        transition(Stage::title_in, false);
    }

    void colorize(Image& image) {
        for (std::size_t i = 0; i < pixels; ++i) {
            image.rgba[i] = palette[image.indices[i]];
        }
    }

    void enter(Stage next) {
        stage = next;
        stage_elapsed = 0;
    }

    void transition(Stage next, bool out) {
        enter(next);
        transition_out = out;
        counter = 0;
        xs.fill(0);
        ys.fill(0);

        for (std::size_t i = 0; i < 32; ++i) {
            phases[i] = -2 * static_cast<int>(i);
        }

        if (!out) {
            std::fill(surface.indices.begin(), surface.indices.end(), 0);
        }

        colorize(surface);
    }

    const std::vector<std::uint8_t>& transition_source() const {
        if (stage == Stage::score_in) {
            return score;
        }

        if (stage == Stage::credits_in || stage == Stage::credits_out) {
            return credits.indices;
        }

        return main;
    }

    bool transition_loop() {
        const auto& source = transition_source();
        const auto& table = stage == Stage::score_in ? table2 : table1;

        if (space_held) {
            if (transition_out) {
                std::fill(surface.indices.begin(), surface.indices.end(), 0);
            } else {
                surface.indices = source;
            }

            colorize(surface);

            return true;
        }

        for (std::size_t i = 0; i < 32; ++i) {
            if (phases[i] < 14) {
                ++phases[i];
            }

            if (counter < 300 && (phases[i] == 0 || (phases[i] == 14 && counter != 299))) {
                ++counter;

                if (counter >= 300) {
                    throw std::logic_error("Transition table scheduler overflow");
                }

                phases[i] = 0;
                xs[i] = static_cast<int>(table[static_cast<std::size_t>(counter)]);
                ys[i] = static_cast<int>(table[static_cast<std::size_t>(counter) + 300]);
            }
        }

        for (std::size_t i = 0; i < 32; ++i) {
            const int phase = phases[i];

            if (phase < 0 || phase >= 14) {
                continue; // Terminal phase writes no pixels.
            }

            for (int y = 0; y < 32; ++y) {
                for (int x = 0; x < 32; ++x) {
                    if (mask[static_cast<std::size_t>(phase * 1024 + y * 32 + x)] != 50) {
                        continue;
                    }

                    const auto target = static_cast<std::size_t>((ys[i] + y) * 640 + xs[i] + x);
                    surface.indices[target] = transition_out ? 0 : source[target];
                }
            }
        }

        colorize(surface);

        return std::all_of(phases.begin(), phases.end(), [](int value) {
            return value == 14;
        });
    }

    void title_init() {
        enter(Stage::title);
        title_top = -75;
        title_bottom = 485;
        holes = 0;
        guck_state = 0;
        guck_frame = 0;
        guck_elapsed = 0;
        intro = false;
        title_ready = false;
        title_beats_elapsed = 0;
    }

    void title_image() {
        surface.indices = main;

        for (int i = 0; i < holes; ++i) {
            hole.paint(surface.indices, 0, hole_x[static_cast<std::size_t>(i)], hole_y[static_cast<std::size_t>(i)]);
        }

        if (intro || guck_state != 0) {
            guck.paint(surface.indices, static_cast<std::uint32_t>(guck_frame), 230, 115);
        }
    }

    void name_image() {
        title_image();
        const int x0 = std::clamp(static_cast<int>(left), 0, 640), x1 = std::clamp(static_cast<int>(right), 0, 640);
        const int y0 = std::clamp(static_cast<int>(top), 0, 480), y1 = std::clamp(static_cast<int>(bottom), 0, 480);

        for (int y = y0; y < y1; ++y) {
            for (int x = x0; x < x1; ++x) {
                const auto i = static_cast<std::size_t>(y * 640 + x);
                surface.indices[i] = clut[main[i]];
            }
        }

        colorize(surface);
    }

    void high_init() {
        enter(Stage::highscores);
        heading_y = -80;
        target_y = 485;
        highfly_y = 0;
        highfly_frame = 0;
        // Restart the animation but keep the shared time remainder.
        high_beats_elapsed = 0;
        high_stage = 0;
        beats = 0;
        row_x = {640, 680, 720, 760, 800};
        row_state = {1, 1, 1, 1, 1};
        row_hold.fill(0);
        intro = true;
    }

    void sound(std::vector<Command>& commands, std::string id, bool loop = false) {
        commands.push_back({CommandKind::play_sound, std::move(id), 1.0F, loop ? -1 : 0});
    }

    void stop_intro(std::vector<Command>& commands) {
        if (intro) {
            commands.push_back({CommandKind::stop_sound, "intro22"});
        }

        intro = false;
    }

    void credits_start(std::vector<Command>& commands) {
        stop_intro(commands);

        if (stage == Stage::gameplay && !end_requested) {
            commands.push_back({CommandKind::end_round, {}});
            end_requested = true;
        }

        transition(Stage::credits_in, false);
    }

    void title_effects(std::uint32_t dt, std::vector<Command>& commands) {
        if (!intro) {
            title_beats_elapsed += dt;
        }

        if (holes < 8 && title_beats_elapsed > static_cast<std::uint64_t>(holes) * 200) {
            ++holes;
            sound(commands, "shoot22");
        }

        if (!intro && title_beats_elapsed > 2000) {
            intro = true;
            guck_state = 1;
            sound(commands, "intro22", true);

            return;
        }

        if (guck_state == 1) {
            guck_elapsed += static_cast<float>(dt);

            if (guck_elapsed >= 100) {
                guck_elapsed -= 100;
                ++guck_frame;

                if (guck_frame > 7) {
                    guck_state = 2;
                }
            }
        } else if (guck_state == 2) {
            const auto value = random() & 127;

            if (value == 50 || value == 100) {
                guck_frame = value == 50 ? 8 : 10;
                guck_state = value == 50 ? 3 : 4;
                guck_elapsed = 0;
            }
        } else if (guck_state == 3 || guck_state == 4) {
            guck_elapsed += static_cast<float>(dt);

            if (guck_elapsed >= 400) {
                guck_elapsed -= 400;
                ++guck_frame;

                if (guck_frame > (guck_state == 3 ? 9 : 11)) {
                    guck_state = 2;
                    guck_frame = 8;
                }
            }
        }
    }

    void title_loop(std::uint32_t dt, std::vector<Command>& commands) {
        title_ready = title_bottom <= 360;

        if (title_top < 10) {
            title_top += 4;
        }

        if (title_bottom > 360) {
            title_bottom -= 4;
        }

        if (title_ready) {
            title_effects(dt, commands);
        }
    }

    void high_loop(std::uint32_t dt, std::vector<Command>& commands) {
        if (high_stage == 0) {
            if (heading_y < 10) {
                heading_y += 4;
            }

            if (target_y > 380) {
                target_y -= 4;
            } else {
                high_stage = 1;
            }
        } else if (high_stage == 1) {
            high_beats_elapsed += dt;

            if (beats < 3 && high_beats_elapsed > static_cast<std::uint64_t>(beats + 1) * 200) {
                ++beats;
                sound(commands, "shoot22");

                if (beats == 3) {
                    high_stage = 2;
                }
            }
        } else if (high_stage == 2) {
            highfly_elapsed += static_cast<float>(dt);

            if (highfly_elapsed >= 62.5F) {
                highfly_elapsed -= 62.5F;
                ++highfly_frame;

                if (highfly_frame == 12 || highfly_frame == 21) {
                    sound(commands, "fall22");
                }

                if (highfly_frame >= 21) {
                    high_stage = 3;
                }
            }
        } else if (high_stage == 4) {
            if (heading_y > -80) {
                heading_y -= 4;
            }

            if (target_y < 485) {
                target_y += 4;
            }

            highfly_y += 4;

            if (highfly_y > 280) {
                stop_intro(commands);
                transition(Stage::title_in, false);

                return;
            }
        }

        for (std::size_t i = 0; i < 5; ++i) {
            if (row_state[i] == 1) {
                row_x[i] -= 4;

                if (row_x[i] <= 128) {
                    row_x[i] = 128;
                    row_state[i] = 2;
                    row_hold[i] = 0;
                }
            } else if (row_state[i] == 2) {
                row_hold[i] += dt;

                if (row_hold[i] >= 5000) {
                    row_state[i] = 3;
                }
            } else if (row_state[i] == 3) {
                row_x[i] -= 4;

                if (row_x[i] < -399) {
                    row_state[i] = 0;
                }
            }
        }

        if (row_state[4] == 0) {
            high_stage = 4;
        }
    }
};

Menu::Menu(const MenuAssets& assets, storage::HighScores scores, std::function<std::uint32_t()> random)
    : impl_(std::make_unique<Impl>(assets, std::move(scores), std::move(random))) {}

Menu::~Menu() = default;
Menu::Menu(Menu&&) noexcept = default;
Menu& Menu::operator=(Menu&&) noexcept = default;

Stage Menu::stage() const {
    return impl_->stage;
}

const std::string& Menu::player() const {
    return impl_->scores.player;
}

const storage::HighScores& Menu::scores() const {
    return impl_->scores;
}

AssetView Menu::image(std::string_view id) const {
    if (id == "ui.surface") {
        return impl_->surface.view();
    }

    if (id == "ui.credits") {
        return impl_->credits.view();
    }

    throw std::out_of_range("Unknown owned UI image");
}

void Menu::storage_error(std::string message) {
    impl_->error = std::move(message);
}

void Menu::round_finished(std::int32_t score) {
    auto& m = *impl_;

    if (m.stage != Stage::gameplay) {
        return;
    }

    m.inserted = storage::insert(m.scores, m.scores.player, score);
    m.transition(Stage::score_in, false);
}

bool Menu::awaiting_save_ack() const {
    return impl_->stage == Stage::finished && !impl_->error.empty();
}

std::vector<Command> Menu::advance(const InputFrame& input, std::uint32_t dt, std::uint32_t elapsed) {
    auto& m = *impl_;
    std::vector<Command> commands;
    m.elapsed = elapsed;
    m.stage_elapsed += dt;

    for (const auto& action : input.actions) {
        if (action.control == InputControl::space) {
            m.space_held = action.edge == InputEdge::pressed;
        }
    }

    if (!input.focused) {
        m.space_held = false;
    }

    if (input.quit_requested && m.stage != Stage::credits_in && m.stage != Stage::credits && m.stage != Stage::credits_out && m.stage != Stage::finished) {
        m.credits_start(commands);

        return commands;
    }

    const auto stage = m.stage; // Queue drain: never route old actions to a new stage.

    switch (stage) {
        case Stage::title_in:
            if (m.transition_loop()) {
                m.title_init();
            }

            break;
        case Stage::title:
            m.title_loop(dt, commands);

            if (m.title_ready) {
                bool cancel = false;

                for (const auto& action : input.actions) {
                    if (action.edge == InputEdge::pressed && action.control == InputControl::cancel) {
                        cancel = true;
                    }
                }

                if (m.space_held || cancel) {
                    m.quit_after_title = cancel;
                    m.enter(Stage::title_exit);
                }
            }

            break;
        case Stage::title_exit:
            m.title_effects(dt, commands);

            if (m.title_top > -75) {
                m.title_top -= 4;
            }

            if (m.title_bottom < 485) {
                m.title_bottom += 4;
            } else {
                if (m.quit_after_title) {
                    m.title_image();
                    m.transition(Stage::title_out, true);
                } else {
                    m.stop_intro(commands);
                    m.enter(Stage::name_open);
                    m.left = 320;
                    m.top = 240;
                    m.right = 321;
                    m.bottom = 241;
                    m.name_image();
                }
            }

            break;
        case Stage::title_out:
            if (m.transition_loop()) {
                m.credits_start(commands);
            }

            break;
        case Stage::name_open: {
            const float f = static_cast<float>(dt) * 0.001F * 60.0F;
            m.left -= 4 * f;
            m.right += 4 * f;
            m.top -= f;
            m.bottom += f;

            if (m.left <= 181) {
                m.left = 180;
                m.top = 210;
                m.right = 460;
                m.bottom = 270;
                m.enter(Stage::name_entry);
            }

            m.name_image();
            break;
        }

        case Stage::name_entry:
            for (const auto& action : input.actions) {
                if (action.edge != InputEdge::pressed) {
                    continue;
                }

                if (action.control == InputControl::accept || action.control == InputControl::cancel) {
                    m.sound(commands, "ready22");
                    m.enter(Stage::name_close);
                    break;
                }

                if (action.control == InputControl::backspace) {
                    if (!m.scores.player.empty()) {
                        m.scores.player.pop_back();
                    }

                    m.sound(commands, "typo22");
                } else if (action.control == InputControl::text) {
                    // Accept one-byte ASCII only; never truncate a multibyte character.
                    for (std::size_t i = 0; i < action.text.size();) {
                        const auto c = static_cast<unsigned char>(action.text[i]);

                        if (c >= 32 && c <= 127 && m.scores.player.size() < 10) {
                            m.scores.player.push_back(static_cast<char>(c));
                        }

                        std::size_t width = 1;
                        const std::size_t expected = c >= 0xc2 && c <= 0xdf ? 2 : c >= 0xe0 && c <= 0xef ? 3 : c >= 0xf0 && c <= 0xf4 ? 4 : 1;

                        while (width < expected && i + width < action.text.size() && (static_cast<unsigned char>(action.text[i + width]) & 0xc0) == 0x80) {
                            ++width;
                        }

                        i += width;
                        m.sound(commands, "typo22");
                    }
                } else if (action.control == InputControl::left || action.control == InputControl::right || action.control == InputControl::up
                           || action.control == InputControl::down) {
                    m.sound(commands, "typo22");
                }
            }

            break;
        case Stage::name_close: {
            const float f = static_cast<float>(dt) * 0.001F * 60.0F;
            m.left += 4 * f;
            m.right -= 4 * f;
            m.top += f;
            m.bottom -= f;
            m.name_image();

            if (m.right - m.left < 0 || m.bottom - m.top < 0) {
                m.transition(Stage::round_out, true);
            }

            break;
        }

        case Stage::round_out:
            if (m.transition_loop()) {
                m.enter(Stage::gameplay);
                m.end_requested = false;
                commands.push_back({CommandKind::start_round, {}});
            }

            break;
        case Stage::gameplay:
            for (const auto& action : input.actions) {
                if (action.control == InputControl::cancel && action.edge == InputEdge::pressed && !m.end_requested) {
                    commands.push_back({CommandKind::end_round, {}});
                    m.end_requested = true;
                }
            }

            break;
        case Stage::score_in:
            if (m.transition_loop()) {
                m.high_init();
                m.sound(commands, "intro22", true);
            }

            break;
        case Stage::highscores:
            if (m.space_held) {
                m.high_stage = 4;
            }

            m.high_loop(dt, commands);
            break;
        case Stage::credits_in:
            if (m.transition_loop()) {
                m.enter(Stage::credits);
            }

            break;
        case Stage::credits:
            if (m.stage_elapsed > 5000 || m.space_held) {
                m.transition(Stage::credits_out, true);
            }

            break;
        case Stage::credits_out:
            if (m.transition_loop()) {
                m.enter(Stage::finished);
                commands.push_back({CommandKind::save_scores, {}});
                commands.push_back({CommandKind::quit, {}});
            }

            break;
        case Stage::finished:
            if (!m.error.empty()) {
                bool acknowledge = input.quit_requested;

                for (const auto& action : input.actions) {
                    if (action.edge == InputEdge::pressed && (action.control == InputControl::accept || action.control == InputControl::cancel)) {
                        acknowledge = true;
                    }
                }

                if (acknowledge) {
                    m.error.clear();
                    commands.push_back({CommandKind::quit, {}});
                }
            }

            break;
    }

    return commands;
}

DrawList Menu::draw() const {
    const auto& m = *impl_;
    std::vector<DrawCommand> out;

    switch (m.stage) {
        case Stage::title_in:
        case Stage::title_out:
        case Stage::round_out:
        case Stage::score_in:
        case Stage::credits_in:
        case Stage::credits_out:
            sprite(out, "ui.surface", 0, 0, 0, std::nullopt);
            break;
        case Stage::title:
        case Stage::title_exit:
            sprite(out, "main", 0, 0, 0, std::nullopt);
            sprite(out, "virtuell", 0, 160, m.title_top);
            sprite(out, "mschiess", 0, 140, m.title_bottom);

            for (int i = 0; i < m.holes; ++i) {
                sprite(out, "hole", 0, hole_x[static_cast<std::size_t>(i)], hole_y[static_cast<std::size_t>(i)]);
            }

            if (m.intro) {
                sprite(out, "guck", static_cast<std::uint32_t>(m.guck_frame), 230, 115);
            }

            break;
        case Stage::name_open:
        case Stage::name_entry:
        case Stage::name_close:
            sprite(out, "ui.surface", 0, 0, 0, std::nullopt);

            if (m.stage == Stage::name_entry) {
                small_text(out, "ENTER YOUR NAME:", -1, 213);
                big_text(out, m.scores.player + ((m.elapsed & 511) < 256 ? "_" : " "), -1, 227);
            }

            break;
        case Stage::highscores: {
            sprite(out, "score", 0, 0, 0, std::nullopt);
            sprite(out, "highscor", 0, 120, m.heading_y);
            sprite(out, "target", 0, 308, m.target_y);
            sprite(out, "highfly", static_cast<std::uint32_t>(m.highfly_frame), 340, m.highfly_y);

            for (std::size_t row = 0; row < 5; ++row) {
                if (m.row_state[row] == 0 || (static_cast<int>(row) == m.inserted && (m.elapsed & 511) < 256)) {
                    continue;
                }

                std::string text(16, ' ');
                text[0] = static_cast<char>('1' + row);
                text[1] = '.';
                std::copy(m.scores.entries[row].name.begin(), m.scores.entries[row].name.end(), text.begin() + 2);
                char score[32];
                std::snprintf(score, sizeof(score), "%5i", m.scores.entries[row].score);
                std::copy_n(score, 5, text.begin() + 10);
                big_text(out, text, m.row_x[row], 120 + 40 * static_cast<int>(row));
            }

            break;
        }

        case Stage::credits:
            sprite(out, "ui.credits", 0, 0, 0, std::nullopt);
            break;
        case Stage::gameplay:
            break;
        case Stage::finished:
            if (!m.error.empty()) {
                big_text(out, "HIGH SCORES NOT SAVED", -1, 180);
                small_text(out, "PRESS ENTER OR ESC TO CLOSE", -1, 244);
            }

            break;
    }

    if (!m.error.empty() && m.stage != Stage::finished) {
        out.emplace_back(FillDraw{{0, 455}, 640, 25, {0, 0, 0, 255}});
        small_text(out, "HIGH SCORE STORAGE ERROR", 8, 461);
    }

    return DrawList(std::move(out));
}
} // namespace moorhuhn::ui
