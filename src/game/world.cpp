#include "game/world.hpp"

#include "game/combat.hpp"
#include "game/simulation.hpp"
#include "render/renderer.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace moorhuhn::game {
namespace {
using namespace contracts;
using L = long double; // Keep live intermediates extended; explicit float stores mark rounding.
constexpr float angle_scale = 57.29579162597656F;
// Active animation counts may be smaller than stored atlases; leaf uses 19 of 20 frames.
// clang-format off
constexpr std::array<ObjectSpec, 23> specs{{
    {"chickl", 120, 80, 19, 62.5F},
    {"chickl2", 60, 40, 19, 62.5F},
    {"chickl3", 30, 20, 19, 62.5F},
    {"chickr", 120, 80, 19, 62.5F},
    {"chickr2", 60, 40, 19, 62.5F},
    {"chickr3", 30, 20, 19, 62.5F},
    {"chickd", 120, 80, 19, 50},
    {"chickd2", 60, 40, 19, 50},
    {"chickd3", 30, 20, 19, 50},
    {"big1", 200, 256, 15, 62.5F},
    {"bighit", 200, 256, 17, 50},
    {"guck", 48, 72, 14, 100},
    {"balloon", 80, 100, 21, 400},
    {"balloon2", 100, 97, 20, 140},
    {"leaf", 60, 60, 19, 50},
    {"leafhit", 80, 80, 25, 20},
    {"scare", 132, 127, 1, 0},
    {"hat", 132, 127, 20, 40},
    {"windmill", 260, 250, 1, 0},
    {"wing", 260, 250, 37, 50},
    {"plane", 100, 97, 20, 100},
    {"sign", 175, 100, 1, 0},
    {"chickie", 8, 6, 19, 63},
}};
// clang-format on

constexpr std::array<float, 3> chicken_k{2.2F, 1.9F, 1.6F};
constexpr std::array<double, 3> random_x{0.04766845806443598, 0.04116821237403201, 0.03466796926659299};

constexpr std::array<float, 3> arc_y{350, 275, 240};
constexpr std::array<float, 3> amplitude{150, 100, 60};
constexpr std::array<float, 3> arc_velocity{7.5e-6F, 5e-6F, 2.5e-6F};
constexpr std::array<float, 3> fly_velocity{0.1F, 0.05F, 0.025F};
constexpr std::array<float, 3> fall_velocity{0.025F, 0.0125F, 0.0062F};
constexpr std::array<float, 3> fall_y{0.1F, 0.05F, 0.025F};
constexpr std::array<float, 3> fall_boundary{400, 350, 300};
constexpr std::array<int, 3> chicken_score{5, 10, 25};

std::int32_t trunc_int(L value) {
    if (!std::isfinite(value) || value < std::numeric_limits<std::int32_t>::min() || value >= static_cast<L>(std::numeric_limits<std::int32_t>::max()) + 1) {
        throw std::invalid_argument("World coordinate is outside int32");
    }

    return static_cast<std::int32_t>(value);
}

float stored(L value) {
    return static_cast<float>(value);
}

float camera_k(const WorldObject& o) {
    if (o.layer >= 2 && o.layer <= 4 && static_cast<int>(o.state) < 4) {
        return chicken_k[static_cast<std::size_t>(o.layer - 2)];
    }

    switch (o.state) {
        case MotionState::arc_left:
        case MotionState::arc_right:
        case MotionState::fly_left:
        case MotionState::fly_right:
            throw std::invalid_argument("Chicken layer must be 2..4");
        case MotionState::leaf_near:
        case MotionState::leaf_near_hit:
            return 2.8F;
        case MotionState::tiny:
            return 1.3F;
        case MotionState::balloon_left:
        case MotionState::balloon_right:
        case MotionState::balloon_hit:
        case MotionState::plane:
        case MotionState::plane_hit:
        case MotionState::banner:
        case MotionState::banner_hit:
        case MotionState::dropped_banner:
            return 1.6F;
        case MotionState::wing:
        case MotionState::hat:
        case MotionState::scare:
        case MotionState::windmill:
        case MotionState::wing_hit:
        case MotionState::hat_swap_hit:
            return 1.9F;
        default:
            if (static_cast<int>(o.state) >= 20 && static_cast<int>(o.state) <= 31) {
                return chicken_k[static_cast<std::size_t>((static_cast<int>(o.state) - 20) / 4)];
            }

            return 2.5F;
    }
}

L camera_offset(const WorldSnapshot& world, float k) {
    return trunc_int(static_cast<L>(world.camera) * k);
}

void position(WorldObject& o, const WorldSnapshot& world, L x, L y, float k) {
    o.screen_x = stored(x - camera_offset(world, k));
    o.screen_y = stored(y);
}

void asset(WorldObject& o, WorldAsset value) {
    const auto spec = object_spec(value);
    o.asset = value;
    o.frame_count = spec.frame_count;
    o.period_ms = spec.period_ms;
}

void effect(RoundContext& context, std::string_view name, const WorldObject& o) {
    context.events.emit(PresentationEffect{std::string(name), o.id, std::nullopt});
}

void retire(RoundContext& context, WorldObject& o, std::string_view sound = {}) {
    o.active = false;

    if (!sound.empty()) {
        effect(context, sound, o);
    }

    effect(context, "world.removed", o);
}

// Compare the live extended sum, then store; subtract at most one period.
bool animation_tick(WorldObject& o, float dt) {
    const L sum = static_cast<L>(o.animation_ms) + dt;
    o.animation_ms = stored(sum);

    if (sum < o.period_ms) {
        return false;
    }

    o.animation_ms = stored(sum - o.period_ms);

    return true;
}

void loop(WorldObject& o, float dt) {
    if (animation_tick(o, dt) && ++o.frame >= o.frame_count) {
        o.frame = 0;
    }
}

void clamp(WorldObject& o, float dt) {
    if (animation_tick(o, dt) && ++o.frame >= o.frame_count) {
        o.frame = o.frame_count - 1;
    }
}

int free_slot(const WorldSnapshot& world) {
    for (int slot = 4; slot < world.pool_limit; ++slot) {
        if (!world.objects[static_cast<std::size_t>(slot)].active) {
            return slot;
        }
    }

    return -1;
}

WorldObject& create(RoundContext& context, int slot, WorldAsset value, MotionState state, int layer, int score) {
    auto& o = context.round.world.objects[static_cast<std::size_t>(slot)];
    o = WorldObject{};
    asset(o, value);
    o.id = context.events.allocate_object();
    o.active = true;
    o.state = state;
    o.layer = layer;
    o.score = score;

    return o;
}

void publish(RoundContext& context) {
    context.round.camera = {context.round.world.camera, 0};
    context.round.objects.clear();

    for (const auto& o : context.round.world.objects) {
        if (o.active) {
            context.round.objects.push_back(
                {o.id, std::string(object_spec(o.asset).asset), {trunc_int(o.world_x), trunc_int(o.world_y)}, object_frame(o), {static_cast<std::int64_t>(o.animation_ms) * 1000}});
        }
    }
}

void initial_projection(WorldObject& o, const WorldSnapshot& world) {
    const L q = static_cast<L>(o.phase) * angle_scale;

    switch (o.state) {
        case MotionState::arc_left:
        case MotionState::arc_right: {
            const auto size = static_cast<std::size_t>(o.layer - 2);
            position(o, world, static_cast<L>(o.world_x) + stored(std::sin(q)) * static_cast<L>(amplitude[size]), static_cast<L>(o.world_y) + std::cos(q) * amplitude[size],
                chicken_k[size]);
            break;
        }

        case MotionState::tiny:
            position(o, world, static_cast<L>(o.world_x) + std::sin(q) * 25, static_cast<L>(o.world_y) + std::cos(q) * 5 + static_cast<L>(world.shake) * 0.2F, 1.3F);
            break;
        case MotionState::wing:
        case MotionState::hat:
        case MotionState::scare:
        case MotionState::windmill:
            position(o, world, o.world_x, static_cast<L>(o.world_y) + static_cast<L>(world.shake) * 0.6F, 1.9F);
            break;
        case MotionState::plane:
        case MotionState::banner:
            position(o, world, o.world_x, static_cast<L>(o.world_y) + static_cast<L>(world.shake) * 0.4F, 1.6F);
            break;
        case MotionState::leaf_near:
        case MotionState::leaf_far:
            position(o, world, o.world_x, static_cast<L>(o.world_y) + world.shake, camera_k(o));
            break;
        default:
            position(o, world, o.world_x, o.world_y, camera_k(o));
            break;
    }
}

void validate(const WorldSnapshot& w) {
    if (w.camera < 0 || w.camera > 1280 || w.pool_limit < 4 || w.pool_limit > 32 || w.shield_variant < 0 || w.shield_variant > 2) {
        throw std::invalid_argument("Invalid world camera/pool/shield state");
    }

    for (const auto& o : w.objects) {
        if (o.active) {
            if (!o.id || !drawable(o.state) || o.layer < 0 || o.layer > 5 || o.frame_count <= 0 || o.frame < 0 || o.frame >= o.frame_count || !std::isfinite(o.world_x)
                || !std::isfinite(o.world_y) || !std::isfinite(o.screen_x) || !std::isfinite(o.screen_y) || !std::isfinite(o.phase) || !std::isfinite(o.auxiliary)
                || !std::isfinite(o.animation_ms) || !std::isfinite(o.period_ms) || o.period_ms < 0) {
                throw std::invalid_argument("Invalid active world record");
            }
        }
    }
}
} // namespace

