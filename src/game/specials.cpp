#include "game/specials.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace moorhuhn::game {
namespace {
using L = long double; // Keep live intermediates extended; explicit float stores mark rounding.

float store(L value) {
    return static_cast<float>(value);
}

std::int32_t integer(L value) {
    if (!std::isfinite(value) || value < std::numeric_limits<std::int32_t>::min() || value > std::numeric_limits<std::int32_t>::max()) {
        throw std::invalid_argument("invalid special projection");
    }

    return static_cast<std::int32_t>(value);
}

std::int32_t add32(std::int32_t a, std::int32_t b) {
    return std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(a) + static_cast<std::uint32_t>(b));
}

void effect(RoundContext& c, std::string_view name, std::optional<contracts::ObjectId> id = {}, std::optional<contracts::ScreenPoint> position = {}) {
    c.events.emit(contracts::PresentationEffect{std::string(name), id, position});
}

void transaction(
    RoundContext& c, ShotId shot, ScoreCause cause, std::int32_t requested, std::int32_t slot = -1, std::int32_t rectangle = -1, std::optional<contracts::ObjectId> object = {}) {
    const auto previous = c.round.score;
    // Add in signed low-32-bit arithmetic, then clamp negative totals to zero.
    c.round.score = std::max(0, add32(previous, requested));
    const auto delta = std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(c.round.score) - static_cast<std::uint32_t>(previous));
    c.round.specials.ledger.push_back({shot, cause, c.step.at, object, slot, rectangle, requested, delta, c.round.score});
    effect(c, "specials.score", object);
}

ScorePopup* free_popup(SpecialsState& state) {
    const auto found = std::ranges::find_if(state.popups, [](const auto& p) {
        return !p.active;
    });

    return found == state.popups.end() ? nullptr : &*found;
}

void popup(RoundContext& c, ScorePopup& p, float x, float y, std::int32_t layer, std::int32_t score) {
    p = {};
    p.active = true;
    p.id = c.events.allocate_object();
    p.world_x = x;
    p.world_y = y;
    p.layer = layer;
    p.score = score;
}

void object_popup(RoundContext& c, const WorldObject& o) {
    auto* p = free_popup(c.round.specials);

    if (!p) {
        return;
    }

    const auto state = static_cast<int>(o.state);
    L x = o.world_x;
    L y = o.world_y;

    if (state >= 0 && state <= 3 && o.layer >= 2 && o.layer <= 4) {
        const int size = o.layer - 2;
        constexpr std::array<L, 3> amplitudes{150, 100, 60}, dx{48, 6, -9}, dy{20, 0, -10};

        if (state <= 1) {
            const L angle = static_cast<L>(o.phase) * static_cast<L>(57.29579162597656F);
            // Round the cosine result before applying the popup amplitude.
            x += std::sin(angle) * amplitudes[static_cast<std::size_t>(size)];
            y += static_cast<L>(store(std::cos(angle))) * amplitudes[static_cast<std::size_t>(size)];
        }

        x += dx[static_cast<std::size_t>(size)];
        y += dy[static_cast<std::size_t>(size)];
    } else {
        switch (state) {
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
                return;
        }
    }

    popup(c, *p, store(x), store(y), o.layer, o.score);
}

bool handled(const WorldObject& o) {
    const int state = static_cast<int>(o.state);

    if (o.layer == 0) {
        return state == 8;
    }

    if (o.layer == 1) {
        return state == 4 || state == 5 || state == 9;
    }

    if (o.layer == 2) {
        return state >= 0 && state <= 3;
    }

    if (o.layer == 3) {
        return (state >= 0 && state <= 3) || state == 10 || state == 11 || state == 15;
    }

    if (o.layer == 4) {
        return (state >= 0 && state <= 3) || state == 6 || state == 7 || (state >= 12 && state <= 14);
    }

    return false;
}

