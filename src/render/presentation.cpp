#include "render/presentation.hpp"

namespace moorhuhn::render {

void present(platform::Display& display, const Framebuffer& framebuffer) {
    display.present(framebuffer.bytes());
}

} // namespace moorhuhn::render