ObjectSpec object_spec(WorldAsset value) {
    const auto index = static_cast<std::size_t>(value);

    if (index >= specs.size()) {
        throw std::invalid_argument("Unknown world asset");
    }

    return specs[index];
}

FrameId object_frame(const WorldObject& o) {
    if (o.frame < 0) {
        throw std::invalid_argument("Negative world frame");
    }

    return {{std::string(object_spec(o.asset).asset)}, static_cast<std::uint32_t>(o.frame)};
}

bool drawable(MotionState state) noexcept {
    const auto code = static_cast<std::int32_t>(state);

    return (code >= 0 && code <= 15) || (code >= 20 && code <= 36) || code == 50 || code == 51 || (code >= 53 && code <= 56);
}

std::int32_t projected_integer(float value) {
    return trunc_int(value);
}

bool spawn_world(RoundContext& context, std::int32_t type) {
    auto& w = context.round.world;

    if (w.pool_limit < 4 || w.pool_limit > 32 || type < -1 || type > 7) {
        throw std::invalid_argument("Invalid spawn type/pool");
    }

    const auto slot = free_slot(w);

    if (slot < 0) {
        return false;
    }

    auto draw = [&] {
        return context.random.draw();
    };

    if (type < 0) {
        // Weighted spawn table; keep this draw before type-specific draws.
        constexpr std::array<int, 32> types{0, 1, 2, 0, 1, 2, 0, 1, 2, 0, 1, 2, 7, 5, 4, 3, 0, 1, 2, 0, 1, 2, 0, 1, 2, 0, 1, 2, 0, 1, 2, 0};
        type = types[draw() & 31];
    }

    if (type == 7 && w.plane_present) {
        return false;
    }

    if (type <= 2) {
        constexpr std::array<int, 4> layers{2, 2, 3, 4};
        const auto layer = layers[draw() & 3];
        const auto size = static_cast<std::size_t>(layer - 2);
        auto& o = create(context, slot, type == 1 ? static_cast<WorldAsset>(3 + size) : static_cast<WorldAsset>(size), static_cast<MotionState>(type), layer, chicken_score[size]);

        if (type < 2) {
            o.world_x = stored(static_cast<L>(draw() & 65535) * random_x[size]);
            o.world_y = arc_y[size];
            o.phase = type == 1 ? 0.1F : 0;
        } else {
            const bool right = (draw() & 1) != 0;
            o.state = right ? MotionState::fly_right : MotionState::fly_left;
            asset(o, static_cast<WorldAsset>((right ? 3 : 0) + size));
            o.world_x = stored((right ? -150 : 670) - static_cast<L>(trunc_int(static_cast<L>(w.camera) * -chicken_k[size])));
            o.world_y = layer == 2 ? static_cast<float>(256 - (draw() & 255)) : static_cast<float>(128 - (draw() & 127));
        }

        initial_projection(o, w);
        effect(context, "world.spawned", o);

        return true;
    }

    if (type == 3) {
        auto& o = create(context, slot, WorldAsset::big1, MotionState::close, 1, 25);
        o.world_x = stored(static_cast<L>(draw() & 65535) * random_x[0]);
        o.world_y = 225;
        initial_projection(o, w);
        effect(context, "world.spawned", o);
        effect(context, "world.sound.big.start", o);

        return true;
    }

    if (type == 4) {
        const bool right = (draw() & 1) != 0;
        auto& o = create(context, slot, WorldAsset::balloon, right ? MotionState::balloon_right : MotionState::balloon_left, 4, -25);
        o.world_x = stored((right ? -150 : 670) - static_cast<L>(trunc_int(static_cast<L>(w.camera) * -1.6F)));
        o.world_y = static_cast<float>(64 - (draw() & 63));
        initial_projection(o, w);
        effect(context, "world.spawned", o);

        return true;
    }

    if (type == 5 || type == 6) {
        float x;

        if (type == 5) {
            const auto direction = draw() & 1;
            auto& o = create(context, slot, WorldAsset::leaf, static_cast<MotionState>(8 + direction), static_cast<int>(direction), 0);
            x = stored(static_cast<L>(draw() & 65535) * random_x[0] - 256.0L);
            o.world_x = x;
            o.world_y = -static_cast<float>(draw() & 127) - 60;
            initial_projection(o, w);
            effect(context, "world.spawned", o);

            return true;
        }

        x = stored(static_cast<L>(draw() & 255) - trunc_int(static_cast<L>(w.camera) * -2.8F));
        const auto direction = draw() & 1;
        auto& o = create(context, slot, WorldAsset::leaf, static_cast<MotionState>(8 + direction), static_cast<int>(direction), 0);
        o.world_x = x;
        o.world_y = -60 - static_cast<float>(draw() & 127);
        o.frame = static_cast<int>(draw() & 15);
        initial_projection(o, w);
        effect(context, "world.spawned", o);

        return true;
    }

    auto& plane = create(context, slot, WorldAsset::plane, MotionState::plane, 4, -25);
    plane.world_x = stored(670 - static_cast<L>(trunc_int(static_cast<L>(w.camera) * -1.6F)));
    plane.world_y = static_cast<float>(64 - (draw() & 63));
    w.plane_present = true;
    const auto banner_slot = free_slot(w);

    if (banner_slot < 0) {
        plane.active = false;
        w.plane_present = false;

        return false;
    }

    auto& banner = create(context, banner_slot, WorldAsset::sign, MotionState::banner, 4, -25);
    banner.world_x = stored(748 - static_cast<L>(trunc_int(static_cast<L>(w.camera) * -1.6F)));
    banner.world_y = stored(static_cast<L>(plane.world_y) - 14);
    plane.linked_slot = banner_slot;
    initial_projection(plane, w);
    initial_projection(banner, w);
    effect(context, "world.spawned", plane);
    effect(context, "world.spawned", banner);
    effect(context, "world.sound.plane.start", plane);

    return true;
}