void object_hit(RoundContext& c, const CombatResult& shot, ShotId id) {
    // The old slot can now be inactive or reused. Read its current record anyway.
    const auto before = c.round.world.objects[static_cast<std::size_t>(shot.cached_slot)];
    const auto object = before.id ? std::optional{before.id} : std::nullopt;
    transaction(c, id, ScoreCause::object, before.score, shot.cached_slot, -1, object);

    if (!handled(before)) {
        return;
    }

    const int state = static_cast<int>(before.state);

    if (state <= 3 && before.layer >= 2 && before.layer <= 4) {
        static_cast<void>(c.random.draw()); // All r&3 branches request the same sound.
        constexpr std::array<std::string_view, 3> names{"specials.sound.hit.large", "specials.sound.hit.medium", "specials.sound.hit.small"};

        effect(c, names[static_cast<std::size_t>(before.layer - 2)], object);
    } else if (before.layer == 1 && state == 4) {
        effect(c, "specials.sound.hit.close", object);
    } else if (before.layer == 3 && state == 10) {
        effect(c, "specials.sound.hit.wing", object);
    } else if (before.layer == 3 && state == 11) {
        effect(c, "specials.sound.hit.hat", object);
    } else if (before.layer == 3 && state == 15) {
        effect(c, "specials.sound.hit.scare", object);
    } else if (before.layer == 4 && (state == 6 || state == 7)) {
        effect(c, "specials.sound.balloon.start", object);
    } else if (before.layer == 4 && state == 12) {
        effect(c, "world.sound.plane.stop", object);
        effect(c, "specials.sound.plane_hit.start", object);
    }

    object_popup(c, before);
    const auto change = apply_world_hit(c.round.world, shot.cached_slot);

    if (change.changed) {
        effect(c, "specials.object_hit", object, shot.point);
    }

    c.persistent.cached_target_slot = -1;
}

struct Rectangle {
    int left;
    int right;
    int top;
    int bottom;
    int action;
};

constexpr std::array<Rectangle, 6> rectangles{
    {{1552, 1628, 79, 107, 1}, {1564, 1628, 115, 152, 1}, {1656, 1786, 72, 111, 2}, {300, 425, 1, 479, 3}, {2220, 2345, 1, 479, 3}, {3475, 3576, 1, 479, 3}}};

void rectangle_hit(RoundContext& c, const CombatResult& shot, ShotId id) {
    const auto offset = integer(static_cast<L>(c.round.world.camera) * 2.5F);
    // Strict edges; process every matching rectangle.
    for (std::size_t i = 0; i < rectangles.size(); ++i) {
        const auto r = rectangles[i];

        if (shot.point.x <= r.left - offset || shot.point.x >= r.right - offset || shot.point.y <= r.top || shot.point.y >= r.bottom) {
            continue;
        }

        if (r.action == 3) {
            c.events.emit(contracts::PlaySound{{"tree22"}});
            auto& state = c.round.specials;
            auto& hole = state.holes[state.next_hole];
            // Holes use cursor top-left; rectangle tests use the hotspot.
            hole = {true, c.events.allocate_object(), add32(c.persistent.cursor.x, offset), c.persistent.cursor.y};

            state.next_hole = (state.next_hole + 1) % 16;
            effect(c, "specials.tree_hole", hole.id, shot.point);
            auto& limit = c.round.world.pool_limit;

            if (limit < 0 || limit > 20) {
                throw std::invalid_argument("tree requires a normal pool limit in 0..20");
            }

            struct Restore {
                int& value;
                int saved;

                ~Restore() {
                    value = saved;
                }
            } restore{limit, limit};

            limit += 12;

            for (int count = 0; count < 8; ++count) {
                static_cast<void>(spawn_world(c, 6));
            }
        } else {
            c.events.emit(contracts::PlaySound{{"shield22"}});
            // A full popup pool skips the penalty, but keeps sound and shield changes.
            if (auto* p = free_popup(c.round.specials)) {
                popup(c, *p, 1600, 150, 0, -25);
                transaction(c, id, ScoreCause::shield, -25, -1, static_cast<int>(i));
            }

            auto& world = c.round.world;

            if ((r.action == 1 && world.shield_flag > 0) || (r.action == 2 && world.shield_flag < 2)) {
                world.shield_flag = r.action == 1 ? 0 : 2;
                world.shield_variant = world.shield_flag;
                c.persistent.shield_variant = static_cast<std::uint8_t>(world.shield_variant);
                effect(c, "specials.shield_changed", {}, shot.point);
            }
        }
    }
}

