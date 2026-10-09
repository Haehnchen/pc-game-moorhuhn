#include "app/session.hpp"

#include "assets/asset_store.hpp"
#include "render/renderer.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace {
using namespace moorhuhn;
using namespace contracts;
std::size_t checks{};

void require(bool condition, std::string_view message) {
    ++checks;
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

struct Step {
    std::int64_t ms{};
    std::string action;
    ScreenPoint pointer;
};

std::vector<Step> timeline() {
    std::ifstream input(std::filesystem::path(__FILE__).parent_path() / "fixtures/session/round.csv");
    require(static_cast<bool>(input), "integration timeline fixture exists");
    std::vector<Step> result;
    std::string line;
    while (std::getline(input, line)) {
        if (line.empty() || line[0] == '#') {
            continue;
        }
        std::ranges::replace(line, ',', ' ');
        std::istringstream fields(line);
        Step step;
        require(static_cast<bool>(fields >> step.ms >> step.action >> step.pointer.x >> step.pointer.y), "timeline row parses");
        result.push_back(step);
    }
    require(result.size() == 14, "sparse full-round timeline has14 steps");
    return result;
}

std::string describe(const GameEvent& e) {
    std::ostringstream s;
    s << e.sequence << ':' << e.at.microseconds << ':';
    if (const auto* p = std::get_if<PlaySound>(&e.payload)) {
        s << "sound:" << p->sound.value;
    } else {
        const auto& presentation = std::get<PresentationEffect>(e.payload);
        s << "effect:" << presentation.name;
        if (presentation.object) {
            s << ":object=" << *presentation.object;
        }
        if (presentation.position) {
            s << ":position=" << presentation.position->x << ',' << presentation.position->y;
        }
    }
    return s.str();
}

bool effect(const UpdateOutput& out, std::string_view name) {
    return std::ranges::any_of(out.events, [&](auto& e) {
        const auto* p = std::get_if<PresentationEffect>(&e.payload);
        return p && p->name == name;
    });
}

struct Result {
    std::vector<game::Snapshot> snapshots;
    std::vector<std::string> events;
    std::array<ui::Shell, 8> shells;
    bool operator==(const Result&) const = default;
};

Result round(std::span<const AssetView> assets, const std::vector<Step>& steps, int rate, bool measure) {
    app::Session session(assets, true, 33);
    Result result;
    require(session.snapshot().phase == game::Phase::idle, "session begins idle");
    std::uint64_t last_event{}, sequence{};
    std::int64_t previous{};
    auto record = [&](const UpdateOutput& out) {
        for (const auto& e : out.events) {
            require(e.sequence > last_event, "session events strictly increasing");
            last_event = e.sequence;
            result.events.push_back(describe(e));
        }
        result.snapshots.push_back(session.snapshot());
    };
    record(session.start({0}, 33));
    require(session.snapshot().phase == game::Phase::active, "start enters active round");
    require(session.snapshot().remaining_ms == 90000 && session.snapshot().round.magazine.loaded == 8, "start budget and eight shells");
    for (std::size_t index = 0; index < steps.size(); ++index) {
        const auto& step = steps[index];
        InputFrame input{++sequence, {previous * 1000}, {step.ms * 1000}, step.pointer, {}, false, true};
        std::uint64_t order{};
        if (step.action != "pointer") {
            const auto control = step.action == "primary" ? InputControl::primary : InputControl::secondary;
            input.actions.push_back({input.end, ++order, control, InputEdge::released, step.pointer, {}});
            input.actions.push_back({input.end, ++order, control, InputEdge::pressed, step.pointer, {}});
        }
        const auto out = session.update(input);
        record(out);
        const auto snap = session.snapshot();
        if (index < 8) {
            require(snap.round.magazine.loaded == 7 - index && effect(out, "combat.shot"), "eight loaded shots consume once");
        }
        if (index == 8) {
            require(snap.round.magazine.loaded == 0 && effect(out, "combat.empty"), "ninth primary is empty");
        }
        if (index == 9) {
            require(snap.round.magazine.loaded == 8 && effect(out, "combat.reload"), "empty right reloads immediately");
        }
        if (index == 10) {
            require(snap.persistent.cached_target_slot == -1, "shield acquisition point has no object target");
        }
        if (index == 11) {
            require(snap.round.world.shield_variant == 2 && snap.round.world.shield_flag == 2 && snap.persistent.shield_variant == 2
                        && snap.round.specials.ledger.back().cause == game::ScoreCause::shield,
                "loaded shield penalty and variant persistence");
        }
        if (index == 12) {
            require(snap.phase == game::Phase::ended && snap.end_reason == game::EndReason::expired && snap.remaining_ms == 0 && snap.round.magazine.loaded == 6
                        && effect(out, "combat.shot"),
                "loaded terminal iteration precedes90000 expiry");
        }
        if (index == 13) {
            require(snap.round == result.snapshots[result.snapshots.size() - 2].round && out.events.empty(), "ended mouse input cannot mutate gameplay or emit sounds");
        }
        const auto shells = session.shells().snapshot();
        const int count = rate == 0 ? static_cast<int>(index % 7) + 1 : rate / 10;
        for (int i = 0; i < count; ++i) {
            static_cast<void>(session.draw());
        }
        require(session.snapshot() == snap && session.shells().snapshot() == shells, "extra presentation calls leave all simulation and shells unchanged");
        previous = step.ms;
    }
    const auto ended = session.snapshot();
    record(session.start({91000000}, 33));
    const auto restarted = session.snapshot();
    require(restarted.phase == game::Phase::active && restarted.round_id == 2 && restarted.remaining_ms == 90000, "restart restores round phase and clock");
    require(restarted.round.magazine.loaded == 8 && restarted.round.score == 0 && restarted.round.specials == game::SpecialsState{},
        "restart resets ammunition score popups holes and ledger");
    require(restarted.round.world.shield_variant == 2 && restarted.round.world.shield_flag == 0, "restart retains artwork and resets shield flag");
    require(restarted.persistent.cached_target_slot == ended.persistent.cached_target_slot && restarted.persistent.selector_state == ended.persistent.selector_state,
        "restart retains cached slot and indexed scratch");
    require(restarted.persistent.next_object_id > ended.persistent.next_object_id && restarted.persistent.next_event_sequence > ended.persistent.next_event_sequence,
        "restart object/event IDs remain session-unique");
    require(restarted.round.world.camera == 640 && restarted.persistent.cursor == ScreenPoint{320, 240}, "restart resets camera and cursor");
    InputFrame camera{++sequence, {91000000}, {91010000}, ScreenPoint{0, 240}, {}, false, true};
    record(session.update(camera));
    require(session.snapshot().round.world.camera == 636, "native pointer camera edge moves four pixels");
    if (measure) {
        const auto before = session.snapshot();
        const auto shells = session.shells().snapshot();
        const auto draw = session.draw();
        render::Renderer renderer;
        const auto& frame = renderer.render(draw, assets);
        require(frame.bytes().size() == 640U * 480U * 4U, "actual game-assets frame rendered");
        const std::vector<std::uint8_t> first(frame.bytes().begin(), frame.bytes().end());
        static_cast<void>(renderer.render(session.draw(), assets));
        require(std::ranges::equal(first, renderer.framebuffer().bytes()), "repeated actual render produces identical pixels");
        require(session.snapshot() == before && session.shells().snapshot() == shells, "actual renderer leaves game and shells unchanged");
    }
    result.shells = session.shells().snapshot();
    return result;
}
} // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 2, "usage: session <asset manifest>");
        auto store = assets::AssetStore::load(argv[1]);
        std::vector<AssetView> images;
        for (const auto& id : store.image_ids()) {
            images.push_back(store.image(id.value));
        }
        const auto steps = timeline();
        const auto reference = round(images, steps, 30, true);
        for (int rate : std::array<int, 3>{60, 144, 0}) {
            require(round(images, steps, rate, false) == reference, "same recorded timeline and events under30/60/144/irregular presentation");
        }
        std::cout << "Session: " << checks << " checks passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Session: " << e.what() << '\n';
        return 1;
    }
}