void initialize_world(RoundContext& context) {
    auto& w = context.round.world;
    w = WorldSnapshot{};
    w.shield_variant = context.persistent.shield_variant;
    context.persistent.cursor = {320, 240};
    create(context, 18, WorldAsset::scare, MotionState::scare, 3, 0).world_x = 600;
    w.objects[18].world_y = 190;
    create(context, 19, WorldAsset::hat, MotionState::hat, 3, 25).world_x = 600;
    w.objects[19].world_y = 155;
    create(context, 20, WorldAsset::windmill, MotionState::windmill, 3, 0).world_x = 2450;
    w.objects[20].world_y = 85;

    for (int i = 0; i < 4; ++i) {
        auto& o = create(context, 21 + i, WorldAsset::wing, MotionState::wing, 3, 25);
        o.world_x = 2450;
        o.world_y = 85;
        o.frame = 9 * i;
    }

    // Consume three draws per tiny resident before the two random spawns.
    for (int i = 0; i < 4; ++i) {
        auto& o = create(context, i, WorldAsset::chickie, MotionState::tiny, 0, 0);
        o.world_x = static_cast<float>(855 + (context.random.draw() & 7));
        o.world_y = static_cast<float>(148 + (context.random.draw() & 3));
        o.layer = 5 - static_cast<int>(context.random.draw() & 1);
        o.phase = stored(static_cast<L>(i) * 0.025F);
        o.frame = 0;
    }

    for (auto& o : w.objects) {
        if (o.active) {
            initial_projection(o, w);
        }
    }

    static_cast<void>(spawn_world(context));
    static_cast<void>(spawn_world(context));
    publish(context);
}

