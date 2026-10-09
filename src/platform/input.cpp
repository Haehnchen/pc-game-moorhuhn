#include "platform/input.hpp"

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_mouse.h>

#include <algorithm>
#include <stdexcept>

namespace moorhuhn::platform {
namespace {

std::optional<contracts::InputControl> key_control(SDL_Scancode key) {
    using enum contracts::InputControl;

    switch (key) {
        case SDL_SCANCODE_RETURN:
            return accept;
        case SDL_SCANCODE_SPACE:
            return space;
        case SDL_SCANCODE_BACKSPACE:
            return backspace;
        case SDL_SCANCODE_ESCAPE:
            return cancel;
        case SDL_SCANCODE_LEFT:
            return left;
        case SDL_SCANCODE_RIGHT:
            return right;
        case SDL_SCANCODE_UP:
            return up;
        case SDL_SCANCODE_DOWN:
            return down;
        default:
            return std::nullopt;
    }
}

std::optional<contracts::InputControl> button_control(Uint8 button) {
    if (button == SDL_BUTTON_LEFT) {
        return contracts::InputControl::primary;
    }

    if (button == SDL_BUTTON_RIGHT) {
        return contracts::InputControl::secondary;
    }

    return std::nullopt;
}

} // namespace

InputAdapter::InputAdapter(Display& display, const MonotonicClock& clock)
    : display_(display), clock_(clock), focused_((SDL_GetWindowFlags(display.window()) & SDL_WINDOW_INPUT_FOCUS) != 0) {
    if (SDL_GetMouseFocus() == display.window()) {
        float x{};
        float y{};
        SDL_GetMouseState(&x, &y);
        window_pointer_ = Coordinate{x, y};
    }
}

void InputAdapter::set_text_input(bool enabled) {
    const bool success = enabled ? SDL_StartTextInput(display_.window()) : SDL_StopTextInput(display_.window());

    if (!success) {
        throw std::runtime_error(std::string("Text input: ") + SDL_GetError());
    }
}

void InputAdapter::recenter(contracts::ScreenPoint point) {
    display_.refresh();
    const auto mapped = display_.viewport().logical_to_window(point);

    if (!mapped) {
        throw std::invalid_argument("Cursor center is outside the drawable playfield");
    }

    window_pointer_ = *mapped;

    if (focused_) {
        SDL_WarpMouseInWindow(display_.window(), static_cast<float>(mapped->x), static_cast<float>(mapped->y));
    }
}

contracts::InputFrame InputAdapter::poll() {
    display_.refresh();
    contracts::InputFrame frame;
    frame.sequence = ++sequence_;
    frame.begin = previous_end_;
    const auto window_id = SDL_GetWindowID(display_.window());
    auto pointer = [&]() -> std::optional<contracts::ScreenPoint> {
        return window_pointer_ && focused_ ? display_.viewport().window_to_logical(*window_pointer_) : std::nullopt;
    };

    auto append = [&](const SDL_Event& event, contracts::InputControl control, contracts::InputEdge edge) {
        frame.actions.push_back({clock_.from_sdl_timestamp(event.common.timestamp), ++action_order_, control, edge, pointer()});
    };

    SDL_Event event;

    while (SDL_PollEvent(&event)) {
        switch (event.type) {
            case SDL_EVENT_QUIT:
                frame.quit_requested = true;
                break;
            case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                if (event.window.windowID == window_id) {
                    frame.quit_requested = true;
                }

                break;
            case SDL_EVENT_WINDOW_FOCUS_GAINED:
            case SDL_EVENT_WINDOW_FOCUS_LOST:
                if (event.window.windowID == window_id) {
                    focused_ = event.type == SDL_EVENT_WINDOW_FOCUS_GAINED;

                    if (!focused_) {
                        window_pointer_.reset();
                    } else if (SDL_GetMouseFocus() == display_.window()) {
                        float x{};
                        float y{};
                        SDL_GetMouseState(&x, &y);
                        window_pointer_ = Coordinate{x, y};
                    }

                    append(event, contracts::InputControl::focus, focused_ ? contracts::InputEdge::pressed : contracts::InputEdge::released);
                }

                break;
            case SDL_EVENT_WINDOW_MOUSE_ENTER:
                if (event.window.windowID == window_id && SDL_GetMouseFocus() == display_.window()) {
                    float x{};
                    float y{};
                    SDL_GetMouseState(&x, &y);
                    window_pointer_ = Coordinate{x, y};
                }

                break;
            case SDL_EVENT_WINDOW_MOUSE_LEAVE:
                if (event.window.windowID == window_id) {
                    window_pointer_.reset();
                }

                break;
            case SDL_EVENT_WINDOW_RESIZED:
            case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
            case SDL_EVENT_WINDOW_MINIMIZED:
            case SDL_EVENT_WINDOW_RESTORED:
                if (event.window.windowID == window_id) {
                    display_.refresh();
                }

                break;
            case SDL_EVENT_MOUSE_MOTION:
                if (event.motion.windowID == window_id) {
                    window_pointer_ = Coordinate{event.motion.x, event.motion.y};
                }

                break;
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
            case SDL_EVENT_MOUSE_BUTTON_UP:
                if (event.button.windowID == window_id) {
                    window_pointer_ = Coordinate{event.button.x, event.button.y};

                    if (const auto control = button_control(event.button.button)) {
                        append(event, *control, event.type == SDL_EVENT_MOUSE_BUTTON_DOWN ? contracts::InputEdge::pressed : contracts::InputEdge::released);
                    }
                }

                break;
            case SDL_EVENT_KEY_DOWN:
            case SDL_EVENT_KEY_UP:
                if (event.key.windowID == window_id && (!event.key.repeat || event.key.scancode == SDL_SCANCODE_BACKSPACE)) {
                    if (const auto control = key_control(event.key.scancode)) {
                        append(event, *control, event.type == SDL_EVENT_KEY_DOWN ? contracts::InputEdge::pressed : contracts::InputEdge::released);
                    }
                }

                break;
            case SDL_EVENT_TEXT_INPUT:
                if (event.text.windowID == window_id && event.text.text && event.text.text[0]) {
                    append(event, contracts::InputControl::text, contracts::InputEdge::pressed);
                    frame.actions.back().text = event.text.text;
                }

                break;
            default:
                break;
        }
    }

    frame.end = std::max(frame.begin, clock_.now());
    auto last = frame.begin;

    for (auto& action : frame.actions) {
        action.at = std::clamp(action.at, last, frame.end);
        last = action.at;
    }

    frame.focused = focused_;
    frame.pointer = pointer();
    previous_end_ = frame.end;

    return frame;
}

} // namespace moorhuhn::platform
