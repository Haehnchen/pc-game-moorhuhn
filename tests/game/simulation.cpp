#include "game/simulation.hpp"

#include "fixtures/simulation/commands.hpp"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <filesystem>
#include <format>
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
using moorhuhn::tests::Command;
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
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        throw std::runtime_error("Cannot read fixture: " + path.string());
    }
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

RecordedUpdate update(std::int64_t at_us, std::uint32_t clock, std::uint64_t sequence, std::int64_t begin_us = 0, std::vector<TimedAction> actions = {}) {
    return {{at_us}, clock, {sequence, {begin_us}, {at_us}, std::nullopt, std::move(actions), false, true}};
}

TimedAction action(std::int64_t at, std::uint64_t order, InputControl control, InputEdge edge = InputEdge::pressed, std::optional<ScreenPoint> pointer = std::nullopt) {
    if (!pointer && (control == InputControl::primary || control == InputControl::secondary)) {
        pointer = ScreenPoint{320, 240};
    }
    return {{at}, order, control, edge, pointer};
}

struct ProbeHooks final : RoundHooks {
    std::vector<std::string> order;
    std::vector<std::uint32_t> masks;
    std::vector<std::int32_t> before;
    std::vector<std::int32_t> after;
    std::vector<std::int32_t> targets;
    std::vector<bool> spawn;

    void initialize(RoundContext&) override {
        order.push_back("initialize");
    }

    void camera_and_spawn(RoundContext& context) override {
        order.push_back("camera/spawn");
        spawn.push_back(context.step.spawn_due);
    }

    void combat(RoundContext& context) override {
        order.push_back("combat");
        masks.push_back(context.step.action_mask);
        before.push_back(context.step.remaining_before_ms);
        targets.push_back(context.persistent.cached_target_slot);
    }

    void timer(RoundContext&) override {
        order.push_back("timer");
    }

    void update_world(RoundContext& context) override {
        order.push_back("world");
        after.push_back(context.step.remaining_after_ms);
    }

    void select_target(RoundContext& context) override {
        order.push_back("selection");
        context.persistent.cached_target_slot = 11;
    }
};

void random_vectors(const std::filesystem::path& fixtures) {
    std::istringstream data(read(fixtures / "random-vectors.txt"));
    std::size_t draws = 0;
    std::uint32_t seed{};
    unsigned cursor{}, count{};
    std::string expected_state;
    while (data >> seed >> cursor >> expected_state >> count) {
        RandomGenerator random(seed);
        const auto initial = random.snapshot();
        require(initial.seed == seed && initial.cursor == (seed & 255) && initial.draws == 0, "Explicit RNG seed");
        for (unsigned i = 0; i < count; ++i) {
            std::string expected;
            require(bool(data >> expected), "Missing RNG vector");
            require(random.draw() == std::stoul(expected, nullptr, 16), "Independent RNG draw");
            ++draws;
        }
        const auto& result = random.snapshot();
        require(result.cursor == cursor, "Independent RNG cursor");
        require(result.draws == count, "RNG draw count");
        std::string state_hex;
        for (auto byte : result.bytes) {
            state_hex += std::format("{:02x}", byte);
        }
        require(state_hex == expected_state, "Independent 260-byte state");
        random.reseed(seed);
        require(random.snapshot() == initial, "Reseed restores initial state");
    }
    require(draws == 80, "All eighty independent draws checked");
    for (std::uint32_t seed : {0U, 1U, 255U, 100000U}) {
        RandomGenerator first(seed), second(seed + 256);
        require(first.snapshot().bytes == second.snapshot().bytes && first.snapshot().cursor == second.snapshot().cursor, "Seed plus256 has identical random state");
        for (unsigned i = 0; i < 300; ++i) {
            require(first.draw() == second.draw(), "Seed plus256 sequence equivalence");
        }
    }
}

