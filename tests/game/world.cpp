#include "game/world.hpp"

#include "game/combat.hpp"
#include "game/simulation.hpp"
#include "render/renderer.hpp"
#ifdef MOORHUHN_WORLD_ASSET_TESTS
#include "assets/asset_store.hpp"
#endif

#include <algorithm>
#include <bit>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>

namespace {
using namespace moorhuhn::contracts;
using namespace moorhuhn::game;
std::size_t checks{};

void require(bool condition, std::string_view message) {
    ++checks;
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

template <class F> void rejected(F fn, std::string_view message) {
    bool threw = false;
    try {
        fn();
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    require(threw, message);
}

void near_bits(float value, std::uint32_t bits, std::string_view label) {
    const auto expected = std::bit_cast<float>(bits);
    const float tolerance = std::max(0.000001F, std::abs(expected) * 0.00000024F);
    require(std::abs(value - expected) <= tolerance, label);
}

WorldObject record(int code, int layer, WorldAsset image) {
    const auto spec = object_spec(image);
    WorldObject o;
    o.id = {1};
    o.active = true;
    o.state = static_cast<MotionState>(code);
    o.layer = layer;
    o.asset = image;
    o.world_x = 2000;
    o.world_y = 100;
    o.frame_count = spec.frame_count;
    o.period_ms = spec.period_ms;
    return o;
}

struct Hooks final : RoundHooks {
    std::function<void(RoundContext&)> init, camera, shot, tick, selection;

    void initialize(RoundContext& c) override {
        if (init) {
            init(c);
        }
    }

    void camera_and_spawn(RoundContext& c) override {
        if (camera) {
            camera(c);
        }
    }

    void combat(RoundContext& c) override {
        if (shot) {
            shot(c);
        }
    }

    void update_world(RoundContext& c) override {
        if (tick) {
            tick(c);
        }
    }

    void select_target(RoundContext& c) override {
        if (selection) {
            selection(c);
        }
    }
};

RecordedUpdate step(std::uint32_t ms, std::uint64_t sequence = 1, std::uint32_t before = 0, std::optional<ScreenPoint> pointer = std::nullopt) {
    const auto at = static_cast<std::int64_t>(ms) * 1000;
    return {{at}, ms, {sequence, {static_cast<std::int64_t>(before) * 1000}, {at}, pointer, {}, false, true}};
}

WorldSnapshot once(WorldSnapshot initial, std::uint32_t ms, UpdateOutput* events = nullptr) {
    Hooks hooks;
    hooks.init = [&](RoundContext& c) {
        c.round.world = initial;
    };
    hooks.tick = advance_world;
    Simulation sim;
    static_cast<void>(sim.start_round({{0}, 0, 0}, hooks));
    auto out = sim.update(step(ms), hooks);
    if (events) {
        *events = std::move(out);
    }
    return sim.snapshot().round.world;
}

void motion_fixture() {
    const auto path = std::filesystem::path(__FILE__).parent_path() / "fixtures/world/motion.csv";
    std::ifstream input(path);
    require(static_cast<bool>(input), "motion fixture available");
    std::string line;
    std::size_t rows = 0;
    while (std::getline(input, line)) {
        if (line.empty() || line[0] == '#') {
            continue;
        }
        std::replace(line.begin(), line.end(), ',', ' ');
        std::istringstream fields(line);
        int code{}, image{}, layer{}, frame{};
        std::uint32_t phase{}, x{}, y{}, sx{}, sy{}, next{};
        require(static_cast<bool>(fields >> code >> image >> layer >> phase >> x >> y >> sx >> sy >> next >> frame), "motion row parses");
        WorldSnapshot w;
        w.shake = 8;
        w.objects[4] = record(code, layer, static_cast<WorldAsset>(image));
        w.objects[4].phase = std::bit_cast<float>(phase);
        const auto result = once(w, 100).objects[4];
        const auto label = "motion state " + std::to_string(code);
        require(result.active, label + " stays active");
        near_bits(result.world_x, x, label + " world x");
        near_bits(result.world_y, y, label + " world y");
        near_bits(result.screen_x, sx, label + " screen x");
        near_bits(result.screen_y, sy, label + " screen y");
        near_bits(result.phase, next, label + " phase");
        require(result.frame == frame, label + " frame");
        ++rows;
    }
    require(rows == 39, "all39 active states covered");
    for (int state = 0; state <= 60; ++state) {
        const bool expected = (state <= 15) || (state >= 20 && state <= 36) || state == 50 || state == 51 || (state >= 53 && state <= 56);
        require(drawable(static_cast<MotionState>(state)) == expected, "motion roster excludes unused states");
    }
}

void camera_rules() {
    auto camera = [](int value, ScreenPoint cursor, bool left, bool right) {
        Hooks h;
        h.init = [&](RoundContext& c) {
            c.round.world.camera = value;
            c.persistent.cursor = cursor;
            c.persistent.held[static_cast<std::size_t>(InputControl::left)] = left;
            c.persistent.held[static_cast<std::size_t>(InputControl::right)] = right;
        };
        h.camera = camera_and_spawn_world;
        Simulation s;
        static_cast<void>(s.start_round({{0}, 0, 0}, h));
        static_cast<void>(s.update(step(0), h));
        return s.snapshot().round.world.camera;
    };
    require(camera(640, {31, 240}, false, false) == 636, "pointerleft strict32");
    require(camera(640, {32, 240}, false, false) == 640, "pointer32 neutral");
    require(camera(640, {575, 240}, false, false) == 640, "pointer575 neutral");
    require(camera(640, {576, 240}, false, false) == 644, "pointerright inclusive576");
    require(camera(640, {0, 240}, true, false) == 632, "pointer+keyboard two steps");
    require(camera(0, {0, 240}, true, false) == 0, "left boundary");
    require(camera(1280, {608, 240}, false, true) == 1280, "right boundary");
    require(camera(0, {0, 240}, true, true) == 4, "ordered boundary left left right");
    require(camera(1280, {608, 240}, true, true) == 1280, "ordered boundary right left right");
    require(camera(1276, {608, 240}, false, false) == 1280, "reachable right endpoint");
    WorldSnapshot w;
    w.camera = 1000;
    w.shake = 8;
    const auto draws = world_draw_list(w);
    std::array<int, 6> first{};
    first.fill(std::numeric_limits<int>::max());
    for (const auto& draw : draws.commands()) {
        if (const auto* sprite = std::get_if<SpriteDraw>(&draw)) {
            const auto& name = sprite->frame.image.value;
            if (name.size() == 6 && name.starts_with("layer")) {
                const auto layer = static_cast<std::size_t>(name.back() - '0');
                if (first[layer] == std::numeric_limits<int>::max()) {
                    first[layer] = sprite->destination.x;
                }
            }
        }
    }
    require(first[1] == -19, "background1 binary32 1.3 trunc1299");
    require(first[3] == -43, "background3 binary64 1.9 trunc1899");
    w.objects[4] = record(10, 3, WorldAsset::wing);
    const auto o = once(w, 0).objects[4];
    require(o.screen_x == 101, "object binary32 1.9 product remains below1900");
    require(projected_integer(-1.9F) == -1, "screen trunc towardzero");
    rejected(
        [] {
            static_cast<void>(projected_integer(std::numeric_limits<float>::infinity()));
        },
        "infinite projection rejected");
}

void initialization_and_spawns() {
    PersistentState persistent;
    persistent.shield_variant = 2;
    persistent.cursor = {608, 448};
    Hooks h;
    h.init = initialize_world;
    Simulation s(persistent);
    static_cast<void>(s.start_round({{0}, 0, 1}, h));
    const auto initial = s.snapshot();
    require(initial.persistent.cursor == ScreenPoint{320, 240}, "roundstart resets cursor");
    require(initial.round.world.camera == 640 && initial.round.world.shield_variant == 2 && initial.round.world.shield_flag == 0, "round reset/persistent shield");
    require(initial.round.world.objects[20].state == MotionState::windmill, "windmill resident");
    for (int i = 0; i < 4; ++i) {
        const auto& tiny = initial.round.world.objects[static_cast<std::size_t>(i)];
        require(tiny.state == MotionState::tiny && tiny.world_x >= 855 && tiny.world_x <= 862 && tiny.world_y >= 148 && tiny.world_y <= 151, "tiny bounds");
        require(tiny.layer == 4 || tiny.layer == 5, "tiny random layer");
        const auto& wing = initial.round.world.objects[static_cast<std::size_t>(21 + i)];
        require(wing.active && wing.frame == i * 9 && wing.frame_count == 37 && wing.period_ms == 50, "four wing phases");
    }
    RandomGenerator expected(1);
    for (int i = 0; i < 12; ++i) {
        static_cast<void>(expected.draw());
    }
    constexpr std::array<int, 32> type_table{0, 1, 2, 0, 1, 2, 0, 1, 2, 0, 1, 2, 7, 5, 4, 3, 0, 1, 2, 0, 1, 2, 0, 1, 2, 0, 1, 2, 0, 1, 2, 0};
    for (int n = 0; n < 2; ++n) {
        const int type = type_table[expected.draw() & 31];
        const int count = std::array<int, 8>{2, 2, 3, 1, 2, 3, 4, 1}[static_cast<std::size_t>(type)];
        for (int i = 0; i < count; ++i) {
            static_cast<void>(expected.draw());
        }
    }
    require(initial.random == expected.snapshot(), "12 tiny draws before two spawns");
    for (int type = 0; type < 8; ++type) {
        for (std::uint32_t seed = 0; seed < 16; ++seed) {
            bool created = false;
            Hooks spawn;
            spawn.init = [&](RoundContext& c) {
                created = spawn_world(c, type);
            };
            Simulation sim;
            static_cast<void>(sim.start_round({{0}, 0, seed}, spawn));
            const auto snap = sim.snapshot();
            require(created, "explicit spawn succeeds");
            require(snap.random.draws == static_cast<std::uint64_t>(std::array<int, 8>{2, 2, 3, 1, 2, 3, 4, 1}[static_cast<std::size_t>(type)]), "spawn draw count");
            const auto& o = snap.round.world.objects[4];
            require(o.active && o.id == 1, "firstfree slot+stableID");
            if (type <= 2) {
                require(o.layer >= 2 && o.layer <= 4, "chicken size variant");
                require(o.frame_count == 19, "chicken active19");
                if (type == 2) {
                    require(o.screen_x == -150 || o.screen_x == 670, "horizontal edge spawn");
                }
            }
            if (type == 7) {
                require(snap.round.world.objects[5].state == MotionState::banner && o.linked_slot == 5 && snap.round.world.plane_present, "linked plane banner");
            }
        }
    }
    for (int scenario = 0; scenario < 3; ++scenario) {
        bool created = true;
        Hooks spawn;
        spawn.init = [&](RoundContext& c) {
            c.round.world.pool_limit = scenario == 1 ? 5 : 4;
            c.round.world.plane_present = scenario == 2;
            if (scenario == 2) {
                c.round.world.pool_limit = 20;
            }
            created = spawn_world(c, 7);
        };
        Simulation sim;
        static_cast<void>(sim.start_round({{0}, 0, 0}, spawn));
        const auto snap = sim.snapshot();
        require(!created, "unavailable plane rejected");
        require(snap.random.draws == static_cast<std::uint64_t>(scenario == 1 ? 1 : 0), "fullpool/plane guard draws");
        require(!snap.round.world.objects[4].active, "partial plane creation retired");
    }
}

void animation_and_lifecycle() {
    WorldSnapshot w;
    w.objects[4] = record(2, 2, WorldAsset::chickr);
    require(once(w, 62).objects[4].frame == 0, "animation below boundary");
    w.objects[4].animation_ms = .5F;
    require(once(w, 62).objects[4].frame == 1, "animation exact boundary");
    w.objects[4].animation_ms = 0;
    auto out = once(w, 1000).objects[4];
    require(out.frame == 1 && out.animation_ms == 937.5F, "animation one advancement no catchup");
    w.objects[4].frame = 18;
    require(once(w, 63).objects[4].frame == 0, "flight wraps19");
    w.objects[4] = record(8, 0, WorldAsset::leaf);
    w.objects[4].frame = 18;
    require(once(w, 50).objects[4].frame == 0, "leaf excludes stored frame19");
    w.objects[4] = record(6, 4, WorldAsset::balloon);
    w.objects[4].frame = 19;
    out = once(w, 400).objects[4];
    require(out.frame == 20 && !out.ping_forward, "balloon turns at20");
    w.objects[4] = out;
    out = once(w, 400).objects[4];
    require(out.frame == 19, "balloon reverse");
    w.objects[4].frame = 1;
    w.objects[4].ping_forward = false;
    out = once(w, 400).objects[4];
    require(out.frame == 0 && out.ping_forward, "balloon turns at0");
    for (int size = 0; size < 3; ++size) {
        for (int code = 0; code < 4; ++code) {
            w = WorldSnapshot{};
            w.objects[4] = record(code, 2 + size, static_cast<WorldAsset>(size));
            const auto id = w.objects[4].id;
            w.objects[4].phase = .025F;
            const auto change = apply_world_hit(w, 4);
            require(change.changed && static_cast<int>(w.objects[4].state) == 20 + size * 4 + code && w.objects[4].id == id, "flying hit maps all12 fall states");
            require(w.objects[4].asset == static_cast<WorldAsset>(6 + size) && w.objects[4].period_ms == 50, "fall asset+period");
        }
    }
    constexpr std::array<std::array<int, 4>, 7> transitions{{{4, 1, 9, 32}, {5, 1, 11, 33}, {6, 4, 12, 34}, {7, 4, 12, 34}, {8, 0, 14, 35}, {9, 1, 14, 36}, {10, 3, 19, 51}}};
    for (const auto row : transitions) {
        w = WorldSnapshot{};
        w.objects[4] = record(row[0], row[1], static_cast<WorldAsset>(row[2]));
        require(apply_world_hit(w, 4).changed && static_cast<int>(w.objects[4].state) == row[3], "special lifecycle transition");
    }
    w = WorldSnapshot{};
    w.objects[18] = record(15, 3, WorldAsset::scare);
    w.objects[19] = record(11, 3, WorldAsset::hat);
    w.objects[19].id = {2};
    require(!apply_world_hit(w, 18).changed && w.objects[18].state == MotionState::scare, "scare has no lifecycle change");
    const auto hat_change = apply_world_hit(w, 19);
    require(
        hat_change.changed && hat_change.after == MotionState::hat_swap_hit && w.objects[18].id == 2 && w.objects[18].state == MotionState::hat_swap_hit && w.objects[19].id == 1,
        "hat swaps whole records");
    w = WorldSnapshot{};
    w.objects[4] = record(12, 4, WorldAsset::plane);
    w.objects[4].linked_slot = 5;
    w.objects[5] = record(13, 4, WorldAsset::sign);
    w.objects[5].id = {2};
    require(apply_world_hit(w, 4).changed && w.objects[5].state == MotionState::dropped_banner, "plane hit drops linked banner");
    require(apply_world_hit(w, 5).changed && w.objects[5].state == MotionState::banner_hit, "dropped banner remains hittable by cached branch");
    for (int size = 0; size < 3; ++size) {
        w = WorldSnapshot{};
        w.objects[4] = record(22 + size * 4, 2 + size, static_cast<WorldAsset>(6 + size));
        w.objects[4].world_y = static_cast<float>(400 - 50 * size);
        UpdateOutput events;
        out = once(w, 0).objects[4];
        require(out.active, "fall strict boundary");
        out = once(w, 1, &events).objects[4];
        require(!out.active && events.events.size() == 2, "fall crossing emits sound then retirement");
    }
    w = WorldSnapshot{};
    w.objects[4] = record(2, 4, WorldAsset::chickr3);
    w.objects[4].world_x = 10000;
    require(once(w, 1).objects[4].active, "small horizontal original compares y not x");
    w.objects[4].world_y = 3201;
    require(!once(w, 0).objects[4].active, "small horizontal y retirement");
    w.objects[4] = record(32, 1, WorldAsset::bighit);
    w.objects[4].frame = 16;
    require(!once(w, 50).objects[4].active, "bighit completes17");
    w.objects[4] = record(35, 0, WorldAsset::leafhit);
    w.objects[4].frame = 24;
    require(!once(w, 20).objects[4].active, "leafhit completes25");
    // Branch-specific strict retirement fields; shake is zero in this table.
    constexpr std::array<std::array<int, 4>, 10> bounds{{{8, 0, 14, 480}, {9, 1, 14, 480}, {14, 4, 21, 350}, {34, 4, 13, 300}, {51, 3, 19, 350}, {53, 3, 17, 400}, {54, 4, 20, 275},
        {55, 4, 21, 350}, {12, 4, 20, -120}, {13, 4, 21, -120}}};
    for (const auto row : bounds) {
        w = WorldSnapshot{};
        w.objects[4] = record(row[0], row[1], static_cast<WorldAsset>(row[2]));
        if (row[0] == 12 || row[0] == 13) {
            w.objects[4].world_x = static_cast<float>(row[3]);
        } else {
            w.objects[4].world_y = static_cast<float>(row[3]);
        }
        require(once(w, 0).objects[4].active, "retirement threshold equality remains active");
        require(!once(w, 1).objects[4].active, "retirement threshold crossing removes object");
    }
    w = WorldSnapshot{};
    w.objects[4] = record(33, 1, WorldAsset::guck);
    w.objects[4].phase = .000001F;
    require(!once(w, 1).objects[4].active, "peek hit phase completion");
    w.objects[4] = record(0, 2, WorldAsset::chickl);
    w.objects[4].phase = .1F;
    require(!once(w, 0).objects[4].active, "arc-left phase inclusive end");
    w.objects[4] = record(1, 2, WorldAsset::chickr);
    w.objects[4].phase = 0;
    require(!once(w, 0).objects[4].active, "arc-right phase inclusive end");
    w = WorldSnapshot{};
    w.shake = 8;
    w.objects[4] = record(54, 4, WorldAsset::plane);
    w.objects[4].world_y = 274;
    require(once(w, 0).objects[4].active, "plane hit retires by world y, not shaken screen y");
    require(!once(w, 21).objects[4].active, "plane world y crossing removes it");
    w = WorldSnapshot{};
    w.objects[4] = record(22, 2, WorldAsset::chickd);
    w.objects[4].world_y = 400;
    w.objects[4].animation_ms = 49;
    out = once(w, 1).objects[4];
    require(!out.active && out.frame == 1 && out.animation_ms == 0, "retiring fall still advances animation");
    w.objects[4] = record(4, 1, WorldAsset::big1);
    w.objects[4].phase = 5000;
    const auto retired = once(w, 63);
    require(!retired.objects[4].active && retired.objects[4].frame == -1, "close retains terminal negative frame");
    const auto retired_candidates = world_hit_candidates(retired, {});
    require(!retired_candidates[4].active && retired_candidates[4].frame.index == 0, "inactive negative frame uses safe candidate carrier");
    w.objects[4] = record(2, 2, WorldAsset::chickr);
    w.objects[4].active = false;
    require(apply_world_hit(w, 4).changed && w.objects[4].active, "replacement restores original game asset pointer on cached inactive slot");
    for (int stage : {0, 4, 5, 6, 10, 14}) {
        w = WorldSnapshot{};
        w.objects[4] = record(4, 1, WorldAsset::big1);
        w.objects[4].phase = static_cast<float>(stage * 500);
        w.objects[4].frame = 12;
        out = once(w, 63).objects[4];
        require(out.stage == stage, "close live stage");
        const int expected = stage == 0 ? 11 : stage == 4 ? 13 : stage == 5 ? 12 : stage == 10 ? 11 : 12;
        require(out.frame == expected, "close branch-specific frame");
    }
}

struct SyntheticImage {
    AssetId id;
    int width, height;
    std::vector<AtlasRect> frames;
    std::vector<RgbaPixel> rgba;
    std::vector<std::uint8_t> indices;

    AssetView view() const {
        return {id, width, height, static_cast<std::uint32_t>(width), frames, rgba, indices, {}};
    }
};

std::vector<SyntheticImage> synthetic_images() {
    std::vector<SyntheticImage> images;
    auto add = [&](std::string name, int width, int height, std::uint8_t index) {
        const auto count = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
        images.push_back(
            {{std::move(name)}, width, height, {{0, 0, width, height}}, std::vector<RgbaPixel>(count, {index, index, index, 255}), std::vector<std::uint8_t>(count, index)});
    };
    add("layer0", 1920, 260, 3);
    for (int i = 1; i <= 5; ++i) {
        add("layer" + std::to_string(i), 1920, i == 5 ? 488 : 480, 18);
    }
    add("shield", 768, 488, 18);
    add("chickl", 120, 80, 7);
    add("chickr", 120, 80, 9);
    add("effect", 1, 1, 11);
    images[7].indices[0] = 18;
    images[7].rgba[0] = {18, 18, 18, 255};
    return images;
}

void composition() {
    auto storage = synthetic_images();
    std::vector<AssetView> views;
    for (const auto& image : storage) {
        views.push_back(image.view());
    }
    IndexedWorldAssets indexed(views);
    WorldSnapshot w;
    w.objects[4] = record(0, 2, WorldAsset::chickl);
    w.objects[4].screen_x = 100.75F;
    w.objects[4].screen_y = 100.75F;
    w.objects[5] = record(1, 1, WorldAsset::chickr);
    w.objects[5].id = {2};
    w.objects[5].screen_x = 120.5F;
    w.objects[5].screen_y = 100.25F;
    const auto before = w;
    const auto draws = world_draw_list(w);
    const auto scene = indexed.compose(draws);
    const auto candidates = world_hit_candidates(w, views);
    require(w == before, "draw+candidate operations readonly");
    require(scene[100 * 640 + 100] == 3, "index18 transparent edge");
    require(scene[110 * 640 + 110] == 7, "visible chicken indexed geometry");
    require(scene[110 * 640 + 130] == 9, "nearer layer occludes earlier chicken");
    PersistentState p;
    p.selector_state.resize(65536);
    const auto hit = select_indexed_target(p, {94, 94}, {scene, 640}, candidates);
    require(hit.hit && hit.hit->target.slot == 4 && hit.hit->frame_pixel == ScreenPoint{10, 10}, "Selector uses the projected frame");
    const auto& partial_scene = indexed.compose(draws, AtlasRect{110, 110, 1, 1});
    require(partial_scene[110 * 640 + 110] == scene[110 * 640 + 110], "Region composition preserves the full-scene hotspot index");
    const auto occluded = select_indexed_target(p, {114, 94}, {scene, 640}, candidates);
    require(occluded.hit && occluded.hit->target.slot == 5, "Selector uses the visible nearer layer");
    quantize_world_position(w, 4, 1);
    require(w.objects[4].screen_x == 100 && w.objects[4].screen_y == 100, "selector quantization explicit");
    rejected(
        [&] {
            quantize_world_position(w, 4, 999);
        },
        "stale quantized identity rejected");
    w = WorldSnapshot{};
    for (int layer = 0; layer <= 5; ++layer) {
        w.objects[static_cast<std::size_t>(layer)] = record(0, layer, WorldAsset::chickl);
        w.objects[static_cast<std::size_t>(layer)].id = {static_cast<std::uint64_t>(layer + 1)};
    }
    std::array<std::vector<DrawCommand>, 6> extras;
    WorldDrawExtras insertions;
    for (std::size_t layer = 0; layer < 6; ++layer) {
        extras[layer].emplace_back(FillDraw{{static_cast<int>(layer), 0}, 1, 1, {11, 11, 11, 255}});
        insertions.after_objects[layer] = extras[layer];
    }
    const std::array<DrawCommand, 1> holes{FillDraw{{99, 0}, 1, 1, {12, 12, 12, 255}}};
    insertions.tree_holes = holes;
    const auto ordered = world_draw_list(w, insertions);
    std::array<int, 6> object_pos{}, extra_pos{};
    int hole_pos = -1, layer5_end = -1, index = 0;
    for (const auto& command : ordered.commands()) {
        if (const auto* fill = std::get_if<FillDraw>(&command)) {
            if (fill->destination.x == 99) {
                hole_pos = index;
            } else {
                extra_pos[static_cast<std::size_t>(fill->destination.x)] = index;
            }
        }
        if (const auto* sprite = std::get_if<SpriteDraw>(&command)) {
            if (sprite->frame.image.value == "chickl") {
                const auto layer = 5 - static_cast<int>(std::ranges::count_if(object_pos, [&](int v) {
                    return v > 0;
                }));
                object_pos[static_cast<std::size_t>(layer)] = index;
            }
            if (sprite->frame.image.value == "layer5" || sprite->frame.image.value == "shield") {
                layer5_end = index;
            }
        }
        ++index;
    }
    for (std::size_t layer = 0; layer < 6; ++layer) {
        require(extra_pos[layer] == object_pos[layer] + 1, "effects inserted after same-layer objects");
    }
    require(layer5_end < hole_pos && hole_pos < object_pos[0], "holes between foreground and object0");
    w = WorldSnapshot{};
    w.camera = 600;
    w.shield_variant = 2;
    const auto shield_draws = world_draw_list(w);
    std::size_t shield_count = 0;
    for (const auto& command : shield_draws.commands()) {
        if (const auto* sprite = std::get_if<SpriteDraw>(&command); sprite && sprite->frame.image.value == "shield") {
            require(sprite->source_rect && sprite->source_rect->x >= 512 && sprite->source_rect->x < 768, "shield variant crop");
            ++shield_count;
        }
    }
    require(shield_count == 4, "four shield strips");
    const auto first = indexed.compose(draws);
    storage.clear();
    require(indexed.compose(draws) == first, "indexed cache owns sourcebuffers");
}

std::string event_key(const GameEvent& event) {
    std::string result = std::to_string(event.sequence) + ":" + std::to_string(event.at.microseconds) + ":";
    if (const auto* effect = std::get_if<PresentationEffect>(&event.payload)) {
        result += effect->name + ":" + std::to_string(effect->object.value_or(0));
        if (effect->position) {
            result += ":" + std::to_string(effect->position->x) + ":" + std::to_string(effect->position->y);
        }
    } else {
        result += std::get<PlaySound>(event.payload).sound.value;
    }
    return result;
}

void replay_transport() {
    std::vector<RecordedUpdate> commands;
    for (std::uint32_t ms = 17, sequence = 1; ms <= 12000; ms += 17, ++sequence) {
        auto command = step(ms, sequence, ms - 17, ScreenPoint{sequence % 3 ? 320 : 0, 240});
        if (sequence % 41 == 0) {
            command.input.actions.push_back({command.at, sequence, InputControl::primary, InputEdge::pressed, ScreenPoint{320, 240}, {}});
        }
        commands.push_back(std::move(command));
    }
    auto replay = [&](int hz) {
        Hooks h;
        h.init = initialize_world;
        h.camera = camera_and_spawn_world;
        h.tick = advance_world;
        std::size_t hits = 0;
        h.shot = [&](RoundContext& context) {
            // Explicit target slot bypasses hit selection.
            const auto& target = context.round.world.objects[4];
            if ((context.step.action_mask & 1) && target.active && static_cast<int>(target.state) < 4) {
                require(apply_world_hit(context.round.world, 4).changed, "recorded lifecycle input accepted");
                ++hits;
            }
        };
        Simulation s;
        std::vector<std::string> events;
        auto out = s.start_round({{0}, 0, 23}, h);
        for (const auto& event : out.events) {
            events.push_back(event_key(event));
        }
        std::size_t next = 0;
        for (std::int64_t present = 0; next < commands.size(); present += 1000000 / hz) {
            while (next < commands.size() && commands[next].at.microseconds <= present) {
                out = s.update(commands[next++], h);
                for (const auto& event : out.events) {
                    events.push_back(event_key(event));
                }
            }
            const auto before = s.snapshot();
            static_cast<void>(world_draw_list(before.round.world));
            require(s.snapshot() == before, "presentation cannot advance world");
        }
        require(hits > 0, "replay includes explicit flight-to-fall transitions");
        return std::pair{s.snapshot(), events};
    };
    const auto a = replay(30), b = replay(60), c = replay(144);
    require(a == b && a == c, "same recorded timeline at30/60/144 identical world/RNG/events");
}
#ifdef MOORHUHN_WORLD_ASSET_TESTS
void game_assets(const std::filesystem::path& manifest) {
    const auto store = moorhuhn::assets::AssetStore::load(manifest);
    std::vector<AssetView> views;
    for (const auto& id : store.image_ids()) {
        views.push_back(store.image(id.value));
    }
    validate_world_assets(views);
    IndexedWorldAssets indexed(views);
    Hooks hooks;
    hooks.init = initialize_world;
    hooks.camera = camera_and_spawn_world;
    hooks.tick = advance_world;
    Simulation sim;
    static_cast<void>(sim.start_round({{0}, 0, 1}, hooks));
    for (std::uint32_t ms = 16, sequence = 1; ms <= 16000; ms += 16, ++sequence) {
        static_cast<void>(sim.update(step(ms, sequence, ms - 16, ScreenPoint{320, 240}), hooks));
        if (sequence % 60 != 0) {
            continue;
        }
        const auto state = sim.snapshot();
        const auto list = world_draw_list(state.round.world);
        const auto scene = indexed.compose(list);
        moorhuhn::render::Renderer renderer;
        const auto& rgba = renderer.render(list, views).pixels();
        require(scene.size() == 640 * 480 && rgba.size() == scene.size(), "real assets compose640x480");
        const auto candidates = world_hit_candidates(state.round.world, views);
        PersistentState selection;
        selection.selector_state.resize(65536);
        static_cast<void>(select_indexed_target(selection, {320, 240}, {scene, 640}, candidates));
        require(sim.snapshot() == state, "real asset presentation readonly");
    }
    require(store.image_metadata("leaf").active_frames == 19 && store.image_metadata("mask").stored_frames == 14, "Active and stored frame counts differ");
    std::cout << "Game assets: " + manifest.string() << '\n';
}
#endif
} // namespace

int main(int argc, char** argv) {
    try {
        motion_fixture();
        camera_rules();
        initialization_and_spawns();
        animation_and_lifecycle();
        composition();
        replay_transport();
#ifdef MOORHUHN_WORLD_ASSET_TESTS
        if (argc == 3 && std::string_view(argv[1]) == "--assets") {
            game_assets(argv[2]);
        } else {
            require(argc == 1, "Usage: world [--assets manifest]");
        }
#else
        static_cast<void>(argv);
        require(argc == 1, "synthetic tests accept no arguments");
#endif
        std::cout << "World checks: " << checks << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "World test failed: " << error.what() << '\n';
        return 1;
    }
}
