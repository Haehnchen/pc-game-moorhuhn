#include "game/combat.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>
#include <string_view>

namespace moorhuhn::game {
namespace {
using namespace contracts;
constexpr std::uint8_t sprite_key = 18;

void require(bool condition, std::string_view message) {
    if (!condition) {
        throw std::invalid_argument(std::string(message));
    }
}

void scratch_ready(PersistentState& persistent) {
    require(persistent.selector_state.empty() || persistent.selector_state.size() == selector_buffer_size, "Invalid selector carrier size");

    if (persistent.selector_state.empty()) {
        persistent.selector_state.assign(selector_buffer_size, 0);
    }
}

auto records(std::span<const HitCandidate> candidates) {
    std::array<const HitCandidate*, 32> slots{};

    for (const auto& candidate : candidates) {
        require(candidate.slot < slots.size() && !slots[candidate.slot], "Invalid or duplicate candidate slot");
        require(!candidate.active || candidate.id != 0, "Active candidate needs an object ID");
        slots[candidate.slot] = &candidate;
    }

    return slots;
}

HitResult target(const HitCandidate& candidate, ScreenPoint point) {
    return {candidate.slot, candidate.id, candidate.kind, candidate.state, candidate.frame, point};
}

std::int32_t truncate(float value) {
    require(std::isfinite(value) && static_cast<double>(value) >= INT32_MIN && static_cast<double>(value) < 2147483648.0, "Invalid projected coordinate");

    return static_cast<std::int32_t>(value);
}

struct Geometry {
    const AssetView* image{};
    AtlasRect frame;
    std::int32_t x{};
    std::int32_t y{};
    std::int64_t left{};
    std::int64_t top{};
};

Geometry geometry(const HitCandidate& candidate) {
    require(candidate.image && candidate.frame.image == candidate.image->id, "Candidate frame/image mismatch");
    const auto& image = *candidate.image;
    require(candidate.frame.index < image.frames.size(), "Candidate frame out of range");
    require(image.width > 0 && image.height > 0 && image.row_stride >= static_cast<std::uint32_t>(image.width), "Invalid indexed image geometry");
    const auto extent = static_cast<std::uint64_t>(image.row_stride) * static_cast<std::uint64_t>(image.height - 1) + static_cast<std::uint64_t>(image.width);
    require(extent <= image.palette_indices.size(), "Candidate palette indices unavailable");
    const auto frame = image.frames[candidate.frame.index];
    require(frame.x >= 0 && frame.y >= 0 && frame.width > 0 && frame.height > 0 && static_cast<std::int64_t>(frame.x) + frame.width <= image.width
                && static_cast<std::int64_t>(frame.y) + frame.height <= image.height,
        "Invalid candidate frame rectangle");
    require(static_cast<std::uint64_t>(frame.width) * static_cast<std::uint64_t>(frame.height) <= selector_buffer_size, "Candidate exceeds selector scratch");
    const auto x = truncate(candidate.screen_x);
    const auto y = truncate(candidate.screen_y);

    return {&image, frame, x, y, static_cast<std::int64_t>(x) - candidate.anchor.x, static_cast<std::int64_t>(y) - candidate.anchor.y};
}

ScreenPoint source_pixel(const Geometry& geometry, std::int32_t x, std::int32_t y, const HitCandidate& candidate) {
    return {candidate.mirror_x ? geometry.frame.width - 1 - x : x, candidate.mirror_y ? geometry.frame.height - 1 - y : y};
}

void draw_candidate(std::vector<std::uint8_t>& scratch, const Geometry& geometry, const HitCandidate& candidate) {
    const auto width = static_cast<std::size_t>(geometry.frame.width);
    const auto height = static_cast<std::size_t>(geometry.frame.height);
    // Clear one contiguous prefix; transparent pixels retain the uncleared tail.
    // A 30x20 frame keeps its final 40 bytes across candidates and rounds.
    const auto cleared = height * 4 * (width / 4);
    std::fill_n(scratch.begin(), cleared, sprite_key);

    for (std::int32_t y = 0; y < geometry.frame.height; ++y) {
        for (std::int32_t x = 0; x < geometry.frame.width; ++x) {
            const auto source = source_pixel(geometry, x, y, candidate);
            const auto offset = static_cast<std::size_t>(geometry.frame.y + source.y) * geometry.image->row_stride + static_cast<std::size_t>(geometry.frame.x + source.x);
            const auto pixel = geometry.image->palette_indices[offset];

            if (pixel != sprite_key) {
                scratch[static_cast<std::size_t>(y) * width + static_cast<std::size_t>(x)] = pixel;
            }
        }
    }
}
} // namespace

ScreenPoint selector_hotspot(ScreenPoint cursor) {
    require(cursor.x >= 0 && cursor.x < logical_width && cursor.y >= 0 && cursor.y < logical_height, "Invalid logical cursor");

    return {cursor.x + 16, cursor.y + 16};
}

void initialize_combat(RoundContext& context) {
    scratch_ready(context.persistent);
    context.round.magazine = {8, 8, false};
}

CombatResult process_combat(RoundContext& context, std::span<const HitCandidate> candidates, LoadedShotHandler* handler) {
    auto& magazine = context.round.magazine;
    require(magazine.capacity == 8 && magazine.loaded <= 8 && !magazine.reloading, "Invalid magazine state");
    require(context.step.action_mask <= 3, "Invalid mouse action mask");
    require(context.persistent.cached_target_slot >= -1 && context.persistent.cached_target_slot < 32, "Invalid cached slot");
    const auto slots = records(candidates);
    CombatResult result;
    result.point = selector_hotspot(context.persistent.cursor);
    result.cached_slot = context.persistent.cached_target_slot;
    // Only exact masks 1 and 2 act; combined presses do nothing.
    if (context.step.action_mask == 1) {
        if (magazine.loaded == 0) {
            result.action = CombatAction::empty;
            context.events.emit(PlaySound{{"empty22"}});
            context.events.emit(PresentationEffect{"combat.empty", std::nullopt, result.point});
        } else {
            result.action = CombatAction::shot;
            result.shake = 8;
            result.ejected_shell = 8 - magazine.loaded;

            if (result.cached_slot != -1) {
                const auto* candidate = slots[static_cast<std::size_t>(result.cached_slot)];

                if (candidate && candidate->id != 0) {
                    result.hit = target(*candidate, result.point);
                }
            }

            context.events.emit(PlaySound{{"shoot22"}});
            context.events.emit(PresentationEffect{"combat.shot", result.hit ? std::optional{result.hit->id} : std::nullopt, result.point}

            );

            if (handler) {
                handler->loaded_shot(context, result);
            }

            require(magazine.capacity == 8 && magazine.loaded == 8 - *result.ejected_shell && !magazine.reloading, "Loaded-shot handler changed magazine state");
            --magazine.loaded;
        }
    } else if (context.step.action_mask == 2 && magazine.loaded == 0) {
        result.action = CombatAction::reloaded;
        context.events.emit(PlaySound{{"reload22"}});
        context.events.emit(PresentationEffect{"combat.reload", std::nullopt, result.point});
        magazine.loaded = 8;
    }

    return result;
}

SelectionResult select_indexed_target(PersistentState& persistent, ScreenPoint cursor, IndexedScene scene, std::span<const HitCandidate> candidates) {
    const auto point = selector_hotspot(cursor);
    require(
        scene.row_stride >= static_cast<std::uint32_t>(logical_width) && static_cast<std::uint64_t>(scene.row_stride) * (logical_height - 1) + logical_width <= scene.pixels.size(),
        "Invalid composed indexed scene");
    const auto slots = records(candidates);
    scratch_ready(persistent);
    SelectionResult result{point, std::nullopt, {}};

    if (cursor.y > 463 || point.x >= logical_width || point.y >= logical_height) {
        persistent.cached_target_slot = -1;

        return result;
    }

    for (const auto* candidate : slots) {
        if (!candidate || !candidate->active || candidate->state >= 20) {
            continue;
        }

        const auto shape = geometry(*candidate);

        if (!(shape.left < point.x && point.x < shape.left + shape.frame.width && shape.top < point.y && point.y < shape.top + shape.frame.height)) {
            continue;
        }

        draw_candidate(persistent.selector_state, shape, *candidate);
        result.positions.push_back({candidate->slot, candidate->id, static_cast<float>(shape.x), static_cast<float>(shape.y)});
        const auto x = static_cast<std::int32_t>(point.x - shape.left), y = static_cast<std::int32_t>(point.y - shape.top);
        const auto pixel = persistent.selector_state[static_cast<std::size_t>(y) * shape.frame.width + static_cast<std::size_t>(x)];
        const auto scene_pixel = scene.pixels[static_cast<std::size_t>(point.y) * scene.row_stride + static_cast<std::size_t>(point.x)];
        // Equal-index foreground pixels can leave a target selectable.
        if (pixel != sprite_key && pixel == scene_pixel) {
            result.hit = IndexedHit{target(*candidate, point), source_pixel(shape, x, y, *candidate), pixel, scene_pixel};

            persistent.cached_target_slot = static_cast<std::int32_t>(candidate->slot);

            return result;
        }
    }

    persistent.cached_target_slot = -1;

    return result;
}
} // namespace moorhuhn::game