void camera_and_spawn_world(RoundContext& context) {
    auto& w = context.round.world;
    auto left = [&] {
        if (w.camera > 0) {
            w.camera -= 4;
        }
    };

    auto right = [&] {
        if (w.camera < 1279) {
            w.camera += 4;
        }
    };

    // Apply pointer steps before keyboard steps, including opposing inputs.
    if (context.persistent.cursor.x < 32) {
        left();
    }

    if (context.persistent.cursor.x >= 576) {
        right();
    }

    if (context.persistent.held[static_cast<std::size_t>(InputControl::left)]) {
        left();
    }

    if (context.persistent.held[static_cast<std::size_t>(InputControl::right)]) {
        right();
    }

    if (context.step.spawn_due) {
        static_cast<void>(spawn_world(context));
    }

    context.round.camera = {w.camera, 0};
}

void advance_world(RoundContext& context) {
    auto& w = context.round.world;
    validate(w);
    const auto dt = context.step.delta_ms;

    if (!std::isfinite(dt) || dt < 0) {
        throw std::invalid_argument("Invalid world step");
    }

    // Retirement branches still finish their animation update.
    for (auto& o : w.objects) {
        if (!o.active) {
            continue;
        }

        const auto code = static_cast<int>(o.state);
        const L q = static_cast<L>(o.phase) * angle_scale;

        if (code >= 20 && code <= 31) {
            const auto size = static_cast<std::size_t>((code - 20) / 4);
            const L x = static_cast<L>(o.world_x) + static_cast<L>(dt) * fall_velocity[size] * (code % 2 ? 1 : -1);
            const L y = static_cast<L>(o.world_y) + static_cast<L>(dt) * fall_y[size];
            o.world_x = stored(x);
            o.world_y = stored(y);
            const bool arc = (code - 20) % 4 < 2;
            const L screen_y = y + (arc ? std::cos(static_cast<L>(stored(q))) * amplitude[size] : 0);
            position(o, w, x + (arc ? std::sin(q) * amplitude[size] : 0), screen_y, chicken_k[size]);

            if (screen_y > fall_boundary[size]) {
                constexpr std::array<std::string_view, 3> sounds{"world.sound.fall.large", "world.sound.fall.medium", "world.sound.fall.small"};

                retire(context, o, sounds[size]);
            }

            clamp(o, dt);
            continue;
        }

        switch (o.state) {
            case MotionState::arc_left:
            case MotionState::arc_right: {
                const auto size = static_cast<std::size_t>(o.layer - 2);
                position(o, w, static_cast<L>(o.world_x) + static_cast<L>(stored(std::sin(q))) * amplitude[size], static_cast<L>(o.world_y) + std::cos(q) * amplitude[size],
                    chicken_k[size]);
                const L next = static_cast<L>(o.phase) + static_cast<L>(dt) * arc_velocity[size] * (code == 0 ? 1 : -1);
                o.phase = stored(next);

                if ((code == 0 && next >= 0.1F) || (code == 1 && next <= 0)) {
                    retire(context, o);
                }

                loop(o, dt);
                break;
            }

            case MotionState::fly_left:
            case MotionState::fly_right: {
                const auto size = static_cast<std::size_t>(o.layer - 2);
                const L x = static_cast<L>(o.world_x) + static_cast<L>(dt) * fly_velocity[size] * (code == 2 ? 1 : -1);
                o.world_x = stored(x);
                position(o, w, x, o.world_y, chicken_k[size]);
                // Smallest flying chickens compare Y for horizontal retirement.
                const L compared = size == 2 ? o.world_y : x;

                if ((code == 2 && compared > (size == 0 ? 4400 : size == 1 ? 3800 : 3200)) || (code == 3 && compared < -120)) {
                    retire(context, o);
                }

                loop(o, dt);
                break;
            }

            case MotionState::close: {
                const L phase = static_cast<L>(o.phase) + dt;
                o.phase = stored(phase);
                const L stage = phase * 0.002F;
                o.auxiliary = stored(stage);
                o.stage = trunc_int(stage);

                if (o.stage >= 0 && o.stage <= 3 && animation_tick(o, dt)) {
                    o.frame = std::min(o.frame + 1, 11);
                } else if (o.stage == 4) {
                    clamp(o, dt);
                } else if (o.stage == 5 && animation_tick(o, dt)) {
                    --o.frame;

                    if (o.frame == 11) {
                        o.frame = 12;
                    }
                } else if (o.stage >= 10 && o.stage <= 13 && animation_tick(o, dt)) {
                    if (--o.frame == -1) {
                        retire(context, o);
                    }
                }

                position(o, w, o.world_x, o.world_y, 2.5F);
                break;
            }

            case MotionState::peek:
                position(o, w, o.world_x, static_cast<L>(o.world_y) + w.shake, 2.5F);
                break;
            case MotionState::balloon_right:
            case MotionState::balloon_left: {
                const L x = static_cast<L>(o.world_x) + static_cast<L>(dt) * 0.025F * (code == 6 ? 1 : -1);
                o.world_x = stored(x);
                position(o, w, x, o.world_y, 1.6F);

                if ((code == 6 && x > 3200) || (code == 7 && x < -120)) {
                    retire(context, o);
                }

                if (animation_tick(o, dt)) {
                    if (o.ping_forward) {
                        ++o.frame;

                        if (o.frame == 20) {
                            o.ping_forward = false;
                        }
                    } else {
                        --o.frame;

                        if (o.frame == 0) {
                            o.ping_forward = true;
                        }
                    }
                }

                break;
            }

            case MotionState::leaf_near:
            case MotionState::leaf_far: {
                const float speed = code == 8 ? 0.1F : 0.05F;
                const L x = static_cast<L>(o.world_x) + static_cast<L>(dt) * speed;
                const L y = static_cast<L>(o.world_y) + static_cast<L>(dt) * speed;
                o.world_x = stored(x);
                o.world_y = stored(y);
                position(o, w, o.world_x, y + w.shake, code == 8 ? 2.8F : 2.5F);

                if (y + w.shake > 480) {
                    retire(context, o);
                }

                loop(o, dt);
                break;
            }

            case MotionState::wing:
            case MotionState::hat:
            case MotionState::scare:
            case MotionState::windmill:
                position(o, w, o.world_x, static_cast<L>(o.world_y) + static_cast<L>(w.shake) * 0.6F, 1.9F);

                if (o.state == MotionState::wing) {
                    loop(o, dt);
                }

                break;
            case MotionState::plane:
            case MotionState::banner: {
                const L x = static_cast<L>(o.world_x) - static_cast<L>(dt) * 0.15F;
                o.world_x = stored(x);
                position(o, w, x, static_cast<L>(o.world_y) + static_cast<L>(w.shake) * 0.4F, 1.6F);
                loop(o, dt);

                if (o.world_x < -120) {
                    if (o.state == MotionState::plane) {
                        w.plane_present = false;
                        effect(context, "world.sound.plane.stop", o);
                    }

                    retire(context, o);
                }

                break;
            }

            case MotionState::dropped_banner:
            case MotionState::plane_hit:
            case MotionState::banner_hit: {
                const float vx = o.state == MotionState::banner_hit ? 0.025F : 0.1F;
                const float vy = o.state == MotionState::banner_hit ? 0.025F : 0.05F;
                const L x = static_cast<L>(o.world_x) - static_cast<L>(dt) * vx;
                const L y = static_cast<L>(o.world_y) + static_cast<L>(dt) * vy;
                o.world_x = stored(x);
                o.world_y = stored(y);
                const L sy = (o.state == MotionState::banner_hit ? y : static_cast<L>(o.world_y)) + static_cast<L>(w.shake) * 0.4F;
                position(o, w, o.state == MotionState::banner_hit ? static_cast<L>(o.world_x) : x, sy, 1.6F);
                loop(o, dt);

                if ((o.state == MotionState::plane_hit && o.world_y > 275) || (o.state != MotionState::plane_hit && o.world_y > 350)) {
                    if (o.state == MotionState::plane_hit) {
                        w.plane_present = false;
                        effect(context, "world.sound.plane_hit.stop", o);
                        retire(context, o, "world.sound.explosion");
                    } else {
                        retire(context, o);
                    }
                }

                break;
            }

            case MotionState::close_hit:
                position(o, w, o.world_x, o.world_y, 2.5F);

                if (animation_tick(o, dt) && ++o.frame >= 17) {
                    retire(context, o, "world.sound.fall.close");
                }

                break;
            case MotionState::peek_hit:
                position(o, w, static_cast<L>(o.world_x) + std::sin(q) * 100, static_cast<L>(o.world_y) + std::cos(q) * 75 + w.shake, 2.5F);
                o.phase = stored(static_cast<L>(o.phase) - static_cast<L>(dt) * 0.0000375F);
                loop(o, dt);

                if (o.phase <= 0) {
                    retire(context, o);
                }

                break;
            case MotionState::balloon_hit: {
                const L y = static_cast<L>(o.world_y) + static_cast<L>(dt) * 0.075F;
                o.world_y = stored(y);
                position(o, w, o.world_x, y, 1.6F);
                clamp(o, dt);

                if (y > 300) {
                    effect(context, "world.sound.balloon.stop", o);
                    retire(context, o, "world.sound.fall.balloon");
                }

                break;
            }

            case MotionState::leaf_near_hit:
            case MotionState::leaf_far_hit: {
                const L x = static_cast<L>(o.world_x) + static_cast<L>(dt) * (code == 35 ? 0.075F : 0.025F);
                const L y = static_cast<L>(o.world_y) + static_cast<L>(dt) * (code == 35 ? 0.05F : 0.025F);
                o.world_x = stored(x);
                o.world_y = stored(y);
                position(o, w, code == 35 ? x : static_cast<L>(o.world_x), (code == 35 ? static_cast<L>(o.world_y) : y) + w.shake, code == 35 ? 2.8F : 2.5F);

                if (animation_tick(o, dt) && ++o.frame >= 25) {
                    retire(context, o);
                }

                break;
            }

            case MotionState::wing_hit:
            case MotionState::hat_swap_hit: {
                L x = o.world_x;

                if (o.state == MotionState::wing_hit) {
                    x += static_cast<L>(dt) * 0.1F;
                }

                const L y = static_cast<L>(o.world_y) + static_cast<L>(dt) * 0.1F;
                o.world_x = stored(x);
                o.world_y = stored(y);
                const L sy = y + static_cast<L>(w.shake) * 0.6F;
                position(o, w, o.world_x, sy, 1.9F);

                if (o.state == MotionState::hat_swap_hit) {
                    loop(o, dt);
                }

                if ((o.state == MotionState::wing_hit ? sy : static_cast<L>(o.screen_y)) > (o.state == MotionState::wing_hit ? 350 : 400)) {
                    retire(context, o, o.state == MotionState::wing_hit ? "world.sound.fall.medium" : "");
                }

                if (o.state == MotionState::wing_hit) {
                    loop(o, dt);
                }

                break;
            }

            case MotionState::tiny:
                position(o, w, static_cast<L>(o.world_x) + std::sin(q) * 25, static_cast<L>(o.world_y) + std::cos(q) * 5 + static_cast<L>(w.shake) * 0.2F, 1.3F);
                o.phase = stored(static_cast<L>(o.phase) - static_cast<L>(dt) * 0.000002F);
                o.frame = 0;
                break;
            default:
                throw std::logic_error("Missing active motion state");
        }
    }

    publish(context);
}

