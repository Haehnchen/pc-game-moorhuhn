#include "game/specials.hpp"

#include "../fixtures/commands.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace {
using namespace moorhuhn::game;
using namespace moorhuhn::contracts;
std::size_t checks{};

void require(bool value, std::string_view message) {
    ++checks;
    if (!value) {
        throw std::runtime_error(std::string(message));
    }
}

struct Hooks final : RoundHooks {
    std::function<void(RoundContext&)> init, fire, tick, select;

    void initialize(RoundContext& c) override {
        initialize_combat(c);
        initialize_specials(c);
        if (init) {
            init(c);
        }
    }

    void combat(RoundContext& c) override {
        if (fire) {
            fire(c);
        }
    }

    void update_world(RoundContext& c) override {
        if (tick) {
            tick(c);
        }
    }

    void select_target(RoundContext& c) override {
        if (select) {
            select(c);
        }
    }
};

RecordedUpdate update(std::uint32_t ms = 0, std::uint64_t sequence = 1, std::uint32_t before = 0) {
    return {{static_cast<std::int64_t>(ms) * 1000}, ms, {sequence, {static_cast<std::int64_t>(before) * 1000}, {static_cast<std::int64_t>(ms) * 1000}, {}, {}, false, true}};
}

WorldObject object(int code, int layer, WorldAsset asset, int score) {
    WorldObject o;
    const auto spec = object_spec(asset);
    o.id = {42};
    o.active = true;
    o.state = static_cast<MotionState>(code);
    o.layer = layer;
    o.asset = asset;
    o.score = score;
    o.world_x = 2000;
    o.world_y = 150;
    o.frame_count = spec.frame_count;
    o.period_ms = spec.period_ms;
    return o;
}

WorldAsset asset(std::string_view name) {
    for (int i = 0; i < 23; ++i) {
        if (object_spec(static_cast<WorldAsset>(i)).asset == name) {
            return static_cast<WorldAsset>(i);
        }
    }
    throw std::runtime_error("unknown fixture asset");
}

std::size_t popups(const Snapshot& s) {
    return static_cast<std::size_t>(std::ranges::count_if(s.round.specials.popups, [](auto& p) {
        return p.active;
    }));
}

bool effect(const UpdateOutput& out, std::string_view name) {
    return std::ranges::any_of(out.events, [&](const auto& e) {
        const auto* p = std::get_if<PresentationEffect>(&e.payload);
        return p && p->name == name;
    });
}

std::size_t sound_count(const UpdateOutput& out, std::string_view name) {
    return static_cast<std::size_t>(std::ranges::count_if(out.events, [&](const auto& e) {
        const auto* p = std::get_if<PlaySound>(&e.payload);
        return p && p->sound.value == name;
    }));
}

