#pragma once

#include "contracts/events.hpp"
#include "game/specials_state.hpp"
#include "game/world_state.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace moorhuhn::game {

// Stable random generator ID.
inline constexpr std::string_view random_algorithm = "R02-contiguous-xor260-v1";
inline constexpr std::size_t selector_buffer_size = 65536;

struct RandomSnapshot {
    std::array<std::uint8_t, 260> bytes{};
    std::uint8_t cursor{};
    std::uint32_t seed{};
    std::uint64_t draws{};

    bool operator==(const RandomSnapshot&) const = default;
};

class RandomGenerator {
public:
    explicit RandomGenerator(std::uint32_t seed = 0);
    void reseed(std::uint32_t seed);
    [[nodiscard]] std::uint32_t draw();

    [[nodiscard]] const RandomSnapshot& snapshot() const noexcept {
        return state_;
    }

private:
    RandomSnapshot state_;
};

enum class Phase : std::uint8_t {
    idle,
    active,
    ended,
    quit
};

enum class EndReason : std::uint8_t {
    none,
    expired,
    cancelled,
    quit
};

// Session input, target cache, selector bytes and shield artwork survive round starts.
struct PersistentState {
    // Hit selection and shield appearance carry over to the next round.
    std::int32_t cached_target_slot{}; // First round starts with slot 0.
    std::vector<std::uint8_t> selector_state; // Empty until initialized once with zeroes.
    std::uint8_t shield_variant{1};

    contracts::ScreenPoint cursor{};
    std::array<bool, 11> held{}; // Control codes 0..10; slot 8 (focus) stays unused.
    bool focused{true};

    // IDs and input ordering span the whole session.
    std::uint64_t next_round_id{1};
    std::uint64_t next_object_id{1};
    std::uint64_t next_event_sequence{1};
    std::uint64_t last_input_sequence{};
    contracts::TimePoint last_input_end;

    bool operator==(const PersistentState&) const = default;
};

// Reset for each round; hooks update this state in Simulation::update order.
struct RoundModel {
    contracts::MagazineState magazine{8, 8, false};
    std::int32_t score{};

    contracts::WorldPoint camera{640, 0};
    std::vector<contracts::ObjectState> objects;

    WorldSnapshot world; // Authoritative world; camera and objects are projections.
    SpecialsState specials;

    bool operator==(const RoundModel&) const = default;
};

struct Snapshot {
    Phase phase{Phase::idle};
    EndReason end_reason{EndReason::none};
    std::uint64_t round_id{};

    contracts::TimePoint started_at;
    contracts::TimePoint now;
    std::uint32_t clock_sample{};
    std::uint32_t elapsed_ms{};
    std::uint32_t delta_bits{};
    std::int32_t remaining_ms{90000};
    std::int32_t spawn_elapsed_ms{};
    std::uint64_t update_index{};

    RoundModel round;
    PersistentState persistent;
    RandomSnapshot random;

    bool operator==(const Snapshot&) const = default;
};

struct RoundStart {
    contracts::TimePoint at;
    std::uint32_t clock_sample{}; // Raw milliseconds; subtraction wraps at 32 bits.
    std::uint32_t seed{};

    bool operator==(const RoundStart&) const = default;
};

// One simulation boundary. Rendering and input timestamps must not split it.
struct RecordedUpdate {
    contracts::TimePoint at;
    std::uint32_t clock_sample{};
    contracts::InputFrame input;

    bool operator==(const RecordedUpdate&) const = default;
};

struct StepInfo {
    contracts::TimePoint at;
    std::uint32_t clock_sample{};
    std::uint32_t delta_u32{};
    float delta_ms{};
    std::uint32_t action_mask{};
    std::int32_t remaining_before_ms{};
    std::int32_t remaining_after_ms{};
    bool spawn_due{}; // Strict >750ms; one request, excess discarded.
};

class EventWriter {
public:
    [[nodiscard]] contracts::ObjectId allocate_object();
    void emit(contracts::EventPayload payload);

private:
    friend class Simulation;

    EventWriter(PersistentState& state, contracts::TimePoint at, contracts::UpdateOutput& output) : state_(state), at_(at), output_(output) {}

    PersistentState& state_;
    contracts::TimePoint at_;
    contracts::UpdateOutput& output_;
};

// Borrowed only during a hook. Do not retain any reference.
struct RoundContext {
    const StepInfo& step;
    RoundModel& round;
    PersistentState& persistent;
    RandomGenerator& random;
    EventWriter& events;
};

// Hooks add real modules; the base performs no combat, spawn or selection.
class RoundHooks {
public:
    virtual ~RoundHooks() = default;

    virtual void initialize(RoundContext&) {}

    virtual void camera_and_spawn(RoundContext&) {}

    virtual void combat(RoundContext&) {}

    virtual void timer(RoundContext&) {} // After budget subtraction, before world updates.

    virtual void update_world(RoundContext&) {}

    virtual void select_target(RoundContext&) {}
};

class Simulation {
public:
    explicit Simulation(PersistentState initial = {}, std::uint32_t initial_random_seed = 0);
    [[nodiscard]] std::uint32_t draw_between_rounds(); // Explicit UI update draw, never rendering.

    [[nodiscard]] contracts::UpdateOutput start_round(const RoundStart&, RoundHooks&);
    // Idle/end updates consume ordered transport without advancing clocks, RNG or hooks.
    [[nodiscard]] contracts::UpdateOutput update(const RecordedUpdate&, RoundHooks&);

    [[nodiscard]] Snapshot snapshot() const;
    [[nodiscard]] contracts::GameState game_state() const;

private:
    Snapshot state_;
    RandomGenerator random_;
};

} // namespace moorhuhn::game