WorldHitChange apply_world_hit(WorldSnapshot& w, std::int32_t slot) {
    if (slot < 0 || slot >= 32) {
        throw std::invalid_argument("Hit slot outside world");
    }

    auto& o = w.objects[static_cast<std::size_t>(slot)];
    WorldHitChange change{false, slot, o.id, o.state, o.state};
    const auto code = static_cast<int>(o.state);
    // Artwork replacement reactivates even an inactive cached record.
    auto replace = [&](WorldAsset image, MotionState state) {
        asset(o, image);
        o.active = true;
        o.state = state;
        o.frame = 0;
    };

    if (code >= 0 && code <= 3 && o.layer >= 2 && o.layer <= 4) {
        const auto size = o.layer - 2;
        replace(static_cast<WorldAsset>(6 + size), static_cast<MotionState>(20 + size * 4 + code));
    } else if (o.layer == 1 && o.state == MotionState::close) {
        replace(WorldAsset::bighit, MotionState::close_hit);
    } else if (o.layer == 1 && o.state == MotionState::peek) {
        o.state = MotionState::peek_hit;
        o.phase = std::bit_cast<float>(0x3d9db22dU);
        o.world_x = stored(static_cast<L>(o.world_x) + 100);
    } else if (o.layer == 4 && (o.state == MotionState::balloon_left || o.state == MotionState::balloon_right)) {
        replace(WorldAsset::balloon2, MotionState::balloon_hit);
        o.world_x = stored(static_cast<L>(o.world_x) - 10);
    } else if (o.layer == 3 && o.state == MotionState::wing) {
        o.state = MotionState::wing_hit;
    } else if (o.layer == 3 && o.state == MotionState::hat) {
        // Popup placement precedes this swap; the selected identity moves to slot 18.
        std::swap(w.objects[18], w.objects[19]);
        w.objects[18].state = MotionState::hat_swap_hit;
    } else if (o.layer == 4 && o.state == MotionState::plane) {
        o.state = MotionState::plane_hit;

        if (o.linked_slot >= 0 && o.linked_slot < 32) {
            auto& banner = w.objects[static_cast<std::size_t>(o.linked_slot)];

            if (banner.state == MotionState::banner) {
                banner.state = MotionState::dropped_banner;
            }
        }
    } else if (o.layer == 4 && (o.state == MotionState::banner || o.state == MotionState::dropped_banner)) {
        o.state = MotionState::banner_hit;
    } else if (o.layer == 0 && o.state == MotionState::leaf_near) {
        replace(WorldAsset::leafhit, MotionState::leaf_near_hit);
    } else if (o.layer == 1 && o.state == MotionState::leaf_far) {
        replace(WorldAsset::leafhit, MotionState::leaf_far_hit);
    } else {
        return change;
    }

    change.changed = true;
    change.after = change.before == MotionState::hat ? MotionState::hat_swap_hit : w.objects[static_cast<std::size_t>(slot)].state;

    return change;
}

