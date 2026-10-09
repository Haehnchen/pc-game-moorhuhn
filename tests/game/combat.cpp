#include "game/combat.hpp"

#include "../fixtures/commands.hpp"
#include "render/renderer.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace {
using namespace moorhuhn::contracts;
using namespace moorhuhn::game;
using moorhuhn::tests::CommandSequence;
using moorhuhn::tests::event_value;
std::size_t checks{};

void require(bool condition, std::string_view message) {
    ++checks;
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

template <class Function> void rejected(Function&& function, std::string_view message) {
    bool threw = false;
    try {
        function();
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    require(threw, message);
}

std::string read(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("Cannot read fixture: " + path.string());
    }
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

struct Sprite {
    std::int32_t width, height;
    std::vector<std::uint8_t> pixels;
    std::vector<RgbaPixel> rgba;
    std::vector<AtlasRect> frames;
    AssetView asset;

    Sprite(std::string name, std::int32_t w, std::int32_t h, std::int32_t count = 1, std::uint8_t fill = 18)
        : width(w), height(h), pixels(static_cast<std::size_t>(w) * h * count, fill), rgba(pixels.size(), RgbaPixel{fill, fill, fill, 255}) {
        for (std::int32_t i = 0; i < count; ++i) {
            frames.push_back({w * i, 0, w, h});
        }
        asset = {{std::move(name)}, w * count, h, static_cast<std::uint32_t>(w * count), frames, rgba, pixels, {}};
    }

    void set(std::uint32_t frame, std::int32_t x, std::int32_t y, std::uint8_t value) {
        const auto offset = static_cast<std::size_t>(y) * asset.row_stride + frame * width + x;
        pixels[offset] = value;
        rgba[offset] = {value, value, value, 255};
    }
};

Sprite fixture_sprite(const std::filesystem::path& fixtures) {
    std::istringstream data(read(fixtures / "synthetic-sprite.txt"));
    std::string name;
    std::int32_t width{}, height{}, count{};
    require(bool(data >> name >> width >> height >> count), "Bad sprite fixture");
    Sprite sprite(name, width, height, count);
    for (std::int32_t frame = 0; frame < count; ++frame) {
        for (std::int32_t y = 0; y < height; ++y) {
            for (std::int32_t x = 0; x < width; ++x) {
                unsigned value{};
                require(bool(data >> value) && value <= 255, "Missing sprite pixel");
                sprite.set(frame, x, y, static_cast<std::uint8_t>(value));
            }
        }
    }
    return sprite;
}

HitCandidate candidate(const Sprite& sprite, std::uint32_t slot = 5, float x = 100, float y = 100) {
    return {slot, 100 + slot, "test.sprite", 0, true, {sprite.asset.id, 0}, &sprite.asset, x, y, {0, 0}, false, false};
}

ScreenPoint cursor_for(std::int32_t x, std::int32_t y) {
    return {x - 16, y - 16};
}

std::vector<std::uint8_t> scene_at(ScreenPoint point, std::uint8_t value) {
    std::vector<std::uint8_t> scene(640 * 480, 5);
    scene[static_cast<std::size_t>(point.y) * 640 + point.x] = value;
    return scene;
}

std::vector<std::uint8_t> compose(std::span<const HitCandidate> candidates) {
    std::vector<const HitCandidate*> ordered;
    for (const auto& record : candidates) {
        if (record.active && record.image) {
            ordered.push_back(&record);
        }
    }
    std::ranges::sort(ordered, {}, [](const HitCandidate* record) {
        return record->slot;
    });
    std::vector<AssetView> assets;
    std::vector<DrawCommand> commands;
    for (const auto* record : ordered) {
        assets.push_back(*record->image);
        SpriteDraw draw;
        draw.frame = record->frame;
        draw.destination = {static_cast<std::int32_t>(record->screen_x) - record->anchor.x, static_cast<std::int32_t>(record->screen_y) - record->anchor.y};
        draw.transparent_index = 18;
        draw.mirror_x = record->mirror_x;
        draw.mirror_y = record->mirror_y;
        commands.emplace_back(std::move(draw));
    }
    moorhuhn::render::Renderer renderer;
    const auto& framebuffer = renderer.render(DrawList(std::move(commands)), assets, {{5, 5, 5, 255}});
    std::vector<std::uint8_t> result;
    result.reserve(framebuffer.pixels().size());
    // Synthetic grayscale palette makes renderer output bytes equal original indices.
    for (const auto& pixel : framebuffer.pixels()) {
        result.push_back(pixel.r);
    }
    return result;
}

struct Witness final : LoadedShotHandler {
    std::vector<std::uint32_t> loaded;
    std::vector<std::int32_t> remaining;
    std::vector<CombatResult> results;

    void loaded_shot(RoundContext& context, const CombatResult& result) override {
        loaded.push_back(context.round.magazine.loaded);
        remaining.push_back(context.step.remaining_before_ms);
        results.push_back(result);
        context.events.emit(PresentationEffect{"test.response", std::nullopt, result.point});
    }
};

struct Hooks final : RoundHooks {
    std::vector<HitCandidate> candidates;
    std::vector<std::uint8_t> scene;
    std::vector<CombatResult> results;
    std::vector<SelectionResult> selections;
    LoadedShotHandler* response{};
    bool select{};

    void initialize(RoundContext& context) override {
        initialize_combat(context);
    }

    void combat(RoundContext& context) override {
        results.push_back(process_combat(context, candidates, response));
    }

    void select_target(RoundContext& context) override {
        if (!select) {
            return;
        }
        const auto selection = select_indexed_target(context.persistent, context.persistent.cursor, {scene, 640}, candidates);
        for (const auto& position : selection.positions) {
            auto record = std::ranges::find(candidates, position.slot, &HitCandidate::slot);
            require(record != candidates.end() && record->id == position.id, "Quantization preserves candidate identity");
            record->screen_x = position.screen_x;
            record->screen_y = position.screen_y;
        }
        selections.push_back(selection);
    }
};

RecordedUpdate input(std::int64_t at, std::uint32_t clock, std::uint64_t sequence, std::int64_t begin, std::vector<InputControl> buttons, ScreenPoint cursor = {86, 86}) {
    InputFrame frame{sequence, {begin}, {at}, cursor, {}, false, true};
    std::uint64_t order = 0;
    for (const auto button : buttons) {
        frame.actions.push_back({{at}, ++order, button, InputEdge::pressed, cursor, {}});
    }
    return {{at}, clock, std::move(frame)};
}

struct TimelineStep {
    std::uint32_t clock;
    unsigned loaded;
    std::string action;
    int shell;
    std::vector<InputControl> buttons;
};

std::vector<TimelineStep> timeline(const std::filesystem::path& fixtures) {
    std::istringstream data(read(fixtures / "magazine-timeline.txt"));
    std::vector<TimelineStep> result;
    TimelineStep step;
    unsigned count{};
    while (data >> step.clock >> step.loaded >> step.action >> step.shell >> count) {
        step.buttons.clear();
        for (unsigned i = 0; i < count; ++i) {
            unsigned button{};
            require(bool(data >> button), "Missing timeline button");
            step.buttons.push_back(static_cast<InputControl>(button));
        }
        result.push_back(step);
    }
    require(!result.empty(), "Empty timeline");
    return result;
}

CombatAction action_name(std::string_view name) {
    if (name == "shot") {
        return CombatAction::shot;
    }
    if (name == "empty") {
        return CombatAction::empty;
    }
    if (name == "reloaded") {
        return CombatAction::reloaded;
    }
    return CombatAction::none;
}

void magazine_cases(const std::filesystem::path& fixtures) {
    const auto steps = timeline(fixtures);
    Hooks hooks;
    Witness response;
    hooks.response = &response;
    PersistentState initial;
    initial.cached_target_slot = -1;
    Simulation simulation(initial);
    (void)simulation.start_round({{0}, 0, 255}, hooks);
    require(simulation.snapshot().persistent.selector_state == std::vector<std::uint8_t>(65536, 0), "Scratch initializes once to zero");
    std::int64_t previous = 0;
    std::uint64_t sequence = 0;
    std::uint64_t event_sequence = simulation.snapshot().persistent.next_event_sequence - 1;
    for (const auto& step : steps) {
        const auto clock = step.clock;
        const auto at = static_cast<std::int64_t>(clock) * 1000 + static_cast<std::int64_t>(++sequence);
        std::vector<InputControl> buttons;
        for (const auto button : step.buttons) {
            buttons.push_back(button);
        }
        const auto before = hooks.results.size();
        const auto output = simulation.update(input(at, clock, sequence, previous, std::move(buttons)), hooks);
        const auto state = simulation.snapshot();
        require(state.round.magazine.loaded == step.loaded && !state.round.magazine.reloading, "Magazine timeline matches accepted rules");
        if (step.action == "ended") {
            require(hooks.results.size() == before && output.events.empty(), "Post-expiry input has no combat or audio events");
        } else {
            const auto& result = hooks.results.back();
            require(result.action == action_name(step.action), "Timeline action result");
            require(result.cached_slot == -1 && !result.hit, "Loaded miss preserves negative cache");
            require(result.shake == (result.action == CombatAction::shot ? 8U : 0U), "Shake impulse occurs only on loaded shot");
            if (step.shell >= 0) {
                require(result.ejected_shell == static_cast<unsigned>(step.shell), "Original shell ejection index");
            } else {
                require(!result.ejected_shell, "No shell ejection for empty/reload/ignored actions");
            }
            const auto expected = result.action == CombatAction::none ? 0U : result.action == CombatAction::shot ? 3U : 2U;
            const auto end_event = state.phase == Phase::ended ? 1U : 0U;
            require(output.events.size() == expected + end_event, "Combat events emitted once");
            if (result.action != CombatAction::none) {
                const auto sound = std::get<PlaySound>(output.events.front().payload).sound.value;
                require(sound == (result.action == CombatAction::shot ? "shoot22" : result.action == CombatAction::empty ? "empty22" : "reload22"), "Prepared event sound ID");
            }
        }
        for (const auto& event : output.events) {
            require(event.sequence > event_sequence, "Event sequences increase once");
            event_sequence = event.sequence;
        }
        const auto saved = simulation.snapshot();
        for (unsigned i = 0; i < 3; ++i) {
            (void)simulation.game_state();
            require(simulation.snapshot() == saved, "Reads cannot consume ammo or repeat events");
        }
        previous = at;
    }
    require(response.loaded[0] == 8 && response.loaded[7] == 1, "Response runs before shell consumption");
    require(response.remaining.back() == 89887 && simulation.snapshot().remaining_ms == 0, "Terminal shot handler runs before expiry subtraction");
    const auto rng = simulation.snapshot().random;
    require(rng.seed == 255 && rng.draws == 0, "Generic combat and selection draw no random numbers");
    const auto carrier = simulation.snapshot().persistent.selector_state;
    (void)simulation.start_round({{previous + 1}, 42, 511}, hooks);
    require(simulation.snapshot().round.magazine.loaded == 8 && simulation.snapshot().persistent.selector_state == carrier, "Restart resets magazine without clearing scratch");
}

void geometry_cases(const std::filesystem::path& fixtures) {
    auto sprite = fixture_sprite(fixtures);
    auto record = candidate(sprite, 5, 100.75F, 100.99F);
    auto scene = compose(std::span{&record, 1});
    PersistentState state;
    auto result = select_indexed_target(state, cursor_for(102, 102), {scene, 640}, std::span{&record, 1});
    require(result.hit && result.hit->target.id == 105 && result.hit->target.frame == FrameId{sprite.asset.id, 0}, "Visible original index and frame ID");
    require(result.hit->frame_pixel == ScreenPoint{2, 2} && result.hit->candidate_index == 7 && result.hit->scene_index == 7, "Point maps to original frame pixel");
    require(result.positions == std::vector<QuantizedPosition>{{5, 105, 100, 100}}, "Scratch rendering reports binary32 screen quantization");
    for (const auto point : std::array<ScreenPoint, 4>{{{100, 102}, {108, 102}, {102, 100}, {102, 108}}}) {
        const auto before = state.selector_state;
        result = select_indexed_target(state, cursor_for(point.x, point.y), {scene, 640}, std::span{&record, 1});
        require(!result.hit && result.positions.empty() && state.selector_state == before, "Strict rectangle rejects all four edges before scratch writes");
    }
    result = select_indexed_target(state, cursor_for(101, 101), {scene, 640}, std::span{&record, 1});
    require(!result.hit, "Transparent interior edge skips index18");
    result = select_indexed_target(state, cursor_for(103, 103), {scene, 640}, std::span{&record, 1});
    require(!result.hit, "Transparent mask hole remains unhittable in cleared prefix");
    auto zero_scene = scene_at({104, 102}, 0);
    result = select_indexed_target(state, cursor_for(104, 102), {zero_scene, 640}, std::span{&record, 1});
    require(result.hit && result.hit->candidate_index == 0, "Sprite palette index0 is opaque");
    auto key_scene = scene_at({103, 103}, 18);
    result = select_indexed_target(state, cursor_for(103, 103), {key_scene, 640}, std::span{&record, 1});
    require(!result.hit, "Index18 rejects even when composed byte equals18");
    record.frame.index = 1;
    scene = compose(std::span{&record, 1});
    result = select_indexed_target(state, cursor_for(102, 102), {scene, 640}, std::span{&record, 1});
    require(!result.hit, "Frame change exposes transparent hole");
    result = select_indexed_target(state, cursor_for(103, 103), {scene, 640}, std::span{&record, 1});
    require(result.hit && result.hit->target.frame.index == 1 && result.hit->candidate_index == 9, "New frame uses its own indices");
    record.frame.index = 0;
    record.anchor = {1, 2};
    scene = compose(std::span{&record, 1});
    result = select_indexed_target(state, cursor_for(101, 100), {scene, 640}, std::span{&record, 1});
    require(result.hit && result.hit->frame_pixel == ScreenPoint{2, 2}, "Explicit anchor matches draw origin");
    record.anchor = {0, 0};
    for (const auto& [mirror_x, mirror_y] : std::array<std::pair<bool, bool>, 3>{{{true, false}, {false, true}, {true, true}}}) {
        record.mirror_x = mirror_x;
        record.mirror_y = mirror_y;
        scene = compose(std::span{&record, 1});
        const auto point = ScreenPoint{100 + (mirror_x ? 5 : 2), 100 + (mirror_y ? 5 : 2)};
        result = select_indexed_target(state, cursor_for(point.x, point.y), {scene, 640}, std::span{&record, 1});
        require(result.hit && result.hit->frame_pixel == ScreenPoint{2, 2} && result.hit->candidate_index == 7, "Mirroring matches renderer and original frame coordinates");
    }
    record.mirror_x = record.mirror_y = false;
    record.screen_x = 1640.75F - static_cast<std::int32_t>(640 * 2.2F);
    scene = compose(std::span{&record, 1});
    result = select_indexed_target(state, cursor_for(234, 102), {scene, 640}, std::span{&record, 1});
    require(result.hit && result.positions[0].screen_x == 232, "Projected camera offset is not applied twice");
    for (auto& pixel : sprite.rgba) {
        pixel.a = 0;
    }
    auto index_scene = scene_at({234, 102}, 7);
    result = select_indexed_target(state, cursor_for(234, 102), {index_scene, 640}, std::span{&record, 1});
    require(result.hit.has_value(), "Original indices determine hits independently of prepared alpha");
    Sprite negative("combat-negative-position", 30, 8);
    negative.set(0, 26, 2, 7);
    auto left = candidate(negative, 5, -10.75F, 100.99F);
    scene = compose(std::span{&left, 1});
    result = select_indexed_target(state, cursor_for(16, 102), {scene, 640}, std::span{&left, 1});
    require(
        result.hit && result.hit->frame_pixel == ScreenPoint{26, 2} && result.positions[0].screen_x == -10, "Negative projected positions truncate towardzero rather than floor");
}

void occlusion_and_state(const std::filesystem::path& fixtures) {
    auto sprite = fixture_sprite(fixtures);
    Sprite other("combat-other", 8, 8);
    other.set(0, 2, 2, 11);
    auto first = candidate(sprite, 4), second = candidate(other, 9);
    std::array records{second, first};
    PersistentState state;
    auto scene = compose(records);
    auto result = select_indexed_target(state, cursor_for(102, 102), {scene, 640}, records);
    require(result.hit && result.hit->target.slot == 9 && result.positions.size() == 2, "Different-index foreground occlusion rejects lower slot");
    other.set(0, 2, 2, 7);
    scene = compose(records);
    result = select_indexed_target(state, cursor_for(102, 102), {scene, 640}, records);
    require(result.hit && result.hit->target.slot == 4 && result.positions.size() == 1, "Equal-index occluder accepts first ascending slot");
    scene[102 * 640 + 102] = 99;
    result = select_indexed_target(state, cursor_for(102, 102), {scene, 640}, records);
    require(!result.hit && state.cached_target_slot == -1, "Different popup/foreground byte blocks all candidates");
    scene[102 * 640 + 102] = 7;
    for (const auto excluded : {20, 32, 56}) {
        records[0].state = excluded;
        records[1].state = excluded;
        result = select_indexed_target(state, cursor_for(102, 102), {scene, 640}, records);
        require(!result.hit && result.positions.empty(), "State20andabove excluded from fresh selection");
    }
    records[0].state = 0;
    records[1].state = -1;
    records[1].active = false;
    result = select_indexed_target(state, cursor_for(102, 102), {scene, 640}, records);
    require(result.hit && result.hit->target.slot == 9, "Inactive slot excluded");
    records[1].active = true;
    result = select_indexed_target(state, cursor_for(102, 102), {scene, 640}, records);
    require(result.hit && result.hit->target.slot == 4, "Eligibility uses signed state less than20");
    std::vector<std::uint8_t> padded(650 * 480, 5);
    padded[102 * 650 + 102] = 7;
    result = select_indexed_target(state, cursor_for(102, 102), {padded, 650}, records);
    require(result.hit && result.hit->target.slot == 4, "Composed scene stride is explicit");
    result = select_indexed_target(state, {100, 464}, {scene, 640}, records);
    require(!result.hit, "Original cursorY463 guard prevents selection below playfield");
}

void scratch_tail() {
    Sprite small("combat-width30", 30, 20);
    auto small_record = candidate(small, 5);
    const auto point = ScreenPoint{101, 119};
    auto zero_scene = scene_at(point, 0);
    PersistentState state;
    auto result = select_indexed_target(state, cursor_for(point.x, point.y), {zero_scene, 640}, std::span{&small_record, 1});
    require(result.hit && result.hit->candidate_index == 0 && result.hit->frame_pixel == ScreenPoint{1, 19}, "Initial-zero transparent tail can be accepted");
    require(state.selector_state[29] == 18 && state.selector_state[559] == 18 && state.selector_state[560] == 0 && state.selector_state[599] == 0,
        "Width30height20 clears contiguous560bytes, retains final40");
    auto prefix_scene = scene_at({101, 118}, 0);
    result = select_indexed_target(state, cursor_for(101, 118), {prefix_scene, 640}, std::span{&small_record, 1});
    require(!result.hit, "Transparent pixel in cleared prefix is rejected");
    Sprite wide("combat-width32", 32, 20, 1, 7);
    wide.set(0, 1, 19, 18);
    auto wide_record = candidate(wide, 4);
    auto seven_scene = scene_at(point, 7);
    std::array records{small_record, wide_record};
    result = select_indexed_target(state, cursor_for(point.x, point.y), {seven_scene, 640}, records);
    require(result.hit && result.hit->target.slot == 5 && result.hit->candidate_index == 7 && result.positions.size() == 2,
        "Opaque writes from preceding candidate persist through later transparent tail");
    require(state.selector_state[559] == 18 && state.selector_state[560] == 7 && state.selector_state[599] == 7, "No per-row tail clear or transparency overwrite");
    const auto saved = state.selector_state;
    Simulation simulation(state);
    Hooks hooks;
    (void)simulation.start_round({{0}, 0, 0}, hooks);
    require(simulation.snapshot().persistent.selector_state == saved && simulation.snapshot().persistent.cached_target_slot == 5,
        "Round initialization preserves scratch and cached slot");
}

void cached_slot_cases(const std::filesystem::path& fixtures) {
    auto sprite = fixture_sprite(fixtures);
    Hooks hooks;
    auto stale = candidate(sprite, 5);
    stale.active = false;
    stale.state = 32;
    auto next = candidate(sprite, 9, 200, 100);
    hooks.candidates = {stale, next};
    hooks.scene = compose(hooks.candidates);
    hooks.select = true;
    PersistentState initial;
    initial.cached_target_slot = 5;
    Simulation simulation(initial);
    (void)simulation.start_round({{0}, 0, 0}, hooks);
    (void)simulation.update(input(1000, 1, 1, 0, {InputControl::primary}, cursor_for(202, 102)), hooks);
    require(hooks.results[0].cached_slot == 5 && hooks.results[0].hit && hooks.results[0].hit->id == 105 && hooks.results[0].hit->state == 32,
        "Loaded shot uses inactive/falling old slot without fresh validation");
    require(simulation.snapshot().persistent.cached_target_slot == 9, "Later selection replaces old cache");
    hooks.candidates[1].id = 999;
    hooks.candidates[1].kind = "test.reused";
    (void)simulation.update(input(2000, 2, 2, 1000, {InputControl::primary}, cursor_for(102, 102)), hooks);
    require(hooks.results[1].hit && hooks.results[1].hit->id == 999 && hooks.results[1].hit->kind == "test.reused", "Cached slot resolves current reused record identity");
    require(hooks.results[1].point == ScreenPoint{102, 102}, "Result location is current dispatch point, not a new target test");
    Hooks absent;
    Simulation carry(initial);
    (void)carry.start_round({{0}, 0, 0}, absent);
    (void)carry.update(input(1000, 1, 1, 0, {InputControl::primary}), absent);
    require(absent.results[0].cached_slot == 5 && !absent.results[0].hit && absent.results[0].action == CombatAction::shot,
        "Missing current identity does not turn cached slot into special-rectangle miss");
    Hooks outside;
    Simulation unmapped(initial);
    (void)unmapped.start_round({{0}, 0, 0}, outside);
    auto unnormalized = input(1000, 1, 1, 0, {InputControl::primary});
    unnormalized.input.pointer.reset();
    unnormalized.input.actions[0].pointer.reset();
    (void)unmapped.update(unnormalized, outside);
    require(outside.results[0].action == CombatAction::none && unmapped.snapshot().round.magazine.loaded == 8, "Outside click cannot consume ammo or hit old cache");
}

void invalid_candidates(const std::filesystem::path& fixtures) {
    auto sprite = fixture_sprite(fixtures);
    auto record = candidate(sprite);
    auto scene = scene_at({102, 102}, 7);
    PersistentState state;
    auto select = [&] {
        (void)select_indexed_target(state, {86, 86}, {scene, 640}, std::span{&record, 1});
    };
    record.frame.index = 2;
    rejected(select, "Invalid frame rejected");
    record.frame.index = 0;
    record.frame.image = {"wrong"};
    rejected(select, "Frame image mismatch rejected");
    record.frame.image = sprite.asset.id;
    record.screen_x = std::numeric_limits<float>::quiet_NaN();
    rejected(select, "NaN projected position rejected");
    record.screen_x = std::numeric_limits<float>::infinity();
    rejected(select, "Infinite projected position rejected");
    record.screen_x = 2147483648.0F;
    rejected(select, "Out-of-range projected integer rejected");
    record.screen_x = 100;
    record.slot = 32;
    rejected(select, "Invalid slot rejected");
    record.slot = 5;
    std::array duplicate{record, record};
    rejected(
        [&] {
            (void)select_indexed_target(state, {86, 86}, {scene, 640}, duplicate);
        },
        "Duplicate slots rejected");
    auto saved = sprite.asset.palette_indices;
    sprite.asset.palette_indices = {};
    rejected(select, "Missing original indices rejected");
    sprite.asset.palette_indices = saved;
    state.selector_state = {1};
    rejected(select, "Partial selector carrier rejected");
    state.selector_state.clear();
    rejected(
        [&] {
            (void)select_indexed_target(state, {86, 86}, {{}, 640}, std::span{&record, 1});
        },
        "Truncated scene rejected");
    Sprite oversized("combat-oversized", 257, 256);
    auto large = candidate(oversized);
    rejected(
        [&] {
            (void)select_indexed_target(state, {86, 86}, {scene, 640}, std::span{&large, 1});
        },
        "Oversized scratch frame rejected");
}

struct ReplayResult {
    std::vector<Snapshot> snapshots;
    std::vector<CombatResult> combat;
    std::vector<SelectionResult> selections;
    std::vector<std::string> events;
    bool operator==(const ReplayResult&) const = default;
};

ReplayResult replay_schedule(const CommandSequence& replay, const Sprite& sprite, const std::vector<std::int64_t>& intervals) {
    Hooks hooks;
    hooks.candidates.push_back(candidate(sprite));
    hooks.scene = compose(hooks.candidates);
    hooks.select = true;
    Simulation simulation(replay.initial);
    ReplayResult result;
    std::size_t next = 0, tick = 0;
    std::int64_t presentation = 0;
    while (next < replay.commands.size()) {
        while (next < replay.commands.size()
               && std::visit(
                      [](const auto& command) {
                          return command.at.microseconds;
                      },
                      replay.commands[next])
                      <= presentation) {
            const auto output = moorhuhn::tests::apply(simulation, replay.commands[next++], hooks);
            for (const auto& event : output.events) {
                result.events.push_back(event_value(event));
            }
            result.snapshots.push_back(simulation.snapshot());
        }
        const auto saved = simulation.snapshot();
        (void)simulation.game_state();
        require(simulation.snapshot() == saved, "Presentation is read-only");
        presentation += intervals[tick++ % intervals.size()];
    }
    result.combat = hooks.results;
    result.selections = hooks.selections;
    return result;
}

void replay_cases(const std::filesystem::path& fixtures) {
    auto sprite = fixture_sprite(fixtures);
    const auto steps = timeline(fixtures);
    CommandSequence replay;
    replay.initial.cached_target_slot = 5;
    replay.commands.push_back(RoundStart{{0}, 0, 255});
    std::int64_t previous = 0;
    std::uint64_t sequence = 0;
    for (const auto& step : steps) {
        const auto clock = step.clock;
        const auto at = static_cast<std::int64_t>(clock) * 1000 + static_cast<std::int64_t>(++sequence);
        std::vector<InputControl> buttons;
        for (const auto button : step.buttons) {
            buttons.push_back(button);
        }
        replay.commands.emplace_back(input(at, clock, sequence, previous, std::move(buttons)));
        previous = at;
    }
    replay.commands.push_back(RoundStart{{previous + 1}, 42, 511});
    const auto baseline = replay_schedule(replay, sprite, {33333, 33333, 33334});
    for (const auto& intervals : std::vector<std::vector<std::int64_t>>{{16666, 16667, 16667}, {6944, 6944, 6945}, {1, 70000, 3, 19000, 250000, 333}}) {
        require(replay_schedule(replay, sprite, intervals) == baseline, "30/60/144/irregular schedules preserve hits/ammo/scratch/events");
    }
}
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 3 || std::string_view(argv[1]) != "--fixtures") {
            throw std::invalid_argument("Usage: combat --fixtures PATH");
        }
        const std::filesystem::path fixtures = argv[2];
        magazine_cases(fixtures);
        geometry_cases(fixtures);
        occlusion_and_state(fixtures);
        scratch_tail();
        cached_slot_cases(fixtures);
        invalid_candidates(fixtures);
        replay_cases(fixtures);
        std::cout << "Combat: " << checks << " checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Combat: " << error.what() << '\n';
        return 1;
    }
}
