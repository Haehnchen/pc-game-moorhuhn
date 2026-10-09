#include "game/simulation.hpp"

#include <algorithm>
#include <bit>
#include <limits>
#include <stdexcept>
#include <utility>

namespace moorhuhn::game {
namespace {
using namespace contracts;
static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559);

// Fixed seed bytes; changing them changes every random sequence.
constexpr std::string_view seed_hex = "c67e816b4bfbe2fb54f6bddf7c1ce18701bf31de56720f4767668759aa883c59ea561"
                                      "37bd285a1d83c54552f37ae655bda027998cce31a768e5fd9998f1f3f36ee43784d0d"
                                      "fabea6dae4868edc296d4eff56e17020fb8fb1580590c509dc53cdaa3b489952d3529d"
                                      "069feab5c206139849b2011eac3288319c52469571368f57f6391d16fa8874f5987c1"
                                      "75c41bb6d718e0f7059c7011b2f333d91c01da50d0dab338d7e5e8f3ee66874a63a"
                                      "b1c39311a864c7dbcae060e1f3bf090067a2e325a0213187d562c5a84f7e2e096b94"
                                      "9fb06da99e5a0b467080b6cf470ca6a52ad8acfba0ebb779247223924880c5a6a785b"
                                      "7d78c90e4ab63445266e39c3325f95e";
static_assert(seed_hex.size() == 512);

constexpr std::uint8_t nibble(char value) {
    return static_cast<std::uint8_t>(value <= '9' ? value - '0' : value - 'a' + 10);
}

constexpr auto seed_table() {
    std::array<std::uint8_t, 256> table{};

    for (std::size_t i = 0; i < table.size(); ++i) {
        table[i] = static_cast<std::uint8_t>((nibble(seed_hex[i * 2]) << 4) | nibble(seed_hex[i * 2 + 1]));
    }

    return table;
}

constexpr auto table = seed_table();

void require(bool condition, std::string_view error) {
    if (!condition) {
        throw std::invalid_argument(std::string(error));
    }
}

std::uint64_t allocate(std::uint64_t& next) {
    if (next == 0 || next == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("Session sequence exhausted");
    }

    return next++;
}

// Round to nearest-even binary32 without depending on the host rounding mode.
float binary32_delta(std::uint32_t value) {
    if (value == 0) {
        return 0;
    }

    unsigned exponent = 31U - static_cast<unsigned>(std::countl_zero(value));
    std::uint32_t significand;

    if (exponent <= 23) {
        significand = value << (23 - exponent);
    } else {
        const auto shift = exponent - 23;
        significand = value >> shift;
        const auto remainder = value & ((1U << shift) - 1);
        const auto halfway = 1U << (shift - 1);

        if (remainder > halfway || (remainder == halfway && (significand & 1))) {
            ++significand;
        }

        if (significand == (1U << 24)) {
            significand >>= 1;
            ++exponent;
        }
    }

    return std::bit_cast<float>(((exponent + 127) << 23) | (significand & 0x7fffff));
}

std::int32_t signed_low(std::int64_t value) {
    // Keep the low 32 bits and reinterpret them as signed.
    return std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(value));
}

void pointer_valid(ScreenPoint point) {
    require(point.x >= 0 && point.x < logical_width && point.y >= 0 && point.y < logical_height, "Pointer outside logical playfield");
}

void persistent_valid(const PersistentState& state) {
    require(state.cached_target_slot >= -1 && state.cached_target_slot < 32, "Invalid cached target slot");
    require(state.selector_state.empty() || state.selector_state.size() == selector_buffer_size, "Invalid selector carrier size");
    require(state.shield_variant <= 2, "Invalid shield variant");
    pointer_valid(state.cursor);
    require(state.next_round_id && state.next_object_id && state.next_event_sequence, "Invalid session sequence");
    require(state.last_input_end.microseconds >= 0, "Negative input time");
}

void input_valid(const RecordedUpdate& update, const PersistentState& state, TimePoint previous) {
    const auto& input = update.input;
    require(update.at >= previous && update.at.microseconds >= 0, "Update time regressed");
    require(input.sequence > state.last_input_sequence, "Input sequence must increase");
    require(input.begin >= state.last_input_end && input.end >= input.begin && input.end <= update.at, "Invalid input interval");

    if (input.pointer) {
        pointer_valid(*input.pointer);
    }

    auto time = input.begin;
    std::uint64_t order = 0;
    bool first = true;
    bool focus = state.focused;

    for (const auto& action : input.actions) {
        require(action.at >= time && action.at <= input.end, "Invalid action time");
        require(first || action.order > order, "Action order must increase");
        require(static_cast<unsigned>(action.control) <= static_cast<unsigned>(InputControl::text), "Invalid input control code");
        require(action.edge == InputEdge::pressed || action.edge == InputEdge::released, "Invalid input edge code");
        require(action.control == InputControl::text ? action.edge == InputEdge::pressed && !action.text.empty() : action.text.empty(), "Invalid text action payload");

        if (action.pointer) {
            pointer_valid(*action.pointer);
        }

        if (action.control == InputControl::focus) {
            focus = action.edge == InputEdge::pressed;
        }

        time = action.at;
        order = action.order;
        first = false;
    }

    require(input.focused == focus, "Focus change needs an ordered edge");
}