void quantize_world_position(WorldSnapshot& w, std::int32_t slot, ObjectId id) {
    if (slot < 0 || slot >= 32 || w.objects[static_cast<std::size_t>(slot)].id != id) {
        throw std::invalid_argument("Quantized slot identity changed");
    }

    auto& o = w.objects[static_cast<std::size_t>(slot)];
    o.screen_x = static_cast<float>(trunc_int(o.screen_x));
    o.screen_y = static_cast<float>(trunc_int(o.screen_y));
}

DrawList world_draw_list(const WorldSnapshot& w, const WorldDrawExtras& extras) {
    validate(w);
    std::vector<DrawCommand> commands;
    auto sprite = [&](std::string_view id, std::uint32_t frame, ScreenPoint dest, std::optional<AtlasRect> crop, std::optional<std::uint8_t> key) {
        commands.emplace_back(SpriteDraw{{{std::string(id)}, frame}, dest, SpriteBlend::copy, key, {}, crop, false, false}

        );
    };

    sprite("layer0", 0, {0, 0}, AtlasRect{w.camera, 0, 640, 260}, {});
    auto objects = [&](int layer) {
        for (const auto& o : w.objects) {
            if (o.active && o.layer == layer && drawable(o.state)) {
                sprite(object_spec(o.asset).asset, static_cast<std::uint32_t>(o.frame), {trunc_int(o.screen_x), trunc_int(o.screen_y)}, {}, 18);
            }
        }

        commands.insert(commands.end(), extras.after_objects[static_cast<std::size_t>(layer)].begin(), extras.after_objects[static_cast<std::size_t>(layer)].end());
    };

    objects(5);
    constexpr std::array<L, 5> k{static_cast<L>(1.3F), static_cast<L>(1.6F), 1.9L, static_cast<L>(2.2F), 2.5L};

    constexpr std::array<float, 5> sy{0.2F, 0.4F, 0.6F, 0.8F, 1};

    for (int layer = 1; layer <= 5; ++layer) {
        const auto index = static_cast<std::size_t>(layer - 1);
        // Background layer3 specifically multiplies a binary64 1.9 operand.
        const L factor = layer == 3 ? static_cast<L>(1.9) : k[index];
        const auto offset = trunc_int(static_cast<L>(w.camera) * factor);
        const auto first = offset / 64;
        const int y = trunc_int(static_cast<L>(w.shake) * sy[index]) + (layer == 5 ? -8 : 0);

        for (int strip = first; strip * 64 - offset < 640; ++strip) {
            if (strip < 0 || strip >= 60) {
                throw std::invalid_argument("Landscape strip outside repeated source");
            }

            const int source = (strip % 30) * 64;

            if (layer == 5 && strip >= 24 && strip <= 27) {
                sprite("shield", 0, {strip * 64 - offset, y}, AtlasRect{w.shield_variant * 256 + (strip - 24) * 64, 0, 64, 488}, 18);
            } else {
                sprite("layer" + std::to_string(layer), 0, {strip * 64 - offset, y}, AtlasRect{source, 0, 64, layer == 5 ? 488 : 480}, 18);
            }
        }

        if (layer == 5) {
            commands.insert(commands.end(), extras.tree_holes.begin(), extras.tree_holes.end());
        }

        objects(5 - layer);
    }

    return DrawList{std::move(commands)};
}