void matrix() {
    std::ifstream input(std::filesystem::path(__FILE__).parent_path() / "fixtures/specials/hits.csv");
    require(static_cast<bool>(input), "hit matrix fixture exists");
    std::string line;
    std::size_t rows{};
    while (std::getline(input, line)) {
        if (line.empty() || line[0] == '#') {
            continue;
        }
        std::ranges::replace(line, ',', ' ');
        std::istringstream fields(line);
        int layer{}, before{}, score{}, after{}, expected_popup{}, draws{};
        std::string image, sound;
        require(static_cast<bool>(fields >> layer >> before >> image >> score >> after >> expected_popup >> sound >> draws), "hit row parses");
        Hooks h;
        h.init = [&](RoundContext& c) {
            c.round.score = 100;
            c.round.world.objects[19] = object(before, layer, asset(image), score);
            c.round.world.objects[18] = object(15, 3, WorldAsset::scare, 0);
            c.round.world.objects[18].id = {41};
            c.round.world.objects[18].world_y = 190;
            c.persistent.cached_target_slot = 19;
            c.round.world.objects[19].linked_slot = 3;
            c.round.world.objects[3] = object(13, 4, WorldAsset::sign, -25);
        };
        SpecialsHandler handler;
        CombatResult shot;
        shot.action = CombatAction::shot;
        shot.cached_slot = 19;
        h.fire = [&](RoundContext& c) {
            handler.loaded_shot(c, shot, {1, 1});
        };
        Simulation sim;
        static_cast<void>(sim.start_round({{0}, 0, 77}, h));
        const auto out = sim.update(update(), h);
        auto s = sim.snapshot();
        const auto label = image + " state" + std::to_string(before);
        require(s.round.score == 100 + score, label + " generic score");
        require(popups(s) == static_cast<std::size_t>(expected_popup), label + " popup request");
        require(s.random.draws == static_cast<std::uint64_t>(draws), label + " RNG count");
        require(s.persistent.cached_target_slot == -1, label + " cache clears");
        const auto& result = s.round.world.objects[before == 11 ? 18 : 19];
        require(static_cast<int>(result.state) == after, label + " lifecycle");
        require(s.round.specials.ledger.size() == 1 && s.round.specials.ledger[0].requested == score, label + " ledger");
        if (sound != "none") {
            require(effect(out, sound), label + " accepted sound event");
        }
        if (before == 12) {
            require(s.round.world.objects[3].state == MotionState::dropped_banner, "plane drops linked sign");
        }
        if (before == 11) {
            require(s.round.world.objects[19].id == 41 && result.id == 42, "hat swap preserves identities");
        }
        if (before == 15) {
            require(!effect(out, "specials.object_hit"), "scare has sound/cache only");
        }
        if (expected_popup) {
            const auto& p = s.round.specials.popups[0];
            require(p.id != 0, "popup gets stable ID");
            float x = 2000, y = 150;
            if (before <= 3) {
                constexpr std::array<float, 3> dx{48, 6, -9}, dy{20, 0, -10}, radius{150, 100, 60};
                x += dx[static_cast<std::size_t>(layer - 2)];
                y += dy[static_cast<std::size_t>(layer - 2)];
                if (before <= 1) {
                    y += radius[static_cast<std::size_t>(layer - 2)];
                }
            } else {
                switch (before) {
                    case 4:
                        x += 76;
                        break;
                    case 5:
                    case 6:
                    case 7:
                        x += 16;
                        break;
                    case 10:
                    case 11:
                        x += 106;
                        break;
                    case 12:
                        x += 25;
                        break;
                    case 13:
                    case 14:
                        x += 64;
                        break;
                    default:
                        break;
                }
            }
            require(p.world_x == x && p.world_y == y, label + " popup anchor before lifecycle mutation");
        }
        ++rows;
    }
    require(rows == 24, "all24 hit rows covered");
}

void score_and_dedup() {
    SpecialsHandler handler;
    CombatResult shot;
    shot.action = CombatAction::shot;
    shot.cached_slot = 4;
    Hooks h;
    h.init = [](RoundContext& c) {
        c.round.world.objects[4] = object(22, 2, WorldAsset::chickd, 5);
        c.round.world.objects[4].active = false;
        c.persistent.cached_target_slot = 4;
    };
    h.fire = [&](RoundContext& c) {
        handler.loaded_shot(c, shot, {1, 99});
        handler.loaded_shot(c, shot, {1, 99});
    };
    Simulation s;
    static_cast<void>(s.start_round({{0}, 0, 0}, h));
    auto out = s.update(update(), h);
    const auto first = s.snapshot();
    require(first.round.score == 5 && first.round.specials.ledger.size() == 1, "inactive cached record scores once");
    require(first.persistent.cached_target_slot == 4, "unhandled falling branch retains cache");
    require(popups(first) == 0 && first.random.draws == 0, "unhandled record adds no popup or sound draw");
    require(out.events.size() == 1 && effect(out, "specials.score"), "duplicate adds no events");
    out = s.update(update(1, 2), h);
    require(out.events.empty() && s.snapshot().round == first.round, "cross-update dedup");
    h.fire = [&](RoundContext& c) {
        handler.loaded_shot(c, shot, {1, 100});
    };
    static_cast<void>(s.update(update(2, 3, 1), h));
    require(s.snapshot().round.score == 10, "new shot can score same stale slot");
    h.fire = [&](RoundContext& c) {
        c.round.world.objects[4].score = -25;
        handler.loaded_shot(c, shot, {1, 101});
    };
    static_cast<void>(s.update(update(3, 4, 2), h));
    const auto final = s.snapshot();
    require(final.round.score == 0 && final.round.specials.ledger.back().delta == -10, "negative score clamps and ledger stores actual delta");
    int total{};
    for (const auto& entry : final.round.specials.ledger) {
        total += entry.delta;
        require(total == entry.total, "ledger reconstructs each total");
    }
    h.fire = [&](RoundContext& c) {
        c.round.score = 2147483647;
        c.round.world.objects[4].score = 5;
        handler.loaded_shot(c, shot, {1, 102});
    };
    static_cast<void>(s.update(update(4, 5, 3), h));
    require(s.snapshot().round.score == 0, "ADD32 negative wrap clamps without C++ overflow");
}