void boundary_and_restart() {
    PersistentState carry;
    carry.cached_target_slot = 7;
    carry.shield_variant = 2;
    carry.selector_state.assign(65536, 18);
    carry.selector_state.back() = 99;
    carry.next_round_id = 3;
    carry.next_event_sequence = 40;
    Simulation simulation(carry);
    ProbeHooks hooks;
    const auto start = simulation.start_round({{0}, 100, 255}, hooks);
    require(start.events.size() == 1 && start.events[0].sequence == 40, "Round start allocates one event");
    auto state = simulation.snapshot();
    require(state.phase == Phase::active && state.remaining_ms == 90000 && state.elapsed_ms == 0, "Round start clock/budget");
    require(state.round_id == 3 && state.round.magazine == MagazineState{8, 8, false}, "Round start identity/magazine");
    require(state.round.score == 0 && state.round.camera == WorldPoint{640, 0} && state.round.objects.empty(), "Round start core fields");
    require(state.persistent.cached_target_slot == 7 && state.persistent.selector_state == carry.selector_state, "Initial selector carry-in");
    (void)simulation.update(update(89999000, 90099, 1, 0, {action(89999000, 1, InputControl::primary)}), hooks);
    require(simulation.snapshot().phase == Phase::active && simulation.snapshot().remaining_ms == 1, "89999ms remains active");
    require(hooks.before.back() == 90000 && hooks.targets.back() == 7, "First combat consumes old cached target before budget");
    const auto end = simulation.update(update(90000000, 90100, 2, 89999000, {action(90000000, 1, InputControl::primary)}), hooks);
    state = simulation.snapshot();
    require(state.phase == Phase::ended && state.end_reason == EndReason::expired && state.remaining_ms == 0, "90000ms signed expiry");
    require(hooks.before.back() == 1 && hooks.after.back() == 0 && hooks.masks.back() == 1 && hooks.targets.back() == 11,
        "Terminal loaded-action stage precedes expiry; selection follows world");
    require(hooks.order == std::vector<std::string>{"initialize", "camera/spawn", "combat", "timer", "world", "selection", "camera/spawn", "combat", "timer", "world", "selection"},
        "Ordered round stages");
    require(end.events.size() == 1 && end.events[0].sequence == 41, "Expiry emits once");
    const auto hook_count = hooks.order.size();
    const auto terminal_rng = state.random;
    const auto ignored = simulation.update(update(90001000, 90101, 3, 90000000, {action(90001000, 1, InputControl::primary)}), hooks);
    require(ignored.events.empty() && hooks.order.size() == hook_count, "End input cannot invoke game hooks or reemit expiry");
    require(simulation.snapshot().random == terminal_rng && simulation.snapshot().update_index == 2, "End input cannot advance RNG or simulation");
    const auto observed = simulation.snapshot();
    auto owned = observed;
    owned.round.score = 999;
    owned.persistent.selector_state.back() = 0;
    require(simulation.snapshot() == observed, "Owned immutable observations do not alter simulation");
    (void)simulation.start_round({{90002000}, 0xffffffffU, 511}, hooks);
    state = simulation.snapshot();
    require(state.round_id == 4 && state.phase == Phase::active && state.elapsed_ms == 0 && state.update_index == 0, "Explicit restart resets round clocks");
    require(state.persistent.cached_target_slot == 11 && state.persistent.selector_state == carry.selector_state, "Restart preserves selector cache/carrier");
    require(state.persistent.shield_variant == 2, "Restart preserves shield strip variant");
    require(state.persistent.last_input_sequence == 3 && state.persistent.next_event_sequence == 43, "Restart preserves input/event chronology");
    require(state.random.seed == 511 && state.random.draws == 0, "Restart reseeds explicitly");
    const auto projected = simulation.game_state();
    require(
        projected.round.id == state.round_id && projected.random.words.size() == 68 && projected.random.words[65] == 255, "Shared projection includes all RNG bytes and cursor");
    require(projected.random.words[66] == 511 && projected.random.words[67] == 0 && projected.random.algorithm == random_algorithm,
        "Shared RNG projection includes seed/draw count/version");
}

