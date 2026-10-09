#include "platform/input.hpp"

#include "platform/clock.hpp"
#include "platform/display.hpp"
#include "platform/paths.hpp"

#include <SDL3/SDL.h>

#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {

void require(bool result, const char* message) {
    if (!result) {
        throw std::runtime_error(message);
    }
}

template <typename Function> void rejected(Function function, const char* message) {
    bool failed{};
    try {
        function();
    } catch (const std::exception&) {
        failed = true;
    }
    require(failed, message);
}

void push(SDL_Event& event) {
    require(SDL_PushEvent(&event), "Cannot push test event");
}

void window_event(SDL_EventType type, Uint32 window) {
    SDL_Event event{};
    event.window.type = type;
    event.window.windowID = window;
    push(event);
}

void mouse_event(SDL_EventType type, Uint32 window, Uint8 button, float x, float y, Uint64 timestamp) {
    SDL_Event event{};
    event.button.type = type;
    event.button.windowID = window;
    event.button.button = button;
    event.button.x = x;
    event.button.y = y;
    event.button.timestamp = timestamp;
    push(event);
}

void check_paths(const std::filesystem::path& root) {
    const auto installed = moorhuhn::platform::AppPaths::discover();
    require(installed.resource_root().is_absolute() && installed.user_root().is_absolute(), "Default paths differ");
    require(installed.resource("images.pak").parent_path() == installed.resource_root(), "Resource root differs from executable directory");
    const auto paths = moorhuhn::platform::AppPaths::discover(root);
    std::ifstream marker(paths.resource("marker.txt"));
    std::string text;
    std::getline(marker, text);
    require(text == "Configured resource", "Configured resource content differs");
    require(paths.resource_root().is_absolute() && paths.user_root().is_absolute(), "Paths are relative");
    require(paths.resource_root() != paths.user_root(), "User path is the resource path");
    const auto writable = paths.user_file("write-test.tmp");
    {
        std::ofstream stream(writable);
        require(static_cast<bool>(stream << "user path check\n"), "User path is not writable");
    }
    require(std::filesystem::remove(writable), "Cannot remove user test file");
    rejected(
        [&] {
            (void)paths.resource("../marker.txt");
        },
        "Resource traversal accepted");
    rejected(
        [&] {
            (void)paths.resource(root);
        },
        "Absolute resource file accepted");
    rejected(
        [&] {
            (void)paths.user_file("../escape");
        },
        "User traversal accepted");
    rejected(
        [] {
            (void)moorhuhn::platform::AppPaths::discover("relative");
        },
        "Relative root accepted");
    std::cout << "resource=" << paths.resource_root().string() << " user=" << paths.user_root().string() << '\n';
}