std::vector<HitCandidate> world_hit_candidates(const WorldSnapshot& w, std::span<const AssetView> assets) {
    validate(w);
    std::vector<HitCandidate> result;
    result.reserve(32);

    for (std::size_t slot = 0; slot < w.objects.size(); ++slot) {
        const auto& o = w.objects[slot];
        // An inactive close record retains frame -1; selectors cannot render it.
        const auto frame = o.frame < 0 ? FrameId{{std::string(object_spec(o.asset).asset)}, 0} : object_frame(o);
        const auto image = std::ranges::find_if(assets, [&](const auto& a) {
            return a.id == frame.image;
        });

        if (o.active && image == assets.end()) {
            throw std::invalid_argument("World candidate image unavailable");
        }

        result.push_back({static_cast<std::uint32_t>(slot), o.id, std::string(object_spec(o.asset).asset), static_cast<std::int32_t>(o.state), o.active, frame,
            image == assets.end() ? nullptr : &*image, o.screen_x, o.screen_y, {0, 0}, false, false});
    }

    return result;
}

IndexedWorldAssets::IndexedWorldAssets(std::span<const AssetView> assets) : indices_(static_cast<std::size_t>(contracts::logical_width) * contracts::logical_height) {
    images_.reserve(assets.size());

    for (const auto& source : assets) {
        if (source.width <= 0 || source.height <= 0 || source.row_stride < static_cast<std::uint32_t>(source.width)) {
            throw std::invalid_argument("Invalid indexed world atlas dimensions");
        }

        const auto size = static_cast<std::uint64_t>(source.height - 1) * source.row_stride + static_cast<std::uint32_t>(source.width);

        if (size > source.palette_indices.size()) {
            throw std::invalid_argument("Indexed world atlas is truncated");
        }

        if (std::ranges::any_of(images_, [&](const auto& a) {
                return a.id == source.id;
            })) {
            throw std::invalid_argument("Duplicate indexed world asset");
        }

        Image image{source.id, source.width, source.height, source.row_stride, {source.frames.begin(), source.frames.end()}, {},
            {source.palette_indices.begin(), source.palette_indices.end()}};

        image.rgba.reserve(image.indices.size());
        // Encode indices as gray so the shared renderer preserves selector bytes.
        for (const auto index : image.indices) {
            image.rgba.push_back({index, index, index, 255});
        }

        images_.push_back(std::move(image));
    }
}