void input_batches() {
    Simulation simulation;
    ProbeHooks hooks;
    (void)simulation.start_round({{0}, 0, 0}, hooks);
    auto batch = update(1000, 1, 1, 0, {action(1000, 1, InputControl::primary), action(1000, 2, InputControl::primary), action(1000, 3, InputControl::secondary)});
    batch.input.pointer = ScreenPoint{639, 479};
    (void)simulation.update(batch, hooks);
    require(hooks.masks.back() == 3, "Down edges aggregate OR mask; both buttons cancel exact1/2 dispatch");
    require(simulation.snapshot().persistent.cursor == ScreenPoint{608, 448}, "Original cursor clamp");
    (void)simulation.update(update(2000, 2, 2, 1000), hooks);
    require(hooks.masks.back() == 0, "Held primary produces no implicit new action");
    (void)simulation.update(
        update(3000, 3, 3, 2000, {action(3000, 1, InputControl::primary, InputEdge::released), action(3000, 2, InputControl::secondary, InputEdge::released)}), hooks);
    require(hooks.masks.back() == 0, "Release edges add no action mask");
    (void)simulation.update(update(4000, 4, 4, 3000, {action(4000, 1, InputControl::cancel), action(4000, 2, InputControl::cancel, InputEdge::released)}), hooks);
    require(simulation.snapshot().phase == Phase::active, "Cancel final held byte follows message chronology");
    auto focus = update(5000, 5, 5, 4000,
        {action(5000, 1, InputControl::left), action(5000, 2, InputControl::cancel), action(5000, 3, InputControl::primary),
            action(5000, 4, InputControl::focus, InputEdge::released), action(5000, 5, InputControl::focus)});
    (void)simulation.update(focus, hooks);
    const auto state = simulation.snapshot();
    require(state.phase == Phase::active && state.persistent.focused, "Loss/regain in one poll preserves focus chronology");
    require(!state.persistent.held[3] && !state.persistent.held[4] && state.persistent.held[0], "Focus loss clears keyboard but preserves mouse queue/held transport");
    require(hooks.masks.back() == 1 && state.remaining_ms == 89995, "Focus loss does not discard mouse action or time");
    auto unfocused = update(1005000, 1005, 6, 5000, {action(5001, 1, InputControl::focus, InputEdge::released)});
    unfocused.input.focused = false;
    (void)simulation.update(unfocused, hooks);
    require(!simulation.snapshot().persistent.focused && simulation.snapshot().remaining_ms == 88995, "No inferred pause or long-gap clamp on focus loss");
    const auto before = simulation.snapshot();
    auto invalid = update(1006000, 1006, 7, 1005000);
    invalid.input.focused = true;
    rejected(
        [&] {
            (void)simulation.update(invalid, hooks);
        },
        "Focus final bool needs an edge");
    require(simulation.snapshot() == before, "Invalid input is rejected before mutation");
    auto end = update(1006000, 1006, 7, 1005000, {action(1006000, 1, InputControl::focus), action(1006000, 2, InputControl::cancel)});
    (void)simulation.update(end, hooks);
    require(simulation.snapshot().end_reason == EndReason::cancelled, "Held cancel ends after current stages");
    auto quit = update(1007000, 1007, 8, 1006000);
    quit.input.quit_requested = true;
    const auto output = simulation.update(quit, hooks);
    require(simulation.snapshot().phase == Phase::quit && output.events.size() == 1, "Quit after end emits session event once");
    rejected(
        [&] {
            (void)simulation.start_round({{1008000}, 1008, 0}, hooks);
        },
        "Quit cannot restart a round");
    Simulation outside;
    ProbeHooks mapped;
    (void)outside.start_round({{0}, 0, 0}, mapped);
    (void)outside.update(update(1000, 1, 1, 0, {{{1000}, 1, InputControl::primary, InputEdge::pressed, std::nullopt}}), mapped);
    require(mapped.masks.back() == 0, "Unmapped outside click cannot hit previous cached target");
    (void)outside.update(update(2000, 2, 2, 1000, {action(2000, 1, InputControl::primary), {{2000}, 2, InputControl::secondary, InputEdge::pressed, std::nullopt}}), mapped);
    require(mapped.masks.back() == 1, "Outside action cannot cancel an inside action in the same batch");
}