void check_display(bool native, bool require_minimized, bool skip_minimize) {
    using namespace moorhuhn;
    platform::Display display({"Platform check", 640, 480, !native});
    if (native) {
        require(SDL_SyncWindow(display.window()), "Initial window mapping did not finish");
    }
    platform::MonotonicClock clock;
    platform::InputAdapter input(display, clock);
    (void)input.poll();
    const auto window = SDL_GetWindowID(display.window());
    std::vector<std::uint8_t> frame(640 * 480 * 4, 255);
    display.present(frame);
    rejected(
        [&] {
            display.present(std::span<const std::uint8_t>(frame).first(10));
        },
        "Short frame accepted");
    const auto before = clock.now();
    SDL_Delay(3);
    require(clock.now() > before, "Clock did not advance");
    require(clock.from_sdl_timestamp(0).microseconds == 0, "Pre-epoch time did not clamp");

    window_event(SDL_EVENT_WINDOW_FOCUS_GAINED, window);
    (void)input.poll();
    input.recenter({320, 240});
    require(input.poll().pointer == contracts::ScreenPoint{320, 240}, "Round-entry pointer reset differs");
    const auto equal_time = SDL_GetTicksNS();
    mouse_event(SDL_EVENT_MOUSE_BUTTON_DOWN, window, SDL_BUTTON_LEFT, 0, 0, equal_time);
    mouse_event(SDL_EVENT_MOUSE_BUTTON_UP, window, SDL_BUTTON_LEFT, 639.9F, 479.9F, equal_time);
    mouse_event(SDL_EVENT_MOUSE_BUTTON_DOWN, window, SDL_BUTTON_RIGHT, 640, 200, equal_time - 1000);
    mouse_event(SDL_EVENT_MOUSE_BUTTON_DOWN, window + 100, SDL_BUTTON_LEFT, 50, 50, equal_time);
    SDL_Event key{};
    key.key.type = SDL_EVENT_KEY_DOWN;
    key.key.windowID = window;
    key.key.scancode = SDL_SCANCODE_RETURN;
    key.key.timestamp = equal_time + 5000000000ULL;
    push(key);
    key.key.repeat = true;
    push(key);
    auto controls = input.poll();
    require(controls.actions.size() == 4, "Wrong action count");
    require(controls.actions[0].control == contracts::InputControl::primary && controls.actions[0].edge == contracts::InputEdge::pressed
                && controls.actions[0].pointer == contracts::ScreenPoint{0, 0},
        "Primary edge differs");
    require(controls.actions[1].edge == contracts::InputEdge::released && controls.actions[1].pointer == contracts::ScreenPoint{639, 479}, "Released edge differs");
    require(controls.actions[2].control == contracts::InputControl::secondary && !controls.actions[2].pointer, "Outside click accepted");
    require(controls.actions[3].control == contracts::InputControl::accept, "Keyboard edge missing");
    auto last = controls.begin;
    std::uint64_t order{};
    for (const auto& action : controls.actions) {
        require(action.at >= last && action.at <= controls.end && action.order > order, "Action ordering or timestamp bounds differ");
        last = action.at;
        order = action.order;
    }
    input.set_text_input(true);
    key.key.repeat = false;
    key.key.scancode = SDL_SCANCODE_SPACE;
    push(key);
    key.key.scancode = SDL_SCANCODE_BACKSPACE;
    push(key);
    key.key.repeat = true;
    push(key);
    SDL_Event text{};
    text.text.type = SDL_EVENT_TEXT_INPUT;
    text.text.windowID = window;
    text.text.text = "A\xc3\xa4";
    push(text);
    text.text.windowID = window + 100;
    text.text.text = "foreign";
    push(text);
    const auto typing = input.poll();
    require(typing.actions.size() == 4 && typing.actions[0].control == contracts::InputControl::space && typing.actions[1].control == contracts::InputControl::backspace
                && typing.actions[2].control == contracts::InputControl::backspace && typing.actions[3].control == contracts::InputControl::text
                && typing.actions[3].text == "A\xc3\xa4",
        "UI text transport or key distinction differs");
    input.set_text_input(false);
    window_event(SDL_EVENT_WINDOW_FOCUS_LOST, window);
    const auto lost = input.poll();
    require(!lost.focused && !lost.pointer && !lost.quit_requested, "Focus notice differs");
    require(
        lost.actions.size() == 1 && lost.actions[0].control == contracts::InputControl::focus && lost.actions[0].edge == contracts::InputEdge::released, "Focus loss edge missing");
    display.present(frame);
    SDL_Delay(3);
    const auto unfocused = input.poll();
    require(!unfocused.focused && unfocused.end > lost.end, "Focus stopped session time");
    window_event(SDL_EVENT_WINDOW_FOCUS_GAINED, window);
    require(input.poll().focused, "Focus regain missing");
    window_event(SDL_EVENT_WINDOW_FOCUS_LOST, window);
    window_event(SDL_EVENT_WINDOW_FOCUS_GAINED, window);
    const auto refocused = input.poll();
    require(refocused.focused && refocused.actions.size() == 2 && refocused.actions[0].control == contracts::InputControl::focus
                && refocused.actions[0].edge == contracts::InputEdge::released && refocused.actions[1].control == contracts::InputControl::focus
                && refocused.actions[1].edge == contracts::InputEdge::pressed && refocused.actions[0].order < refocused.actions[1].order,
        "Focus loss/regain collapsed within one poll");

    require(SDL_SetWindowSize(display.window(), 800, 600), "Resize request failed");
    require(SDL_SyncWindow(display.window()), "Resize did not finish");
    SDL_Delay(native ? 100 : 1);
    (void)input.poll();
    display.present(frame);
    auto viewport = display.viewport();
    std::cout << "resize window=" << viewport.window_width << 'x' << viewport.window_height << " pixels=" << viewport.pixel_width << 'x' << viewport.pixel_height
              << " viewport=" << viewport.x << ',' << viewport.y << ',' << viewport.width << ',' << viewport.height << " scale=" << viewport.scale << '\n';
    require(viewport.window_width == 800 && viewport.window_height == 600, "Resize did not apply");
    const auto center = viewport.logical_to_window({320, 240});
    require(center.has_value(), "Missing resized center");
    mouse_event(SDL_EVENT_MOUSE_BUTTON_DOWN, window, SDL_BUTTON_LEFT, static_cast<float>(center->x), static_cast<float>(center->y), SDL_GetTicksNS());
    require(input.poll().actions.back().pointer == contracts::ScreenPoint{320, 240}, "Resized input differs");
    require(SDL_SetWindowSize(display.window(), 320, 200), "Small resize request failed");
    require(SDL_SyncWindow(display.window()), "Small resize did not finish");
    SDL_Delay(native ? 100 : 1);
    (void)input.poll();
    display.present(frame);
    require(display.viewport().scale < 1.0, "Small window did not shrink");

    if (native && !skip_minimize) {
        require(SDL_MinimizeWindow(display.window()), "Minimize request failed");
        require(SDL_SyncWindow(display.window()), "Minimize did not finish");
        SDL_Delay(200);
        (void)input.poll();
        const bool minimized = display.minimized();
        std::cout << "minimized=" << minimized << " drawable=" << display.viewport().drawable() << '\n';
        if (require_minimized) {
            require(minimized && !display.viewport().drawable(), "Window did not minimize");
        }
        display.present(frame);
        require(SDL_RestoreWindow(display.window()), "Restore request failed");
        require(SDL_SyncWindow(display.window()), "Restore did not finish");
        SDL_Delay(200);
        (void)input.poll();
        display.present(frame);
        require(!display.minimized() && display.viewport().drawable(), "Window did not restore");
    }
    window_event(SDL_EVENT_WINDOW_CLOSE_REQUESTED, window + 100);
    require(!input.poll().quit_requested, "Foreign close request accepted");
    window_event(SDL_EVENT_WINDOW_CLOSE_REQUESTED, window);
    require(input.poll().quit_requested && !input.poll().quit_requested, "Close notice was not one-frame");
    SDL_Event quit{};
    quit.type = SDL_EVENT_QUIT;
    push(quit);
    require(input.poll().quit_requested && !input.poll().quit_requested, "Quit notice was not one-frame");
    std::cout << "driver=" << SDL_GetCurrentVideoDriver() << " renderer=" << SDL_GetRendererName(display.renderer()) << " density=" << SDL_GetWindowPixelDensity(display.window())
              << " display_scale=" << SDL_GetWindowDisplayScale(display.window()) << '\n';
}

