#include "app/session.hpp"

namespace moorhuhn::app {
namespace {
game::PersistentState initial(bool focused) {
    game::PersistentState state;
    state.focused = focused;

    return state;
}

std::uint32_t clock_sample(contracts::TimePoint at) {
    return static_cast<std::uint32_t>(at.microseconds / 1000);
}
} // namespace

Session::Session(std::span<const contracts::AssetView> assets, bool focused, std::uint32_t seed)
    : assets_(assets.begin(), assets.end()), indexed_(assets), simulation_(initial(focused), seed) {
    game::validate_world_assets(assets_);
}

contracts::UpdateOutput Session::update(const contracts::InputFrame& input) {
    return simulation_.update({input.end, clock_sample(input.end), input}, *this);
}

contracts::UpdateOutput Session::start(contracts::TimePoint at, std::uint32_t seed) {
    return simulation_.start_round({at, clock_sample(at), seed}, *this);
}

void Session::initialize(game::RoundContext& context) {
    game::initialize_world(context);
    game::initialize_combat(context);
    game::initialize_specials(context);
    shells_.reset();
    timer_.reset();
    context.events.emit(contracts::PresentationEffect{"round.ambience.start", {}, {}});
    compose(context.round);
}

void Session::camera_and_spawn(game::RoundContext& context) {
    game::camera_and_spawn_world(context);
}

void Session::combat(game::RoundContext& context) {
    const auto candidates = game::world_hit_candidates(context.round.world, assets_);
    const auto result = game::process_combat(context, candidates, &specials_);
    context.round.world.shake = static_cast<std::int32_t>(result.shake);

    if (result.ejected_shell) {
        shells_.shot(*result.ejected_shell);
    }

    if (result.action == game::CombatAction::reloaded) {
        shells_.reload();
    }

    shells_.advance_loop();
}

void Session::timer(game::RoundContext& context) {
    if (const auto sound = timer_.update(context.step.remaining_after_ms)) {
        context.events.emit(contracts::PlaySound{{*sound}});
    }
}

void Session::update_world(game::RoundContext& context) {
    game::advance_world(context);
    game::advance_specials(context);
}

void Session::compose(const game::RoundModel& round) {
    const auto extras = game::specials_draw_data(round.world, round.specials);
    playfield_ = game::world_draw_list(round.world, extras.extras());
}

void Session::select_target(game::RoundContext& context) {
    compose(context.round);
    std::optional<contracts::AtlasRect> region{contracts::AtlasRect{0, 0, 0, 0}};
    const auto cursor = context.persistent.cursor;

    if (cursor.x >= 0 && cursor.x < contracts::logical_width && cursor.y >= 0 && cursor.y < contracts::logical_height) {
        const auto point = game::selector_hotspot(cursor);
        region = contracts::AtlasRect{point.x, point.y, 1, 1};
    }

    const auto& pixels = indexed_.compose(playfield_, region);
    const auto candidates = game::world_hit_candidates(context.round.world, assets_);
    const auto selected = game::select_indexed_target(context.persistent, context.persistent.cursor, {pixels, 640}, candidates);

    for (const auto& position : selected.positions) {
        game::quantize_world_position(context.round.world, static_cast<std::int32_t>(position.slot), position.id);
    }
}

contracts::DrawList Session::draw() const {
    const auto state = simulation_.snapshot();
    std::vector<contracts::DrawCommand> commands(playfield_.commands().begin(), playfield_.commands().end());
    const auto hud = ui::draw_hud({state.round.score, state.remaining_ms, state.elapsed_ms}, shells_);
    commands.insert(commands.end(), hud.commands().begin(), hud.commands().end());
    contracts::SpriteDraw cursor{{{"cursors"}, 0}, state.persistent.cursor, contracts::SpriteBlend::copy};

    cursor.transparent_index = 0;
    commands.emplace_back(cursor);

    return contracts::DrawList{std::move(commands)};
}

} // namespace moorhuhn::app