std::uint32_t consume_input(const InputFrame& input, PersistentState& state) {
    std::uint32_t mask = 0;

    for (const auto& action : input.actions) {
        if (action.pointer) {
            state.cursor = *action.pointer;
        }

        const bool down = action.edge == InputEdge::pressed;

        if (action.control == InputControl::focus) {
            state.focused = down;
            // Focus loss clears keyboard holds; mouse edges remain in this batch.
            if (!down) {
                std::fill(state.held.begin() + 2, state.held.end(), false);
            }
        } else if (action.control != InputControl::text) {
            state.held[static_cast<std::size_t>(action.control)] = down;
            // Adapter policy: null means outside/unmapped, so it cannot hit a cached target.
            if (down && action.pointer && action.control == InputControl::primary) {
                mask |= 1;
            }

            if (down && action.pointer && action.control == InputControl::secondary) {
                mask |= 2;
            }
        }
    }

    if (input.pointer) {
        state.cursor = *input.pointer;
    }

    state.last_input_sequence = input.sequence;
    state.last_input_end = input.end;

    return mask;
}

std::string_view phase_name(Phase phase) {
    switch (phase) {
        case Phase::idle:
            return "idle";
        case Phase::active:
            return "active";
        case Phase::ended:
            return "ended";
        case Phase::quit:
            return "quit";
    }

    throw std::logic_error("Invalid phase");
}

} // namespace

RandomGenerator::RandomGenerator(std::uint32_t seed) {
    reseed(seed);
}

void RandomGenerator::reseed(std::uint32_t seed) {
    state_.seed = seed;
    state_.cursor = static_cast<std::uint8_t>(seed);
    state_.draws = 0;

    for (std::size_t i = 0; i < state_.bytes.size(); ++i) {
        state_.bytes[i] = table[(static_cast<std::size_t>(seed) + i) & 255];
    }
}

std::uint32_t RandomGenerator::draw() {
    if (state_.draws == UINT64_MAX) {
        throw std::overflow_error("Random draw count exhausted");
    }

    // Only window starts wrap to 8 bits; each four-byte window stays contiguous.
    const auto source = (static_cast<unsigned>(state_.cursor) - 44U) & 255U;
    const auto destination = (static_cast<unsigned>(state_.cursor) + 4U) & 255U;
    std::uint32_t result = 0;

    for (unsigned i = 0; i < 4; ++i) {
        result |= static_cast<std::uint32_t>(state_.bytes[source + i]) << (8 * i);
    }

    for (unsigned i = 0; i < 4; ++i) {
        state_.bytes[destination + i] ^= static_cast<std::uint8_t>(result >> (8 * i));
    }

    state_.cursor = static_cast<std::uint8_t>(destination);
    ++state_.draws;

    return result;
}

ObjectId EventWriter::allocate_object() {
    return allocate(state_.next_object_id);
}

void EventWriter::emit(EventPayload payload) {
    output_.events.push_back({allocate(state_.next_event_sequence), at_, std::move(payload)});
}

Simulation::Simulation(PersistentState initial, std::uint32_t initial_random_seed) : random_(initial_random_seed) {
    persistent_valid(initial);
    state_.persistent = std::move(initial);
}

std::uint32_t Simulation::draw_between_rounds() {
    require(state_.phase != Phase::active, "UI random draw during an active round");

    return random_.draw();
}

UpdateOutput Simulation::start_round(const RoundStart& start, RoundHooks& hooks) {
    require(state_.phase == Phase::idle || state_.phase == Phase::ended, "Round start requires idle/end phase");
    require(start.at.microseconds >= 0 && start.at >= state_.now && start.at >= state_.persistent.last_input_end, "Round start time regressed");
    const auto id = allocate(state_.persistent.next_round_id);
    auto persistent = std::move(state_.persistent);
    state_ = Snapshot{};
    state_.persistent = std::move(persistent);
    state_.round_id = id;
    state_.phase = Phase::active;
    state_.started_at = start.at;
    state_.now = start.at;
    state_.clock_sample = start.clock_sample;
    random_.reseed(start.seed);
    UpdateOutput output;
    EventWriter events(state_.persistent, start.at, output);
    events.emit(PresentationEffect{"round.started", std::nullopt, std::nullopt});
    const StepInfo step{start.at, start.clock_sample, 0, 0, 0, 90000, 90000, false};
    RoundContext context{step, state_.round, state_.persistent, random_, events};
    hooks.initialize(context);

    return output;
}