void check_manual_display() {
    using namespace moorhuhn;
    require(SDL_ShowCursor(), "Cannot reset cursor visibility");
    {
        platform::Display display({"Fullscreen check", 640, 480, true, true, true});
        require((SDL_GetWindowFlags(display.window()) & SDL_WINDOW_FULLSCREEN) != 0, "Fullscreen request was lost");
        require(!SDL_CursorVisible(), "System cursor duplicates game crosshair");
        const auto center = display.viewport().logical_to_window({320, 240});
        require(center && display.viewport().window_to_logical(*center) == contracts::ScreenPoint{320, 240}, "Fullscreen scaling changed logical pointer coordinates");
    }
    require(SDL_CursorVisible(), "Display destruction did not restore cursor visibility");
}

} // namespace

int main(int argc, char** argv) {
    try {
        std::filesystem::path root;
        bool native{};
        bool require_minimized{};
        bool skip_minimize{};
        for (int index = 1; index < argc; ++index) {
            const std::string_view argument(argv[index]);
            if (argument == "--resource-root" && index + 1 < argc) {
                root = argv[++index];
            } else if (argument == "--native") {
                native = true;
            } else if (argument == "--require-minimized") {
                require_minimized = true;
            } else if (argument == "--skip-minimize") {
                skip_minimize = true;
            } else {
                throw std::invalid_argument("Unknown test argument");
            }
        }
        require(root.is_absolute(), "Pass --resource-root ABSOLUTE_PATH");
        for (int repeat = 0; repeat < 3; ++repeat) {
            moorhuhn::platform::SdlRuntime runtime;
            check_paths(root);
            check_display(native, require_minimized, skip_minimize);
            check_manual_display();
        }
        require(SDL_WasInit(0) == 0, "SDL lifetime was not released");
        std::cout << "3 SDL lifetimes passed; input, focus, clock, presentation, paths passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "platform test: " << error.what() << '\n';
        return 1;
    }
}