void shields() {
    SpecialsHandler handler;
    CombatResult shot;
    shot.action = CombatAction::shot;
    shot.cached_slot = -1;
    shot.point = {1, 90};
    Hooks h;
    h.init = [](RoundContext& c) {
        c.round.score = 100;
        c.round.world.camera = 640;
    };
    h.fire = [&](RoundContext& c) {
        handler.loaded_shot(c, shot, {1, c.step.clock_sample + 1});
    };
    Simulation s;
    static_cast<void>(s.start_round({{0}, 0, 0}, h));
    auto out = s.update(update(), h);
    require(s.snapshot().round.score == 75 && s.snapshot().round.world.shield_variant == 1, "initial action1 penalizes but retains variant1");
    require(sound_count(out, "shield22") == 1 && popups(s.snapshot()) == 1, "shield sound and firstfree popup");
    shot.point = {60, 90};
    static_cast<void>(s.update(update(1, 2), h));
    require(s.snapshot().round.world.shield_flag == 2 && s.snapshot().persistent.shield_variant == 2, "action2 changes persistent variant");
    shot.point = {1, 90};
    static_cast<void>(s.update(update(2, 3, 1), h));
    require(s.snapshot().round.world.shield_flag == 0 && s.snapshot().persistent.shield_variant == 0, "action1 changes flag2 to0");
    for (std::uint32_t t = 3; t < 17; ++t) {
        static_cast<void>(s.update(update(t, t + 1, t - 1), h));
    }
    require(popups(s.snapshot()) == 16 && s.snapshot().round.score == 0, "sixteen popup pool bounds penalties");
    h.fire = [&](RoundContext& c) {
        c.round.score = 100;
        handler.loaded_shot(c, shot, {1, 900});
    };
    out = s.update(update(17, 18, 16), h);
    require(s.snapshot().round.score == 100 && sound_count(out, "shield22") == 1, "full popup pool suppresses penalty only");
    for (const ScreenPoint edge : std::array<ScreenPoint, 4>{{{-48, 90}, {28, 90}, {1, 79}, {1, 107}}}) {
        Hooks e;
        e.init = [](RoundContext& c) {
            c.round.score = 100;
            c.round.world.camera = 640;
        };
        shot.point = edge;
        e.fire = [&](RoundContext& c) {
            handler.loaded_shot(c, shot, {1, 1});
        };
        Simulation edge_sim;
        static_cast<void>(edge_sim.start_round({{0}, 0, 0}, e));
        out = edge_sim.update(update(), e);
        require(out.events.empty() && edge_sim.snapshot().round.score == 100, "strict shield boundary excluded");
    }
    h.fire = {};
    static_cast<void>(s.update(update(90000, 19, 17), h));
    static_cast<void>(s.start_round({{91000000}, 91000, 0}, h));
    require(s.snapshot().round.specials == SpecialsState{} && s.snapshot().persistent.shield_variant == 0, "round resets pools, retains shield artwork");
}