void clock_vectors(const std::filesystem::path& fixtures) {
    RoundHooks hooks;
    Simulation wrap;
    (void)wrap.start_round({{0}, 0xfffffffeU, 0}, hooks);
    (void)wrap.update(update(1000, 1, 1), hooks);
    require(wrap.snapshot().elapsed_ms == 3 && wrap.snapshot().remaining_ms == 89997, "Clock subtraction wraps as uint32");
    std::istringstream vectors(read(fixtures / "clock-vectors.txt"));
    std::uint32_t delta{}, bits{}, elapsed{};
    std::int32_t remaining{};
    bool expires{};
    std::int64_t rounded_dt{};
    while (vectors >> delta >> bits >> elapsed >> remaining >> expires >> rounded_dt) {
        Simulation simulation;
        (void)simulation.start_round({{0}, 0, 0}, hooks);
        (void)simulation.update(update(1000, delta, 1), hooks);
        const auto state = simulation.snapshot();
        require(state.delta_bits == bits, "Binary32 delta barrier");
        require(state.elapsed_ms == elapsed, "Elapsed low32 conversion");
        require(state.remaining_ms == remaining, "Remaining signed low32 conversion");
        require((state.phase == Phase::ended) == expires, "Large-gap signed expiry");
        require(simulation.game_state().elapsed.microseconds == rounded_dt * 1000, "Transport projection keeps full delta rather than elapsed wrap");
    }
    for (const auto& [raw, rounded] : {std::pair{16777217U, 16777216U}, std::pair{16777219U, 16777220U}}) {
        Simulation simulation;
        (void)simulation.start_round({{0}, 0, 0}, hooks);
        (void)simulation.update(update(1, raw, 1), hooks);
        require(simulation.snapshot().delta_bits == std::bit_cast<std::uint32_t>(static_cast<float>(rounded)), "Binary32 halfway rounds to even");
    }
    Simulation spawn;
    ProbeHooks probe;
    (void)spawn.start_round({{0}, 0, 0}, probe);
    (void)spawn.update(update(750000, 750, 1), probe);
    require(!probe.spawn.back() && spawn.snapshot().spawn_elapsed_ms == 750, "750ms does not request spawn");
    (void)spawn.update(update(751000, 751, 2, 750000), probe);
    require(probe.spawn.back() && spawn.snapshot().spawn_elapsed_ms == 0, "751ms requests one spawn and resets accumulator");
    (void)spawn.update(update(10751000, 10751, 3, 751000), probe);
    require(probe.spawn.size() == 3 && probe.spawn.back() && spawn.snapshot().spawn_elapsed_ms == 0, "Large spawn gap requests only one; no catch-up");
}

void timer_stage() {
    struct Hooks final : RoundHooks {
        std::int32_t remaining{};

        void timer(RoundContext& context) override {
            remaining = context.step.remaining_after_ms;
            context.events.emit(PlaySound{{"test.timer"}});
        }

        void update_world(RoundContext& context) override {
            require(context.step.remaining_after_ms == remaining, "World sees the timer's signed budget");
            context.events.emit(PresentationEffect{"test.world", std::nullopt, std::nullopt});
        }

        void select_target(RoundContext& context) override {
            context.events.emit(PresentationEffect{"test.selection", std::nullopt, std::nullopt});
        }
    };

    // Original 407c73 caches seconds after pushes; it does not write the budget.
    for (const auto& [delta, remaining] : {std::pair{90999U, -999}, std::pair{91000U, -1000}, std::pair{120000U, -30000}}) {
        Simulation simulation;
        Hooks hooks;
        (void)simulation.start_round({{0}, 0, 0}, hooks);
        const auto output = simulation.update(update(static_cast<std::int64_t>(delta) * 1000, delta, 1), hooks);
        require(simulation.snapshot().remaining_ms == remaining && hooks.remaining == remaining, "Timer and terminal snapshot preserve long-gap signed remaining");
        require(simulation.snapshot().phase == Phase::ended && simulation.snapshot().end_reason == EndReason::expired, "Long-gap timer completes the terminal loop");
        require(output.events.size() == 4 && std::get<PlaySound>(output.events[0].payload).sound.value == "test.timer"
                    && std::get<PresentationEffect>(output.events[1].payload).name == "test.world"
                    && std::get<PresentationEffect>(output.events[2].payload).name == "test.selection"
                    && std::get<PresentationEffect>(output.events[3].payload).name == "round.ended",
            "Timer event precedes world, selection and terminal event");
    }
}