const std::vector<std::uint8_t>& IndexedWorldAssets::compose(const DrawList& draws, std::optional<AtlasRect> region) const {
    std::vector<AssetView> views;
    views.reserve(images_.size());

    for (const auto& a : images_) {
        views.push_back({a.id, a.width, a.height, a.stride, a.frames, a.rgba, a.indices, {}});
    }

    render::RenderOptions options;
    options.region = region;
    const auto& pixels = renderer_.render(draws, views, options).pixels();
    int left{};
    int top{};
    int right{contracts::logical_width};
    int bottom{contracts::logical_height};

    if (region) {
        const auto bounded = [](std::int64_t value, int maximum) {
            return static_cast<int>(std::clamp(value, std::int64_t{0}, static_cast<std::int64_t>(maximum)));
        };
        left = bounded(region->x, contracts::logical_width);
        top = bounded(region->y, contracts::logical_height);
        right = bounded(static_cast<std::int64_t>(region->x) + region->width, contracts::logical_width);
        bottom = bounded(static_cast<std::int64_t>(region->y) + region->height, contracts::logical_height);

        if (region->width <= 0 || region->height <= 0) {
            right = left;
            bottom = top;
        }
    }

    for (int y = top; y < bottom; ++y) {
        for (int x = left; x < right; ++x) {
            const auto offset = static_cast<std::size_t>(y) * contracts::logical_width + x;
            indices_[offset] = pixels[offset].r;
        }
    }

    return indices_;
}

void validate_world_assets(std::span<const AssetView> assets) {
    auto full_frame = [&](std::string_view name, int width, int height) {
        const auto a = std::ranges::find_if(assets, [&](const auto& v) {
            return v.id.value == name;
        });

        if (a == assets.end() || a->frames.empty() || a->frames[0].width != width || a->frames[0].height != height) {
            throw std::invalid_argument("World landscape dimensions differ: " + std::string(name));
        }
    };

    full_frame("layer0", 1920, 260);

    for (int layer = 1; layer <= 5; ++layer) {
        full_frame("layer" + std::to_string(layer), 1920, layer == 5 ? 488 : 480);
    }

    full_frame("shield", 768, 488);

    for (const auto& spec : specs) {
        const auto a = std::ranges::find_if(assets, [&](const auto& v) {
            return v.id.value == spec.asset;
        });

        if (a == assets.end() || a->frames.size() < static_cast<std::size_t>(spec.frame_count)) {
            throw std::invalid_argument("World active frames unavailable: " + std::string(spec.asset));
        }

        for (int i = 0; i < spec.frame_count; ++i) {
            const auto frame = a->frames[static_cast<std::size_t>(i)];

            if (frame.width != spec.width || frame.height != spec.height) {
                throw std::invalid_argument("World frame dimensions differ: " + std::string(spec.asset));
            }
        }
    }
}

} // namespace moorhuhn::game