void trees() {
    SpecialsHandler handler;
    CombatResult shot;
    shot.action = CombatAction::shot;
    shot.cached_slot = -1;
    shot.point = {350, 200};
    Hooks h;
    h.init = [](RoundContext& c) {
        c.round.world.camera = 0;
        c.persistent.cursor = {334, 184};
    };
    h.fire = [&](RoundContext& c) {
        handler.loaded_shot(c, shot, {1, c.step.clock_sample + 1});
    };
    Simulation s;
    static_cast<void>(s.start_round({{0}, 0, 1}, h));
    auto out = s.update(update(), h);
    auto state = s.snapshot();
    require(std::ranges::count_if(state.round.world.objects,
                [](auto& o) {
                    return o.active;
                })
                == 8,
        "tree requests eight leaves");
    require(state.round.world.pool_limit == 20 && state.random.draws == 32, "tree restores limit and draws four per leaf");
    require(state.round.specials.holes[0].world_x == 334 && state.round.specials.holes[0].world_y == 184, "hole uses cursor top-left");
    require(state.round.score == 0 && state.round.specials.ledger.empty(), "tree carries no score transaction");
    require(sound_count(out, "tree22") == 1, "tree sound once");
    for (std::uint32_t t = 1; t < 18; ++t) {
        static_cast<void>(s.update(update(t, t + 1, t - 1), h));
    }
    state = s.snapshot();
    require(state.round.specials.next_hole == 2, "hole ring wraps16");
    require(std::ranges::count_if(state.round.world.objects,
                [](auto& o) {
                    return o.active;
                })
                == 28,
        "temporary pool admits slots20..31, preserves reserved0..3");
    require(state.random.draws == 112, "full pool requests consume no RNG");
    auto draw = specials_draw_data(state.round.world, state.round.specials);
    require(draw.tree_holes.size() == 16, "sixteen ring holes drawn");
    const auto& hole = std::get<SpriteDraw>(draw.tree_holes[0]);
    require(hole.destination == ScreenPoint{334, 184}, "hole projection");
    auto world = state.round.world;
    world.camera = 4;
    world.shake = 8;
    draw = specials_draw_data(world, state.round.specials);
    require(std::get<SpriteDraw>(draw.tree_holes[0]).destination == ScreenPoint{324, 192}, "hole camera and shake");
}

void rectangle_edges() {
    struct Case {
        int camera, left, right, top, bottom;
        const char* sound;
    };

    const std::array<Case, 6> cases{{{640, -48, 28, 79, 107, "shield22"}, {640, -36, 28, 115, 152, "shield22"}, {640, 56, 186, 72, 111, "shield22"},
        {0, 300, 425, 1, 479, "tree22"}, {800, 220, 345, 1, 479, "tree22"}, {1280, 275, 376, 1, 479, "tree22"}}};
    SpecialsHandler handler;
    for (const auto& r : cases) {
        const auto x = (r.left + r.right) / 2, y = (r.top + r.bottom) / 2;
        const std::array<ScreenPoint, 5> points{{{x, y}, {r.left, y}, {r.right, y}, {x, r.top}, {x, r.bottom}}};
        for (std::size_t i = 0; i < points.size(); ++i) {
            Hooks h;
            h.init = [&](RoundContext& c) {
                c.round.world.camera = r.camera;
                c.round.score = 100;
                c.persistent.cursor = {points[i].x - 16, points[i].y - 16};
            };
            h.fire = [&](RoundContext& c) {
                CombatResult shot;
                shot.action = CombatAction::shot;
                shot.cached_slot = -1;
                shot.point = points[i];
                handler.loaded_shot(c, shot, {1, 1});
            };
            Simulation s;
            static_cast<void>(s.start_round({{0}, 0, 0}, h));
            const auto out = s.update(update(), h);
            require(sound_count(out, r.sound) == (i == 0 ? 1U : 0U), "six special rectangles use strict edges and camera projection");
        }
    }
    Hooks h;
    h.init = [](RoundContext& c) {
        c.round.world.camera = 0;
        c.round.world.objects[4] = object(22, 2, WorldAsset::chickd, 5);
    };
    h.fire = [&](RoundContext& c) {
        CombatResult shot;
        shot.action = CombatAction::shot;
        shot.cached_slot = 4;
        shot.point = {350, 200};
        handler.loaded_shot(c, shot, {1, 1});
    };
    Simulation s;
    static_cast<void>(s.start_round({{0}, 0, 0}, h));
    const auto out = s.update(update(), h);
    require(sound_count(out, "tree22") == 0 && s.snapshot().round.score == 5, "nonnegative old cache suppresses rectangle scan");
}

