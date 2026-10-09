#include "platform/display.hpp"

#include <SDL3/SDL.h>

#include <stdexcept>

namespace moorhuhn::platform {

void throw_sdl_error(const char* operation) {
    throw std::runtime_error(std::string(operation) + ": " + SDL_GetError());
}

SdlRuntime::SdlRuntime() {
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        const std::string cause = SDL_GetError();
        SDL_Quit();
        throw std::runtime_error("SDL_Init: " + cause);
    }
}

SdlRuntime::~SdlRuntime() {
    SDL_Quit();
}

Display::Display(const DisplayOptions& options) {
    if (options.width <= 0 || options.height <= 0) {
        throw std::invalid_argument("Window dimensions must be positive");
    }

    SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;

    if (options.hidden) {
        flags |= SDL_WINDOW_HIDDEN;
    }

    if (options.fullscreen) {
        flags |= SDL_WINDOW_FULLSCREEN;
    }

    window_.reset(SDL_CreateWindow(options.title.c_str(), options.width, options.height, flags));

    if (!window_) {
        throw_sdl_error("SDL_CreateWindow");
    }

    renderer_.reset(SDL_CreateRenderer(window_.get(), nullptr));

    if (!renderer_) {
        throw_sdl_error("SDL_CreateRenderer");
    }

    vsync_ = SDL_SetRenderVSync(renderer_.get(), 1);

    texture_.reset(SDL_CreateTexture(renderer_.get(), SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING, contracts::logical_width, contracts::logical_height));

    if (!texture_) {
        throw_sdl_error("SDL_CreateTexture");
    }

    if (!SDL_SetTextureScaleMode(texture_.get(), SDL_SCALEMODE_NEAREST) || !SDL_SetTextureBlendMode(texture_.get(), SDL_BLENDMODE_NONE)) {
        throw_sdl_error("Configure presentation texture");
    }

    if (options.fullscreen && !options.hidden && !SDL_SyncWindow(window_.get())) {
        throw_sdl_error("Wait for fullscreen window");
    }

    refresh();

    if (options.hide_cursor && SDL_CursorVisible()) {
        if (!SDL_HideCursor()) {
            throw_sdl_error("Hide system cursor");
        }

        restore_cursor_ = true;
    }
}

Display::~Display() {
    if (restore_cursor_) {
        static_cast<void>(SDL_ShowCursor());
    }
}

void Display::refresh() {
    int window_width{};
    int window_height{};
    int pixel_width{};
    int pixel_height{};

    if (!SDL_GetWindowSize(window_.get(), &window_width, &window_height) || !SDL_GetRenderOutputSize(renderer_.get(), &pixel_width, &pixel_height)) {
        throw_sdl_error("Read window dimensions");
    }

    minimized_ = (SDL_GetWindowFlags(window_.get()) & SDL_WINDOW_MINIMIZED) != 0;
    viewport_ = Viewport::fit(window_width, window_height, minimized_ ? 0 : pixel_width, minimized_ ? 0 : pixel_height);
}

void Display::present(std::span<const std::uint8_t> rgba) {
    constexpr std::size_t expected = static_cast<std::size_t>(contracts::logical_width) * contracts::logical_height * 4;

    if (rgba.size() != expected) {
        throw std::invalid_argument("Presentation requires 640 x 480 RGBA bytes");
    }

    refresh();

    if (!viewport_.drawable()) {
        return;
    }

    const SDL_FRect destination{static_cast<float>(viewport_.x), static_cast<float>(viewport_.y), static_cast<float>(viewport_.width), static_cast<float>(viewport_.height)};

    if (!SDL_UpdateTexture(texture_.get(), nullptr, rgba.data(), contracts::logical_width * 4) || !SDL_SetRenderDrawColor(renderer_.get(), 0, 0, 0, 255)
        || !SDL_RenderClear(renderer_.get()) || !SDL_RenderTexture(renderer_.get(), texture_.get(), nullptr, &destination) || !SDL_RenderPresent(renderer_.get())) {
        throw_sdl_error("Present frame");
    }
}

} // namespace moorhuhn::platform
