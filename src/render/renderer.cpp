#include "render/renderer.hpp"

#include "render/lookup.hpp"

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace moorhuhn::render {
namespace {

using namespace contracts;

struct Clip {
    int left{};
    int top{};
    int right{};
    int bottom{};
};

Clip clip(ScreenPoint destination, int width, int height) {
    auto bounded = [](std::int64_t value, int maximum) {
        return static_cast<int>(std::clamp(value, std::int64_t{0}, static_cast<std::int64_t>(maximum)));
    };

    return {bounded(destination.x, Framebuffer::width), bounded(destination.y, Framebuffer::height), bounded(static_cast<std::int64_t>(destination.x) + width, Framebuffer::width),
        bounded(static_cast<std::int64_t>(destination.y) + height, Framebuffer::height)};
}

Clip restrict_to_region(Clip bounds, const std::optional<AtlasRect>& region) {
    if (!region) {
        return bounds;
    }

    if (region->width <= 0 || region->height <= 0) {
        return {};
    }

    const auto region_bounds = clip({region->x, region->y}, region->width, region->height);
    bounds.left = std::max(bounds.left, region_bounds.left);
    bounds.top = std::max(bounds.top, region_bounds.top);
    bounds.right = std::min(bounds.right, region_bounds.right);
    bounds.bottom = std::min(bounds.bottom, region_bounds.bottom);
    return bounds;
}

std::uint64_t storage_size(int width, int height, std::uint32_t stride) {
    if (width <= 0 || height <= 0 || stride < static_cast<std::uint32_t>(width)) {
        throw RenderError("Invalid image dimensions or row stride");
    }

    return static_cast<std::uint64_t>(height - 1) * stride + static_cast<std::uint32_t>(width);
}

struct ResolvedSprite {
    const SpriteDraw* draw{};
    const AssetView* asset{};
    AtlasRect frame{};
    AtlasRect source{}; // Frame-relative crop.
    const MaskView* mask{};
};

using AssetLookup = std::unordered_map<std::string_view, const AssetView*>;

ResolvedSprite resolve(const SpriteDraw& draw, const AssetLookup& assets) {
    if (draw.blend != SpriteBlend::source_over && draw.blend != SpriteBlend::copy) {
        throw RenderError("Unknown sprite blend operation");
    }

    const auto asset = assets.find(draw.frame.image.value);

    if (asset == assets.end()) {
        throw RenderError("Unknown image ID");
    }

    const auto* view = asset->second;

    if (draw.frame.index >= view->frames.size()) {
        throw RenderError("Frame index is unavailable");
    }

    const auto required = storage_size(view->width, view->height, view->row_stride);

    if (required > view->rgba.size()) {
        throw RenderError("RGBA storage is truncated");
    }

    if (draw.transparent_index && required > view->palette_indices.size()) {
        throw RenderError("Index storage is unavailable or truncated");
    }

    const auto frame = view->frames[draw.frame.index];

    if (frame.x < 0 || frame.y < 0 || frame.width <= 0 || frame.height <= 0 || static_cast<std::int64_t>(frame.x) + frame.width > view->width
        || static_cast<std::int64_t>(frame.y) + frame.height > view->height) {
        throw RenderError("Frame rectangle is outside the atlas");
    }

    const auto source = draw.source_rect.value_or(AtlasRect{0, 0, frame.width, frame.height});

    if (source.x < 0 || source.y < 0 || source.width <= 0 || source.height <= 0 || static_cast<std::int64_t>(source.x) + source.width > frame.width
        || static_cast<std::int64_t>(source.y) + source.height > frame.height) {
        throw RenderError("Source rectangle is outside the frame");
    }

    const MaskView* mask{};

    if (draw.mask) {
        if (draw.mask->mode != MaskMode::index_key && draw.mask->mode != MaskMode::coverage) {
            throw RenderError("Unknown mask operation");
        }

        if (draw.mask->index >= view->masks.size()) {
            throw RenderError("Mask index is unavailable");
        }

        mask = &view->masks[draw.mask->index];

        if (mask->width != frame.width || mask->height != frame.height || storage_size(mask->width, mask->height, mask->row_stride) > mask->indices.size()) {
            throw RenderError("Mask dimensions or storage differ from the frame");
        }
    }

    return {&draw, view, frame, source, mask};
}

} // namespace

const Framebuffer& Renderer::render(const DrawList& draws, std::span<const AssetView> assets, RenderOptions options) {
    AssetLookup ids;
    ids.reserve(assets.size());

    for (const auto& asset : assets) {
        if (!ids.emplace(asset.id.value, &asset).second) {
            throw RenderError("Duplicate image ID: " + asset.id.value);
        }
    }

    // Resolve and validate every command before changing the framebuffer.
    std::vector<std::optional<ResolvedSprite>> resolved;
    resolved.reserve(draws.commands().size());
    std::size_t command_index{};

    for (const auto& command : draws.commands()) {
        try {
            if (const auto* sprite = std::get_if<SpriteDraw>(&command)) {
                resolved.emplace_back(resolve(*sprite, ids));
            } else {
                const auto& fill = std::get<FillDraw>(command);

                if (fill.width < 0 || fill.height < 0) {
                    throw RenderError("Fill dimensions cannot be negative");
                }

                resolved.emplace_back(std::nullopt);
            }
        } catch (const RenderError& error) {
            std::string context = "Draw command " + std::to_string(command_index);

            if (const auto* sprite = std::get_if<SpriteDraw>(&command)) {
                context += " image '" + sprite->frame.image.value + "' frame " + std::to_string(sprite->frame.index);
            }

            throw RenderError(context + ": " + error.what());
        }

        ++command_index;
    }

    std::fill(framebuffer_.pixels_.begin(), framebuffer_.pixels_.end(), options.clear_color);
    command_index = 0;

    for (const auto& command : draws.commands()) {
        if (const auto* fill = std::get_if<FillDraw>(&command)) {
            const auto bounds = restrict_to_region(clip(fill->destination, fill->width, fill->height), options.region);

            for (int y = bounds.top; y < bounds.bottom; ++y) {
                for (int x = bounds.left; x < bounds.right; ++x) {
                    framebuffer_.pixels_[static_cast<std::size_t>(y) * Framebuffer::width + x] = fill->color;
                }
            }
        } else {
            const auto& sprite = *resolved[command_index];
            const auto& draw = *sprite.draw;
            const auto bounds = restrict_to_region(clip(draw.destination, sprite.source.width, sprite.source.height), options.region);

            if (draw.blend == SpriteBlend::copy && !sprite.mask && !draw.mirror_x && !draw.mirror_y) {
                // Landscape copies need only a row offset and optional index rejection.
                if (bounds.left < bounds.right && bounds.top < bounds.bottom) {
                    const auto source_x = sprite.frame.x + sprite.source.x + static_cast<int>(static_cast<std::int64_t>(bounds.left) - draw.destination.x);
                    const auto source_y = sprite.frame.y + sprite.source.y + static_cast<int>(static_cast<std::int64_t>(bounds.top) - draw.destination.y);
                    const auto width = bounds.right - bounds.left;

                    for (int y = bounds.top; y < bounds.bottom; ++y) {
                        const auto offset = static_cast<std::size_t>(source_y + (y - bounds.top)) * sprite.asset->row_stride + source_x;
                        const auto* source = sprite.asset->rgba.data() + offset;
                        auto* destination = framebuffer_.pixels_.data() + static_cast<std::size_t>(y) * Framebuffer::width + bounds.left;

                        if (draw.transparent_index) {
                            const auto* indices = sprite.asset->palette_indices.data() + offset;

                            for (int x = 0; x < width; ++x) {
                                if (indices[x] != *draw.transparent_index) {
                                    destination[x] = source[x];
                                }
                            }
                        } else {
                            std::copy_n(source, width, destination);
                        }
                    }
                }
            } else {
                for (int y = bounds.top; y < bounds.bottom; ++y) {
                    for (int x = bounds.left; x < bounds.right; ++x) {
                        const auto local_x = static_cast<int>(static_cast<std::int64_t>(x) - draw.destination.x);
                        const auto local_y = static_cast<int>(static_cast<std::int64_t>(y) - draw.destination.y);
                        const auto source_x = sprite.source.x + (draw.mirror_x ? sprite.source.width - 1 - local_x : local_x);
                        const auto source_y = sprite.source.y + (draw.mirror_y ? sprite.source.height - 1 - local_y : local_y);
                        const auto offset = static_cast<std::size_t>(sprite.frame.y + source_y) * sprite.asset->row_stride + sprite.frame.x + source_x;
                        auto source = sprite.asset->rgba[offset];
                        std::uint32_t coverage = 255;
                        // Apply the draw's index key; alpha remains a separate blend input.
                        if (draw.transparent_index && sprite.asset->palette_indices[offset] == *draw.transparent_index) {
                            coverage = 0;
                        }

                        if (sprite.mask) {
                            const auto mask_offset = static_cast<std::size_t>(source_y) * sprite.mask->row_stride + source_x;
                            const auto value = sprite.mask->indices[mask_offset];
                            coverage = draw.mask->mode == MaskMode::coverage ? (coverage * value + 127) / 255 : (value == draw.mask->transparent_index ? 0 : coverage);
                        }

                        auto& destination = framebuffer_.pixels_[static_cast<std::size_t>(y) * Framebuffer::width + x];

                        if (coverage != 0) {
                            source.a = static_cast<std::uint8_t>((source.a * coverage + 127) / 255);
                            destination = draw.blend == SpriteBlend::copy ? source : source_over(source, destination);
                        }
                    }
                }
            }
        }

        ++command_index;
    }

    return framebuffer_;
}

} // namespace moorhuhn::render
