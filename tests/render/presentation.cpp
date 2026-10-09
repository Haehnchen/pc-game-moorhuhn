#include "render/presentation.hpp"

#include "render/renderer.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <iostream>
#include <stdexcept>

namespace {

void require(bool result, const char* message) {
    if (!result) {
        throw std::runtime_error(message);
    }
}

} // namespace

int main() {
    try {
        using namespace moorhuhn;
        using namespace contracts;
        const DrawList draws{{FillDraw{{0, 0}, 640, 480, {4, 7, 9, 255}}, FillDraw{{0, 0}, 1, 1, {255, 0, 0, 255}}, FillDraw{{639, 479}, 1, 1, {0, 255, 0, 255}},
            FillDraw{{100, 200}, 1, 1, {255, 255, 255, 255}}, FillDraw{{320, 240}, 1, 1, {255, 255, 0, 255}}}};
        render::Renderer renderer;
        const auto& image = renderer.render(draws, {});
        const auto original = renderer.snapshot();
        platform::SdlRuntime runtime;
        platform::Display display({"Presentation check", 640, 480, true});
        for (const auto& size : std::array<std::array<int, 2>, 4>{{{640, 480}, {800, 600}, {1280, 960}, {320, 240}}}) {
            require(SDL_SetWindowSize(display.window(), size[0], size[1]), "Resize failed");
            require(SDL_SyncWindow(display.window()), "Resize did not finish");
            render::present(display, image);
            const auto& viewport = display.viewport();
            require(viewport.drawable(), "Viewport is empty");
            for (const auto point : std::array<ScreenPoint, 4>{{{0, 0}, {639, 479}, {100, 200}, {320, 240}}}) {
                const auto window = viewport.logical_to_window(point);
                require(window && viewport.window_to_logical(*window) == point, "Pointer/image transform differs");
                require(image.at(*viewport.window_to_logical(*window)) == original.at(point), "Pointer targets another pixel");
            }
            if (viewport.x > 0) {
                require(!viewport.window_to_logical({0, 0}), "Letterbox can target an image pixel");
            }
        }
        // A separate 2x window/output pair tests HiDPI units without changing SDL state.
        const auto hidpi = platform::Viewport::fit(800, 600, 1600, 1200);
        const auto pointer = hidpi.logical_to_window({100, 200});
        require(pointer && hidpi.window_to_logical(*pointer) == ScreenPoint{100, 200}, "HiDPI pointer differs");
        require(image.at(*hidpi.window_to_logical(*pointer)) == RgbaPixel{255, 255, 255, 255}, "HiDPI marker differs");
        require(std::equal(image.bytes().begin(), image.bytes().end(), original.bytes().begin()), "Presentation changed CPU pixels");
        std::cout << "Streaming presentation passed at four sizes; logical image/pointer markers and 2x HiDPI agree\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