void popup_draws() {
    Hooks h;
    h.init = [](RoundContext& c) {
        c.round.world.camera = 620;
        for (int layer = 0; layer < 5; ++layer) {
            auto& p = c.round.specials.popups[static_cast<std::size_t>(layer)];
            p.active = true;
            p.layer = layer;
            p.score = 25;
            p.world_x = 2000;
            p.world_y = 150;
        }
    };
    h.tick = advance_specials;
    Simulation s;
    static_cast<void>(s.start_round({{0}, 0, 0}, h));
    static_cast<void>(s.update(update(100), h));
    auto snap = s.snapshot();
    const auto draw = specials_draw_data(snap.round.world, snap.round.specials);
    constexpr std::array<int, 5> offsets{1550, 1550, 1364, 1177, 992};
    constexpr std::array<float, 5> moved{135, 135, 140, 145, 147.5F};
    for (std::size_t i = 0; i < 5; ++i) {
        const auto& p = snap.round.specials.popups[i];
        require(p.world_y == moved[i], "popup layer movement");
        require(p.screen_y == 150 && p.screen_x == 2000 - offsets[i], "popup priorY/live camera truncation");
        require(draw.after_objects[i].size() == 2, "25 two glyphs");
        require(std::get<SpriteDraw>(draw.after_objects[i][0]).frame.index == 127, "digit2 frame127");
    }
    static_cast<void>(s.update(update(1999, 2, 100), h));
    require(popups(s.snapshot()) == 5, "popup remains before2000");
    static_cast<void>(s.update(update(2000, 3, 1999), h));
    require(popups(s.snapshot()) == 0, "popup expires at2000");
    SpecialsState state;
    const std::array<int, 7> values{5, 10, 25, 50, -25, -5, -50};
    const std::array<std::vector<std::uint32_t>, 7> expected{{{130}, {126, 125}, {127, 130}, {130, 125}, {124, 127, 130}, {124, 130}, {}}};
    for (std::size_t i = 0; i < values.size(); ++i) {
        auto& p = state.popups[0];
        p = {};
        p.active = true;
        p.layer = 3;
        p.score = values[i];
        p.screen_x = 10;
        p.screen_y = 20;
        const auto d = specials_draw_data({}, state);
        require(d.after_objects[3].size() == expected[i].size(), "supported glyph count");
        for (std::size_t j = 0; j < expected[i].size(); ++j) {
            const auto& glyph = std::get<SpriteDraw>(d.after_objects[3][j]);
            require(glyph.frame.index == expected[i][j] && glyph.destination == ScreenPoint{10 + static_cast<int>(j) * 24, 20}, "glyph frame and spacing");
        }
        require(state.popups[0].screen_x == 10, "read-only popup draw");
    }
    h.init = [](RoundContext& c) {
        auto& p = c.round.specials.popups[0];
        p.active = true;
        p.score = 0;
        p.layer = 1;
        p.world_y = 150;
    };
    Simulation frozen;
    static_cast<void>(frozen.start_round({{0}, 0, 0}, h));
    static_cast<void>(frozen.update(update(3000), h));
    require(frozen.snapshot().round.specials.popups[0].active && frozen.snapshot().round.specials.popups[0].elapsed_ms == 0, "unsupported layer1 score retains untimed popup");
}

void terminal_combat() {
    Hooks h;
    SpecialsHandler handler;
    CombatResult result;
    h.init = [](RoundContext& c) {
        c.round.world.objects[4] = object(2, 2, WorldAsset::chickr, 5);
        c.persistent.cached_target_slot = 4;
    };
    h.fire = [&](RoundContext& c) {
        HitCandidate candidate;
        candidate.slot = 4;
        candidate.id = 42;
        candidate.kind = "chickr";
        candidate.state = 2;
        candidate.active = true;
        candidate.frame = {{"chickr"}, 0};
        result = process_combat(c, std::span{&candidate, 1}, &handler);
    };
    Simulation s;
    static_cast<void>(s.start_round({{0}, 0, 0}, h));
    auto u = update(90000);
    u.input.actions.push_back({{90000000}, 1, InputControl::primary, InputEdge::pressed, ScreenPoint{200, 100}, {}});
    const auto out = s.update(u, h);
    const auto state = s.snapshot();
    require(state.phase == Phase::ended && state.round.score == 5 && state.round.magazine.loaded == 7, "terminal loaded shot scores before expiry and ammo decrement");
    const auto found = std::ranges::find_if(out.events, [](auto& e) {
        auto* p = std::get_if<PresentationEffect>(&e.payload);
        return p && p->name == "combat.shot";
    });
    require(found != out.events.end() && state.round.specials.ledger[0].shot.event == found->sequence, "ledger references exact combat event");
    require(result.ejected_shell == 0 && state.round.specials.ledger[0].shot.round == 1, "callback integration identity");
}