UpdateOutput Simulation::update(const RecordedUpdate& update, RoundHooks& hooks) {
    // Menus still drain ordered transport; only active rounds advance game rules.
    input_valid(update, state_.persistent, state_.now);
    const auto mask = consume_input(update.input, state_.persistent);
    state_.now = update.at;
    UpdateOutput output;
    EventWriter events(state_.persistent, update.at, output);

    if (state_.phase != Phase::active) {
        if (update.input.quit_requested && state_.phase != Phase::quit) {
            state_.phase = Phase::quit;
            events.emit(PresentationEffect{"session.quit", std::nullopt, std::nullopt});
        }

        return output;
    }

    state_.persistent.cursor.x = std::clamp(state_.persistent.cursor.x, 0, 608);
    state_.persistent.cursor.y = std::clamp(state_.persistent.cursor.y, 0, 448);

    // Raw clock subtraction wraps. Keep large deltas as one update.
    StepInfo step{update.at, update.clock_sample, update.clock_sample - state_.clock_sample, 0, mask, state_.remaining_ms, state_.remaining_ms, false};

    step.delta_ms = binary32_delta(step.delta_u32);
    state_.delta_bits = std::bit_cast<std::uint32_t>(step.delta_ms);
    state_.clock_sample = update.clock_sample;

    const auto dt = static_cast<std::uint64_t>(step.delta_ms);
    state_.elapsed_ms = static_cast<std::uint32_t>(static_cast<std::uint64_t>(state_.elapsed_ms) + dt);
    state_.spawn_elapsed_ms = signed_low(static_cast<std::int64_t>(state_.spawn_elapsed_ms) + static_cast<std::int64_t>(dt));

    if (state_.spawn_elapsed_ms > 750) {
        step.spawn_due = true;
        state_.spawn_elapsed_ms = 0;
    }

    if (state_.update_index == UINT64_MAX) {
        throw std::overflow_error("Update index exhausted");
    }

    ++state_.update_index;

    RoundContext context{step, state_.round, state_.persistent, random_, events};
    // Combat reads the previous selection before budget subtraction.
    // World update and selection still run on the final active step.
    hooks.camera_and_spawn(context);
    hooks.combat(context);

    state_.remaining_ms = signed_low(static_cast<std::int64_t>(state_.remaining_ms) - static_cast<std::int64_t>(dt));
    step.remaining_after_ms = state_.remaining_ms;
    hooks.timer(context);
    hooks.update_world(context);
    hooks.select_target(context);

    if (update.input.quit_requested || state_.persistent.held[static_cast<std::size_t>(InputControl::cancel)] || state_.remaining_ms <= 0) {
        state_.end_reason =
            update.input.quit_requested ? EndReason::quit : (state_.persistent.held[static_cast<std::size_t>(InputControl::cancel)] ? EndReason::cancelled : EndReason::expired);
        state_.phase = update.input.quit_requested ? Phase::quit : Phase::ended;
        events.emit(PresentationEffect{"round.ended", std::nullopt, std::nullopt});
    }

    return output;
}

Snapshot Simulation::snapshot() const {
    auto result = state_;
    result.random = random_.snapshot();

    return result;
}

GameState Simulation::game_state() const {
    GameState result;
    result.round = {state_.round_id, std::string(phase_name(state_.phase)), state_.started_at};
    result.now = state_.now;
    result.elapsed = {static_cast<std::int64_t>(std::bit_cast<float>(state_.delta_bits)) * 1000};
    result.magazine = state_.round.magazine;
    result.score = state_.round.score;
    result.camera = state_.round.camera;
    result.objects = state_.round.objects;
    result.random.algorithm = std::string(random_algorithm);
    const auto& rng = random_.snapshot();
    // 65 little-endian 32-bit words, then cursor, seed and draw count.
    for (std::size_t offset = 0; offset < rng.bytes.size(); offset += 4) {
        std::uint32_t word = 0;

        for (unsigned byte = 0; byte < 4; ++byte) {
            word |= static_cast<std::uint32_t>(rng.bytes[offset + byte]) << (byte * 8);
        }

        result.random.words.push_back(word);
    }

    result.random.words.push_back(rng.cursor);
    result.random.words.push_back(rng.seed);
    result.random.words.push_back(rng.draws);

    return result;
}

} // namespace moorhuhn::game
