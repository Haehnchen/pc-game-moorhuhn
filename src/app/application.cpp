#include "app/application.hpp"

#include "app/audio_clock.hpp"
#include "app/audio_map.hpp"
#include "app/session.hpp"
#include "assets/asset_store.hpp"
#include "audio/audio_engine.hpp"
#include "platform/clock.hpp"
#include "platform/display.hpp"
#include "platform/input.hpp"
#include "platform/paths.hpp"
#include "render/presentation.hpp"
#include "storage/highscores.hpp"
#include "ui/menu.hpp"

#include <SDL3/SDL_audio.h>
#include <SDL3/SDL_timer.h>

#include <algorithm>
#include <chrono>
#include <iostream>
#include <vector>

namespace moorhuhn::app {
int run() {
    const auto paths = platform::AppPaths::discover();
    // Declare the owner first, but initialize video only after startup validation.
    // All decoded audio, textures, and surfaces are destroyed before SDL_Quit.
    std::optional<platform::SdlRuntime> runtime;
    auto store = assets::AssetStore::load_package(paths.resource_root());
    std::vector<contracts::AssetView> views;

    for (const auto& id : store.image_ids()) {
        views.push_back(store.image(id.value));
    }

    auto mixer = [&] {
        try {
            return audio::AudioEngine::device(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK);
        } catch (const audio::AudioError& error) {
            std::cerr << "Audio playback unavailable: " << error.what() << "; continuing with offline audio.\n";

            return audio::AudioEngine::offline();
        }
    }();
    mixer.preload(store);
    mixer.master_volume(0.5F);
    mixer.set_event_map(game_sound_map());

    // Create the window and session after assets and audio have been validated.
    runtime.emplace();
    platform::Display display({"Moorhuhn", 640, 480, false, true, true});
    platform::MonotonicClock clock;
    platform::InputAdapter input(display, clock);
    const auto seed = [&] {
        return static_cast<std::uint32_t>(std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()));
    };

    Session session(views, input.focused(), seed());

    const auto score_path = paths.user_file("highscores.txt");
    const auto loaded = storage::load(score_path);
    ui::Menu menu({store.image("main"), store.image("score"), store.image("credits"), store.image("mask"), store.palette("moorhuhn.pal"), store.lookup("indices/ClutTrns.png").red,
                      store.tabel1(), store.tabel2(), store.image("hole"), store.image("guck")},
        loaded.scores,
        [&] {
            return session.menu_random();
        }

    );

    if (!loaded.error.empty()) {
        menu.storage_error(loaded.error);
        std::cerr << loaded.error << '\n';
    }

    std::vector<contracts::AssetView> render_views = views;
    render_views.reserve(views.size() + 2);
    render_views.push_back(menu.image("ui.surface"));
    render_views.push_back(menu.image("ui.credits"));

    render::Renderer renderer;
    AudioFrameClock audio_clock;
    // Advance offline audio to each event before applying that event's sound changes.
    const auto generate_to = [&](contracts::TimePoint at) {
        // Device mixers advance in their audio callback.
        if (mixer.has_device()) {
            return;
        }

        auto remaining = audio_clock.advance(at);

        while (remaining) {
            const auto count = static_cast<std::uint32_t>(std::min<std::uint64_t>(remaining, 4096));
            static_cast<void>(mixer.render(count));
            remaining -= count;
        }
    };

    const auto consume = [&](const contracts::UpdateOutput& output) {
        for (const auto& event : output.events) {
            generate_to(event.at);
            const auto consumed = mixer.consume(std::span(&event, 1));

            for (const auto& failure : consumed.failures) {
                std::cerr << "Audio event " << failure.sequence << ": " << failure.message << '\n';
            }

            if (const auto* effect = std::get_if<contracts::PresentationEffect>(&event.payload)) {
                if (consumed.consumed && (effect->name == "round.ended" || effect->name == "session.quit")) {
                    for (const auto* sound : {"bird22", "plane22", "plane222", "balon22"}) {
                        mixer.stop_sound(sound);
                    }
                }
            }
        }
    };

    bool quit{};
    bool text_input{};
    std::uint32_t previous_ms{};

    for (;;) {
        // Input reaches the session before the menu handles its commands.
        const auto controls = input.poll();
        consume(session.update(controls));
        const auto now_ms = static_cast<std::uint32_t>(controls.end.microseconds / 1000);
        const auto commands = menu.advance(controls, now_ms - previous_ms, now_ms);
        previous_ms = now_ms;
        generate_to(controls.end);

        for (const auto& command : commands) {
            switch (command.kind) {
                case ui::CommandKind::start_round:
                    consume(session.start(controls.end, seed()));
                    input.recenter({320, 240});
                    break;
                case ui::CommandKind::end_round:
                    break; // The same ordered cancel/quit input reaches Session first.
                case ui::CommandKind::save_scores: {
                    const auto error = storage::save(score_path, menu.scores());
                    menu.storage_error(error);

                    if (!error.empty()) {
                        std::cerr << error << '\n';
                    }

                    break;
                }

                case ui::CommandKind::quit:
                    quit = !menu.awaiting_save_ack();
                    break;
                case ui::CommandKind::play_sound:
                    try {
                        static_cast<void>(mixer.play(command.sound, {command.volume, command.repeats}));
                    } catch (const audio::AudioError& error) {
                        std::cerr << "Audio command failed: " << error.what() << '\n';
                    }
                    break;
                case ui::CommandKind::stop_sound:
                    try {
                        mixer.stop_sound(command.sound);
                    } catch (const audio::AudioError& error) {
                        std::cerr << "Audio command failed: " << error.what() << '\n';
                    }
                    break;
            }
        }

        if (menu.text_input() != text_input) {
            text_input = menu.text_input();
            input.set_text_input(text_input);
        }

        render_views[views.size()] = menu.image("ui.surface");
        render_views[views.size() + 1] = menu.image("ui.credits");
        render::present(display, renderer.render(menu.gameplay() ? session.draw() : menu.draw(), render_views));

        const auto state = session.snapshot();

        // Present the terminal world iteration before entering the score screen.
        if (menu.gameplay() && state.phase == game::Phase::ended) {
            menu.round_finished(state.round.score);
        }

        if (quit) {
            break;
        }

        if (display.minimized()) {
            SDL_Delay(20);
        } else if (!display.vsync()) {
            SDL_Delay(8);
        }
    }

    if (text_input) {
        input.set_text_input(false);
    }

    mixer.stop_all();

    return 0;
}

} // namespace moorhuhn::app