void no_loaded_action() {
    Hooks h;
    SpecialsHandler handler;
    h.init = [](RoundContext& c) {
        c.round.world.camera = 640;
        c.persistent.cursor = {-15, 74};
        c.round.score = 100;
        c.round.magazine.loaded = 0;
    };
    h.fire = [&](RoundContext& c) {
        static_cast<void>(process_combat(c, {}, &handler));
    };
    Simulation s;
    static_cast<void>(s.start_round({{0}, 0, 0}, h));
    auto u = update();
    u.input.actions.push_back({{0}, 1, InputControl::primary, InputEdge::pressed, ScreenPoint{1, 90}, {}});
    const auto out = s.update(u, h);
    require(s.snapshot().round.score == 100 && s.snapshot().round.specials.processed_shots.empty(), "empty primary has no scoring callback");
    require(sound_count(out, "shield22") == 0, "empty click has no special sound");
    u = update(1, 2);
    u.input.actions.push_back({{1000}, 1, InputControl::secondary, InputEdge::pressed, ScreenPoint{1, 90}, {}});
    static_cast<void>(s.update(u, h));
    require(s.snapshot().round.magazine.loaded == 8 && s.snapshot().round.specials.ledger.empty(), "reload has no scoring callback");
}

void replay_presentations() {
    moorhuhn::tests::CommandSequence replay;
    replay.commands.push_back(RoundStart{{0}, 0, 33});
    for (std::uint32_t t = 100; t <= 2400; t += 100) {
        replay.commands.push_back(update(t, t / 100, t - 100));
    }
    auto run = [&](int rate) {
        Hooks h;
        SpecialsHandler handler;
        h.init = [](RoundContext& c) {
            initialize_world(c);
            c.round.world.camera = 0;
            c.persistent.cursor = {334, 184};
        };
        h.fire = [&](RoundContext& c) {
            if (c.step.clock_sample <= 400) {
                CombatResult shot;
                shot.action = CombatAction::shot;
                shot.cached_slot = -1;
                shot.point = {350, 200};
                handler.loaded_shot(c, shot, {1, c.step.clock_sample});
            }
        };
        h.tick = [](RoundContext& c) {
            advance_world(c);
            advance_specials(c);
        };
        Simulation sim(replay.initial);
        std::vector<std::uint64_t> events;
        for (const auto& command : replay.commands) {
            const auto out = moorhuhn::tests::apply(sim, command, h);
            for (const auto& e : out.events) {
                events.push_back(e.sequence);
            }
            const auto saved = sim.snapshot();
            const int presentations = rate == 0 ? static_cast<int>(saved.update_index % 5) + 1 : rate / 10;
            for (int i = 0; i < presentations; ++i) {
                const auto data = specials_draw_data(saved.round.world, saved.round.specials);
                static_cast<void>(world_draw_list(saved.round.world, data.extras()));
                require(sim.snapshot() == saved, "presentation leaves whole snapshot immutable");
            }
        }
        return std::pair{sim.snapshot(), events};
    };
    const auto reference = run(30);
    for (int rate : std::array<int, 3>{60, 144, 0}) {
        require(run(rate) == reference, "recorded boundaries replay across presentation schedules");
    }
}
} // namespace

int main() {
    try {
        matrix();
        score_and_dedup();
        shields();
        trees();
        rectangle_edges();
        popup_draws();
        terminal_combat();
        no_loaded_action();
        replay_presentations();
        std::cout << "Specials: " << checks << " checks passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Specials: " << e.what() << '\n';
        return 1;
    }
}