std::span<const std::uint32_t> glyphs(int score) {
    static constexpr std::array<std::uint32_t, 1> five{130};
    static constexpr std::array<std::uint32_t, 2> ten{126, 125}, twentyfive{127, 130}, fifty{130, 125}, minusfive{124, 130};
    static constexpr std::array<std::uint32_t, 3> minustwentyfive{124, 127, 130};

    switch (score) {
        case 5:
            return five;
        case 10:
            return ten;
        case 25:
            return twentyfive;
        case 50:
            return fifty;
        case -5:
            return minusfive;
        case -25:
            return minustwentyfive;
        default:
            return {};
    }
}
} // namespace

void initialize_specials(RoundContext& c) {
    c.round.specials = {};
}

void SpecialsHandler::loaded_shot(RoundContext& c, const CombatResult& shot) {
    // combat.shot is the most recent event when this callback starts.
    loaded_shot(c, shot, {c.persistent.next_round_id - 1, c.persistent.next_event_sequence - 1});
}

void SpecialsHandler::loaded_shot(RoundContext& c, const CombatResult& shot, ShotId id) {
    if (shot.action != CombatAction::shot) {
        return;
    }

    if (!id.round || !id.event || shot.cached_slot < -1 || shot.cached_slot >= 32) {
        throw std::invalid_argument("invalid loaded shot identity or cached slot");
    }

    auto& processed = c.round.specials.processed_shots;

    if (std::ranges::find(processed, id) != processed.end()) {
        return;
    }

    processed.push_back(id);

    if (shot.cached_slot >= 0) {
        object_hit(c, shot, id);
    } else {
        rectangle_hit(c, shot, id);
    }
}

void advance_specials(RoundContext& c) {
    constexpr std::array<float, 5> scales{2.5F, 2.5F, 2.2F, 1.9F, 1.6F};
    constexpr std::array<float, 5> rates{.15F, .15F, .1F, .05F, .025F};

    for (auto& p : c.round.specials.popups) {
        if (!p.active || p.layer < 0 || p.layer > 4) {
            continue;
        }

        // Unsupported layer-0/1 scores do not move or expire.
        if (p.layer <= 1 && p.score != -25 && p.score != -5 && p.score != 25) {
            continue;
        }

        const auto layer = static_cast<std::size_t>(p.layer);
        const auto offset = integer(static_cast<L>(c.round.world.camera) * scales[layer]);
        p.screen_x = store(static_cast<L>(p.world_x) - offset);
        // Draw from the previous world Y, then advance movement.
        p.screen_y = p.world_y;
        p.world_y = store(static_cast<L>(p.world_y) - static_cast<L>(c.step.delta_ms) * rates[layer]);
        const L elapsed = static_cast<L>(p.elapsed_ms) + c.step.delta_ms;
        p.elapsed_ms = store(elapsed);

        if (elapsed >= 2000) {
            p.active = false;
        }
    }
}

WorldDrawExtras SpecialsDrawData::extras() const noexcept {
    WorldDrawExtras result;

    for (std::size_t i = 0; i < after_objects.size(); ++i) {
        result.after_objects[i] = after_objects[i];
    }

    result.tree_holes = tree_holes;

    return result;
}

SpecialsDrawData specials_draw_data(const WorldSnapshot& world, const SpecialsState& state) {
    SpecialsDrawData result;

    for (const auto& p : state.popups) {
        if (!p.active || p.layer < 0 || p.layer > 5) {
            continue;
        }

        L x = p.screen_x;

        for (const auto frame : glyphs(p.score)) {
            contracts::SpriteDraw draw;
            draw.frame = {{"bigfont"}, frame};
            draw.destination = {projected_integer(store(x)), projected_integer(p.screen_y)};
            draw.transparent_index = 18;
            result.after_objects[static_cast<std::size_t>(p.layer)].push_back(draw);
            x = store(x + 24);
        }
    }

    const auto offset = integer(static_cast<L>(world.camera) * 2.5F);

    for (const auto& hole : state.holes) {
        if (!hole.active) {
            continue;
        }

        contracts::SpriteDraw draw;
        draw.frame = {{"hole2"}, 0};
        draw.destination = {add32(hole.world_x, -offset), add32(hole.world_y, world.shake)};
        draw.transparent_index = 18;
        result.tree_holes.push_back(draw);
    }

    return result;
}
} // namespace moorhuhn::game
