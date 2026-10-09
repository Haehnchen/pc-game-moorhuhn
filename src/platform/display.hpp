#pragma once

#include "platform/viewport.hpp"

#include <SDL3/SDL_render.h>
#include <SDL3/SDL_video.h>

#include <cstdint>
#include <memory>
#include <span>
#include <string>

namespace moorhuhn::platform {

// Own the SDL process lifetime. Construct before all SDL resources.
class SdlRuntime {
public:
    SdlRuntime();
    ~SdlRuntime();
    SdlRuntime(const SdlRuntime&) = delete;
    SdlRuntime& operator=(const SdlRuntime&) = delete;
};

struct DisplayOptions {
    std::string title{"Moorhuhn"};
    int width{contracts::logical_width};
    int height{contracts::logical_height};
    bool hidden{};
    bool fullscreen{};
    bool hide_cursor{};
};

class Display {
public:
    explicit Display(const DisplayOptions& options = {});
    ~Display();
    void refresh();
    // Present a tightly packed 640 x 480 RGBA buffer. This does not update game state.
    void present(std::span<const std::uint8_t> rgba);

    [[nodiscard]] const Viewport& viewport() const noexcept {
        return viewport_;
    }

    [[nodiscard]] bool minimized() const noexcept {
        return minimized_;
    }

    [[nodiscard]] bool vsync() const noexcept {
        return vsync_;
    }

    [[nodiscard]] SDL_Window* window() const noexcept {
        return window_.get();
    }

    [[nodiscard]] SDL_Renderer* renderer() const noexcept {
        return renderer_.get();
    }

private:
    std::unique_ptr<SDL_Window, decltype(&SDL_DestroyWindow)> window_{nullptr, SDL_DestroyWindow};
    std::unique_ptr<SDL_Renderer, decltype(&SDL_DestroyRenderer)> renderer_{nullptr, SDL_DestroyRenderer};

    std::unique_ptr<SDL_Texture, decltype(&SDL_DestroyTexture)> texture_{nullptr, SDL_DestroyTexture};

    Viewport viewport_{};
    bool minimized_{};
    bool vsync_{};
    bool restore_cursor_{};
};

[[noreturn]] void throw_sdl_error(const char* operation);

} // namespace moorhuhn::platform