// Synthetic hooks expose stage, RNG and ID ordering.
struct ReplayHooks final : RoundHooks {
    void initialize(RoundContext& context) override {
        context.round.objects.push_back({context.events.allocate_object(), "test", {0, 0}, {{"test"}, 0}, {0}});
        (void)context.random.draw();
        if (context.persistent.selector_state.empty()) {
            context.persistent.selector_state.assign(65536, 0);
        }
        context.events.emit(PresentationEffect{"test.initialize", context.round.objects.front().id, std::nullopt});
    }

    void camera_and_spawn(RoundContext& context) override {
        if (context.persistent.held[4]) {
            context.round.camera.x -= 4;
        }
        if (context.step.spawn_due) {
            const auto id = context.events.allocate_object();
            context.round.objects.push_back({id, "test", {static_cast<std::int32_t>(context.random.draw() & 255), 0}, {{"test"}, 0}, {0}});
            context.events.emit(PresentationEffect{"test.spawn-request", id, std::nullopt});
        }
    }

    void combat(RoundContext& context) override {
        if (context.step.action_mask == 1) {
            context.round.score += static_cast<std::int32_t>(context.random.draw() & 7);
            context.events.emit(PresentationEffect{"test.action", std::nullopt, context.persistent.cursor});
        }
    }

    void update_world(RoundContext& context) override {
        for (auto& object : context.round.objects) {
            ++object.frame.index;
            object.animation_time.microseconds += static_cast<std::int64_t>(context.step.delta_ms) * 1000;
        }
    }

    void select_target(RoundContext& context) override {
        context.persistent.cached_target_slot = (context.persistent.cached_target_slot + 1) & 31;
        ++context.persistent.selector_state.back();
    }
};

struct ReplayResult {
    std::vector<Snapshot> snapshots;
    std::vector<std::string> events;
    bool operator==(const ReplayResult&) const = default;
};

std::int64_t command_time(const Command& command) {
    return std::visit(
        [](const auto& value) {
            return value.at.microseconds;
        },
        command);
}

ReplayResult presented_replay(const CommandSequence& replay, const std::vector<std::int64_t>& intervals) {
    Simulation simulation(replay.initial);
    ReplayHooks hooks;
    ReplayResult result;
    std::size_t next = 0, tick = 0;
    std::int64_t presentation = 0;
    std::uint64_t last_event = 0;
    while (next < replay.commands.size()) {
        // Consume complete recorded boundaries; presentation never creates updates.
        while (next < replay.commands.size() && command_time(replay.commands[next]) <= presentation) {
            const auto output = moorhuhn::tests::apply(simulation, replay.commands[next++], hooks);
            for (const auto& event : output.events) {
                require(event.sequence > last_event && event.sequence != 0, "Events keep session order across restart");
                last_event = event.sequence;
                result.events.push_back(event_value(event));
            }
            result.snapshots.push_back(simulation.snapshot());
        }
        const auto before = simulation.snapshot();
        const auto shared = simulation.game_state();
        require(simulation.snapshot() == before && simulation.game_state() == shared, "Repeated presentation observes only");
        presentation += intervals[tick++ % intervals.size()];
    }
    return result;
}

