#pragma once

#include "contracts/render.hpp"
#include "render/framebuffer.hpp"

#include <optional>
#include <stdexcept>

namespace moorhuhn::render {

struct RenderOptions {
    contracts::RgbaPixel clear_color{0, 0, 0, 255};
    std::optional<contracts::AtlasRect> region{};
};

class RenderError final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class Renderer {
public:
    // Read commands and assets in order without changing game state.
    // Fills copy RGBA. Sprites use explicit copy or straight-alpha source over.
    // Validate all commands before replacing the last completed framebuffer.
    [[nodiscard]] const Framebuffer& render(const contracts::DrawList& draws, std::span<const contracts::AssetView> assets, RenderOptions options = {});

    [[nodiscard]] const Framebuffer& framebuffer() const noexcept {
        return framebuffer_;
    }

    [[nodiscard]] Framebuffer snapshot() const {
        return framebuffer_;
    }

private:
    Framebuffer framebuffer_;
};

} // namespace moorhuhn::render
