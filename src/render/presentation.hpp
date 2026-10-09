#pragma once

#include "platform/display.hpp"
#include "render/framebuffer.hpp"

namespace moorhuhn::render {

void present(platform::Display& display, const Framebuffer& framebuffer);

} // namespace moorhuhn::render