void presentation_tests() {
    const auto sequence = moorhuhn::tests::simulation_commands();
    const auto baseline = presented_replay(sequence, {33333, 33333, 33334});
    require(baseline.snapshots.size() == sequence.commands.size(), "Every update boundary preserved");
    for (const auto& intervals : std::vector<std::vector<std::int64_t>>{{16666, 16667, 16667}, {6944, 6944, 6945}, {1, 70000, 3, 19000, 250000, 333}}) {
        require(presented_replay(sequence, intervals) == baseline, "30/60/144/irregular presentation yields identical snapshots/events");
    }
    require(baseline.snapshots[9].phase == Phase::ended && baseline.snapshots[10].phase == Phase::active, "Explicit end and restart boundaries");
    require(baseline.snapshots.back().round.objects.front().id > baseline.snapshots.front().round.objects.front().id, "Object IDs increase across rounds");
    const auto malformed = [&](auto mutate, std::string_view message) {
        auto copy = sequence;
        mutate(copy);
        rejected(
            [&] {
                Simulation simulation(copy.initial);
                RoundHooks hooks;
                for (const auto& command : copy.commands) {
                    (void)moorhuhn::tests::apply(simulation, command, hooks);
                }
            },
            message);
    };
    malformed(
        [](auto& copy) {
            std::get<RecordedUpdate>(copy.commands[1]).input.actions[0].control = static_cast<InputControl>(255);
        },
        "Unknown control rejected");
    malformed(
        [](auto& copy) {
            std::get<RecordedUpdate>(copy.commands[1]).input.actions[0].edge = static_cast<InputEdge>(255);
        },
        "Unknown edge rejected");
    malformed(
        [](auto& copy) {
            std::get<RecordedUpdate>(copy.commands[1]).input.actions[0].text = "x";
        },
        "Text on mouse action rejected");
    malformed(
        [](auto& copy) {
            std::get<RecordedUpdate>(copy.commands[2]).input.sequence = 10;
        },
        "Regressing input sequence rejected");
    malformed(
        [](auto& copy) {
            std::get<RecordedUpdate>(copy.commands[2]).at.microseconds = 1;
        },
        "Regressing update time rejected");
    malformed(
        [](auto& copy) {
            copy.initial.next_event_sequence = 0;
        },
        "Zero event sequence rejected");
    malformed(
        [](auto& copy) {
            copy.initial.selector_state = {18};
        },
        "Partial selector state rejected");
    malformed(
        [](auto& copy) {
            copy.initial.shield_variant = 3;
        },
        "Invalid shield variant rejected");
    malformed(
        [](auto& copy) {
            copy.initial.cached_target_slot = 32;
        },
        "Invalid target slot rejected");
    malformed(
        [](auto& copy) {
            copy.commands[1] = copy.commands[0];
        },
        "Restart during active round rejected");
}

void menu_transport() {
    Simulation simulation({}, 73);
    ProbeHooks hooks;
    RandomGenerator expected(73);
    require(simulation.draw_between_rounds() == expected.draw(), "Menu consumes explicit shared RNG");
    const auto before = simulation.snapshot();
    auto idle = update(1000, 1, 1, 0, {action(1000, 1, InputControl::space)});
    require(simulation.update(idle, hooks).events.empty(), "Idle input emits no game events");
    const auto after = simulation.snapshot();
    require(after.phase == Phase::idle && after.remaining_ms == 90000 && after.update_index == 0, "Idle transport does not run round clocks");
    require(after.random == before.random && hooks.order.empty(), "Idle transport does not draw RNG or run hooks");
    require(after.persistent.held[9] && after.persistent.last_input_sequence == 1, "Idle input retains held and ordering state");
    (void)simulation.start_round({{1000}, 1, 91}, hooks);
    rejected(
        [&] {
            (void)simulation.draw_between_rounds();
        },
        "Menu RNG forbidden during a round");
    require(simulation.snapshot().random.seed == 91, "Round reseeds menu RNG explicitly");
}
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 3 || std::string_view(argv[1]) != "--fixtures") {
            throw std::invalid_argument("Usage: simulation --fixtures PATH");
        }
        const std::filesystem::path fixtures = argv[2];
        menu_transport();
        random_vectors(fixtures);
        boundary_and_restart();
        input_batches();
        clock_vectors(fixtures);
        timer_stage();
        presentation_tests();
        std::cout << "Simulation: " << checks << " checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Simulation: " << error.what() << '\n';
        return 1;
    }
}
