#include "audio/audio_engine.hpp"

#include "../fixtures/checksum.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <span>
#include <sstream>
#include <type_traits>
#include <vector>

namespace {
namespace fs = std::filesystem;
namespace audio = moorhuhn::audio;
namespace contracts = moorhuhn::contracts;
using moorhuhn::assets::AssetStore;
using moorhuhn::assets::AudioView;
using Bytes = std::vector<std::uint8_t>;

static_assert(!std::is_copy_constructible_v<audio::AudioEngine>);
static_assert(std::is_nothrow_move_constructible_v<audio::AudioEngine>);

void require(bool condition, std::string_view cause) {
    if (!condition) {
        throw std::runtime_error(std::string(cause));
    }
}

Bytes read(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    require(bool(stream), "cannot read fixture");
    return Bytes(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
}

void silent_guard() {
    require(SDL_WasInit(0) == 0, "test initialized an SDL subsystem");
    require(SDL_GetCurrentAudioDriver() == nullptr, "test selected an audio driver");
}

void error(const std::function<void()>& action, std::string_view cause) {
    try {
        action();
    } catch (const audio::AudioError& failure) {
        require(std::string(failure.what()).find(cause) != std::string::npos, "audio error lacks expected cause");
        require(!failure.operation().empty() && !failure.cause().empty(), "audio error lacks context");
        return;
    }
    throw std::runtime_error("expected AudioError: " + std::string(cause));
}

double energy(std::span<const float> samples) {
    double result{};
    for (float sample : samples) {
        result += static_cast<double>(sample) * sample;
    }
    return result;
}

void same(std::span<const float> actual, std::span<const float> expected, float gain = 1.0F) {
    require(actual.size() == expected.size(), "PCM sizes differ");
    for (std::size_t n = 0; n < actual.size(); ++n) {
        if (std::abs(actual[n] - expected[n] * gain) >= 0.00001F) {
            throw std::runtime_error("PCM samples differ at " + std::to_string(n) + ": " + std::to_string(actual[n]) + " != " + std::to_string(expected[n] * gain));
        }
    }
}

void zero(std::span<const float> samples) {
    const auto maximum = std::max_element(samples.begin(), samples.end(), [](float a, float b) {
        return std::abs(a) < std::abs(b);
    });
    if (maximum != samples.end() && std::abs(*maximum) >= 1e-7F) {
        throw std::runtime_error("expected silent PCM; peak=" + std::to_string(std::abs(*maximum)));
    }
}

std::size_t onset(std::span<const float> samples, float threshold = 0.00001F) {
    auto found = std::find_if(samples.begin(), samples.end(), [threshold](float value) {
        return std::abs(value) > threshold;
    });
    return static_cast<std::size_t>(found - samples.begin());
}

struct Fixture {
    std::vector<Bytes> bytes;
    std::vector<AudioView> sounds;
    std::vector<audio::EffectSound> map;

    explicit Fixture(const fs::path& root) {
        std::ifstream data(root / "tones.txt");
        unsigned count{};
        require(bool(data >> count), "Missing sound fixture count");
        bytes.reserve(count);
        sounds.reserve(count);
        for (unsigned i = 0; i < count; ++i) {
            AudioView sound;
            std::size_t size{};
            std::string crc;
            require(bool(data >> sound.id.value >> sound.path >> size >> crc >> sound.original_sample_frames >> sound.original_sample_rate >> sound.decoded_48000hz_samples
                         >> sound.source_duration_seconds),
                "Missing sound fixture");
            bytes.push_back(read(root / sound.path));
            require(bytes.back().size() == size, "fixture byte length differs");
            require(moorhuhn::tests::checksum(bytes.back()) == crc, "fixture digest differs");
            sound.encoded = bytes.back();
            sounds.push_back(sound);
        }
        require(bool(data >> count), "Missing event map count");
        for (unsigned i = 0; i < count; ++i) {
            audio::EffectSound item;
            std::string action;
            require(bool(data >> item.effect >> action >> item.sound.value >> item.options.volume >> item.options.repeats), "Missing effect fixture");
            if (item.sound.value == "none") {
                item.sound.value.clear();
            }
            require(action == "play" || action == "stop_sound" || action == "stop_all", "bad fixture action");
            item.action = action == "play" ? audio::SoundAction::play : action == "stop_sound" ? audio::SoundAction::stop_sound : audio::SoundAction::stop_all;
            map.push_back(item);
        }
    }
};

audio::AudioEngine engine(const Fixture& fixture, std::uint32_t voices = 8, std::uint32_t channels = 1) {
    auto result = audio::AudioEngine::offline({voices, channels});
    result.preload(fixture.sounds);
    return result;
}

contracts::GameEvent play_event(std::uint64_t sequence, std::string id) {
    return {sequence, {}, contracts::PlaySound{{std::move(id)}}};
}

contracts::GameEvent effect_event(std::uint64_t sequence, std::string effect) {
    return {sequence, {}, contracts::PresentationEffect{std::move(effect), {}, {}}};
}

void synthetic(const fs::path& root, const fs::path& asset_fixture) {
    const Fixture fixture(root);
    std::size_t checks{};
    const auto run = [&](std::string_view name, const std::function<void()>& action) {
        action();
        silent_guard();
        ++checks;
        std::cout << "pass: " << name << '\n';
    };
    run("device-free creation and option errors", [&] {
        auto mixer = engine(fixture);
        require(!mixer.has_device() && mixer.channels() == 1 && mixer.sample_rate() == 48000 && mixer.sound_count() == 3, "offline configuration differs");
        require(mixer.sound("shoot22").decoded_frames == 4800, "decoded tone length differs");
        error(
            [] {
                static_cast<void>(audio::AudioEngine::offline({0, 1}));
            },
            "1..256");
        error(
            [] {
                static_cast<void>(audio::AudioEngine::offline({1, 0}));
            },
            "1..2");
        error(
            [] {
                static_cast<void>(audio::AudioEngine::device(0));
            },
            "explicit playback");
        error(
            [] {
                static_cast<void>(audio::AudioEngine::device(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK));
            },
            "MIX_CreateMixerDevice");
        error(
            [&] {
                static_cast<void>(mixer.render(std::numeric_limits<std::uint32_t>::max()));
            },
            "byte limit");
        require(mixer.render(0).samples.empty(), "zero-sized rendering differs");
    });
    run("predecoded storage survives input release", [&] {
        auto mixer = audio::AudioEngine::offline({2, 1});
        {
            Fixture temporary(root);
            mixer.preload(temporary.sounds);
        }
        static_cast<void>(mixer.play("shoot22"));
        require(energy(mixer.render(4800).samples) > 1, "decoded data borrowed dead source bytes");
    });
    run("AssetStore preload and lifetime", [&] {
        auto mixer = audio::AudioEngine::offline({2, 1});
        {
            auto store = AssetStore::load(asset_fixture / "manifest.txt");
            mixer.preload(store);
        }
        require(mixer.sound_count() == 1 && mixer.sound("typo22").decoded_frames == 1067, "asset-store preload differs");
        static_cast<void>(mixer.play("typo22"));
        const auto rendered = mixer.render(1195);
        require(rendered.frames_with_audio == 1067, "silence has wrong decoded duration");
        require(energy(rendered.samples) < 1e-6, "synthetic silence has unexpected codec noise");
        zero(std::span(rendered.samples).subspan(1067));
    });
    run("shot onset and trimmed length", [&] {
        auto mixer = engine(fixture);
        const std::vector events{play_event(1, "shoot22")};
        const auto consumed = mixer.consume(events);
        require(consumed.started == 1 && consumed.failures.empty(), "shot event did not start");
        const auto rendered = mixer.render(4928);
        require(onset(rendered.samples) < 64, "shot has an unexpected start delay");
        require(rendered.frames_with_audio == 4800, "MP3 decoded length differs");
        zero(std::span(rendered.samples).subspan(4800));
        require(mixer.active_voices() == 0, "finished shot still occupies a playing voice");
    });
    run("overlapping independent voices", [&] {
        auto single = engine(fixture);
        auto overlap = engine(fixture);
        static_cast<void>(single.play("shoot22"));
        const auto expected = single.render(4800);
        const auto first = overlap.play("shoot22");
        const auto second = overlap.play("shoot22");
        require(first != second && overlap.active_voices() == 2, "same sound failed to overlap");
        same(overlap.render(4800).samples, expected.samples, 2.0F);
        auto loud = engine(fixture, 24);
        for (int n = 0; n < 20; ++n) {
            static_cast<void>(loud.play("shoot22"));
        }
        auto saturated = expected.samples;
        for (auto& sample : saturated) {
            sample = std::clamp(sample * 20.0F, -1.0F, 1.0F);
        }
        same(loud.render(4800).samples, saturated);
    });
    run("shot plus reload sums PCM", [&] {
        auto shot = engine(fixture);
        auto reload = engine(fixture);
        auto combined = engine(fixture);
        static_cast<void>(shot.play("shoot22"));
        static_cast<void>(reload.play("reload22"));
        auto expected = shot.render(7328);
        const auto reload_pcm = reload.render(7328);
        for (std::size_t n = 0; n < expected.samples.size(); ++n) {
            expected.samples[n] += reload_pcm.samples[n];
        }
        static_cast<void>(combined.play("shoot22"));
        static_cast<void>(combined.play("reload22"));
        const auto actual = combined.render(7328);
        require(actual.frames_with_audio == 7200, "overlap length did not follow the longer reload");
        same(actual.samples, expected.samples);
    });
    run("voice and master volume", [&] {
        auto reference = engine(fixture);
        auto quiet = engine(fixture);
        static_cast<void>(reference.play("shoot22"));
        const auto id = quiet.play("shoot22");
        quiet.volume(id, 0.5F);
        quiet.master_volume(0.5F);
        same(quiet.render(4800).samples, reference.render(4800).samples, 0.25F);
        auto muted = engine(fixture);
        static_cast<void>(muted.play("shoot22", {0.0F, 0}));
        zero(muted.render(4800).samples);
        for (float value : {-1.0F, 1.1F, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()}) {
            error(
                [&] {
                    quiet.master_volume(value);
                },
                "finite value");
        }
        error(
            [&] {
                static_cast<void>(quiet.play("shoot22", {1.0F, -2}));
            },
            "repeat count");
    });
    run("stop and restart begin at source zero", [&] {
        auto mixer = engine(fixture);
        auto reference = engine(fixture);
        const auto id = mixer.play("shoot22");
        static_cast<void>(reference.play("shoot22"));
        const auto prefix = reference.render(333);
        same(mixer.render(333).samples, prefix.samples);
        mixer.stop(id);
        require(!mixer.playing(id), "stopped voice remains playing");
        zero(mixer.render(200).samples);
        mixer.restart(id);
        same(mixer.render(333).samples, prefix.samples);
        mixer.restart(id, {0.25F, 0});
        same(mixer.render(333).samples, prefix.samples, 0.25F);
    });
    run("finite repeat count is additional passes", [&] {
        auto reference = engine(fixture);
        auto repeated = engine(fixture);
        static_cast<void>(reference.play("shoot22"));
        const auto one = reference.render(4800);
        static_cast<void>(repeated.play("shoot22", {1.0F, 2}));
        const auto three = repeated.render(14528);
        require(three.frames_with_audio == 14400, "finite repeats have wrong duration");
        for (std::size_t n = 0; n < 3; ++n) {
            same(std::span(three.samples).subspan(n * 4800, 4800), one.samples);
        }
        zero(std::span(three.samples).subspan(14400));
        require(repeated.active_voices() == 0, "finite repetition did not end");
    });
    run("infinite repeat requires explicit stop", [&] {
        auto mixer = engine(fixture);
        const auto id = mixer.play("bird22", {0.5F, -1});
        require(energy(mixer.render(18000).samples) > 1 && mixer.playing(id), "infinite repeat ended");
        mixer.stop(id);
        zero(mixer.render(128).samples);
    });
    run("stop one sound preserves other voices", [&] {
        auto mixer = engine(fixture);
        auto reference = engine(fixture);
        static_cast<void>(mixer.play("shoot22"));
        const auto reload = mixer.play("reload22");
        static_cast<void>(reference.play("reload22"));
        mixer.stop_sound("shoot22");
        require(mixer.playing(reload) && mixer.active_voices() == 1, "stop-sound stopped another sound");
        same(mixer.render(256).samples, reference.render(256).samples);
    });
    run("bounded pool and expired IDs", [&] {
        auto mixer = engine(fixture, 1);
        const auto first = mixer.play("shoot22");
        error(
            [&] {
                static_cast<void>(mixer.play("reload22"));
            },
            "capacity exhausted");
        static_cast<void>(mixer.render(4928));
        const auto second = mixer.play("reload22");
        require(first != second && !mixer.playing(first), "voice generation reused an old ID");
        error(
            [&] {
                mixer.restart(first);
            },
            "expired voice");
        error(
            [&] {
                mixer.stop({});
            },
            "expired voice");
        auto reference = engine(fixture, 1);
        static_cast<void>(reference.play("reload22"));
        same(mixer.render(256).samples, reference.render(256).samples);
    });
    run("transactional decode failure", [&] {
        auto mixer = engine(fixture);
        const Bytes bad{0, 1, 2, 3};
        std::vector views{fixture.sounds[0], fixture.sounds[1]};
        views[1].encoded = bad;
        error(
            [&] {
                mixer.preload(views);
            },
            "MIX_LoadAudio_IO");
        require(mixer.sound_count() == 3, "partial preload replaced existing sounds");
        static_cast<void>(mixer.play("shoot22"));
        require(energy(mixer.render(256).samples) > 0, "old preloaded sound disappeared");
        error(
            [&] {
                mixer.preload(fixture.sounds);
            },
            "stop voices");
    });
    run("length and duplicate source errors", [&] {
        auto mixer = engine(fixture);
        auto views = fixture.sounds;
        ++views[0].decoded_48000hz_samples;
        error(
            [&] {
                mixer.preload(views);
            },
            "decoded frame count");
        views = {fixture.sounds[0], fixture.sounds[0]};
        error(
            [&] {
                mixer.preload(views);
            },
            "duplicate sound ID");
        views = fixture.sounds;
        views[0].original_sample_rate = 0;
        error(
            [&] {
                mixer.preload(views);
            },
            "sample metadata");
        error(
            [&] {
                static_cast<void>(mixer.play("unknown"));
            },
            "not preloaded");
    });
    run("event batches consumed once including failures", [&] {
        auto mixer = engine(fixture);
        const std::vector events{play_event(1, "shoot22"), play_event(2, "unknown"), effect_event(3, "unmapped")};
        const auto first = mixer.consume(events);
        require(first.consumed == 3 && first.started == 1 && first.failures.size() == 1 && first.failures[0].sequence == 2, "event result differs");
        const auto duplicate = mixer.consume(events);
        require(duplicate.duplicates == 3 && duplicate.started == 0 && duplicate.failures.empty(), "event replayed or retried failure");
        require(mixer.active_voices() == 1 && mixer.last_sequence() == 3, "duplicate event added a voice");
        require(mixer.consume({}).consumed == 0, "empty batch did work");
    });
    run("sequence validation precedes every side effect", [&] {
        auto mixer = engine(fixture);
        const std::vector invalid{play_event(2, "shoot22"), play_event(1, "reload22")};
        error(
            [&] {
                static_cast<void>(mixer.consume(invalid));
            },
            "strictly increasing");
        const std::vector zero_sequence{play_event(0, "shoot22")};
        error(
            [&] {
                static_cast<void>(mixer.consume(zero_sequence));
            },
            "nonzero");
        require(mixer.active_voices() == 0 && mixer.last_sequence() == 0, "bad batch produced audio");
    });
    run("data mappings play reload and stop round", [&] {
        auto mixer = engine(fixture);
        mixer.set_event_map(fixture.map);
        const std::vector events{effect_event(1, "fixture.fire"), effect_event(2, "fixture.reload"), effect_event(3, "fixture.ambient")};
        require(mixer.consume(events).started == 3 && mixer.active_voices() == 3, "mapping did not create voices");
        const std::vector stop_ambient{effect_event(4, "fixture.stop_ambient")};
        require(mixer.consume(stop_ambient).stopped == 1 && mixer.active_voices() == 2, "mapped stop-sound differs");
        static_cast<void>(mixer.render(128));
        const std::vector round_end{effect_event(5, "fixture.round_end")};
        require(mixer.consume(round_end).stopped == 1 && mixer.active_voices() == 0, "round-end mapping did not stop voices");
        zero(mixer.render(256).samples);
    });
    run("mapping validation and explicit replay reset", [&] {
        auto mixer = engine(fixture);
        mixer.set_event_map(fixture.map);
        auto bad = fixture.map;
        bad.emplace_back(fixture.map.front());
        error(
            [&] {
                mixer.set_event_map(bad);
            },
            "duplicate effect");
        const std::vector event{effect_event(1, "fixture.fire")};
        require(mixer.consume(event).started == 1, "failed map replacement erased valid data");
        mixer.reset_events();
        require(mixer.last_sequence() == 0 && mixer.active_voices() == 0, "replay reset did not stop/clear");
        require(mixer.consume(event).started == 1, "explicit replay could not consume sequence one");
    });
    run("mono and stereo output preserve both MP3 channels", [&] {
        auto mono = engine(fixture);
        auto stereo = engine(fixture, 8, 2);
        static_cast<void>(mono.play("shoot22"));
        static_cast<void>(stereo.play("shoot22"));
        const auto one = mono.render(256);
        const auto two = stereo.render(256);
        require(two.samples.size() == 512 && two.frames_with_audio == 256, "stereo frame accounting differs");
        for (std::size_t n = 0; n < 256; ++n) {
            require(std::abs(two.samples[n * 2] - one.samples[n]) < 1e-5F, "left channel differs");
            require(std::abs(two.samples[n * 2 + 1] - one.samples[n]) < 1e-5F, "right channel differs");
        }
    });
    run("move ownership and independent mixer lifetimes", [&] {
        auto source = engine(fixture);
        const auto id = source.play("shoot22");
        auto destination = engine(fixture);
        static_cast<void>(destination.play("reload22"));
        destination = std::move(source);
        require(destination.playing(id) && destination.sound_count() == 3, "move assignment lost source resources");
        error(
            [&] {
                static_cast<void>(source.sound_count());
            },
            "moved-from");
        {
            auto another = engine(fixture);
            static_cast<void>(another.play("bird22"));
        }
        require(energy(destination.render(128).samples) > 0, "other mixer cleanup invalidated surviving mixer");
    });
    run("repeated failed preload and cleanup release allocations", [&] {
        const Bytes bad{0, 1, 2, 3};
        const auto attempt = [&] {
            auto mixer = engine(fixture);
            std::vector views{fixture.sounds[0], fixture.sounds[1]};
            views[1].encoded = bad;
            error(
                [&] {
                    mixer.preload(views);
                },
                "MIX_LoadAudio_IO");
            static_cast<void>(mixer.play("shoot22", {1.0F, -1}));
            static_cast<void>(mixer.render(128));
        };
        attempt(); // Warm SDL's process/thread caches before comparing allocations.
        const auto before = SDL_GetNumAllocations();
        for (int n = 0; n < 40; ++n) {
            attempt();
        }
        require(SDL_GetNumAllocations() == before, "SDL allocations grew after cleanup");
    });
    std::cout << "synthetic checks: " << checks << "; device/window/subsystem use: none\n";
}

void device(const fs::path& root) {
    const Fixture fixture(root);
    {
        auto mixer = audio::AudioEngine::device(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK);
        require(mixer.has_device() && mixer.channels() == 2 && mixer.sample_rate() == 48000, "dummy playback configuration differs");
        require(std::string_view(SDL_GetCurrentAudioDriver()) == "dummy", "playback selected a host driver");
        mixer.preload(fixture.sounds);
        mixer.set_event_map(fixture.map);
        const std::vector events{effect_event(1, "fixture.ambient")};
        require(mixer.consume(events).started == 1, "device event did not start loop");
        const auto repeated = mixer.consume(events);
        require(repeated.duplicates == 1 && repeated.started == 0, "device event replayed");
        const auto shot = mixer.play("shoot22");
        error(
            [&] {
                static_cast<void>(mixer.render(1));
            },
            "offline mixer");
        const auto deadline = SDL_GetTicks() + 2000;
        while (mixer.playing(shot) && SDL_GetTicks() < deadline) {
            SDL_Delay(5);
        }
        require(!mixer.playing(shot) && mixer.active_voices() == 1, "device callback did not finish shot or preserve loop");
        mixer.stop_all();
        require(mixer.active_voices() == 0, "device stop left active voices");
    }
    silent_guard();
    std::cout << "pass: dummy playback callback, event deduplication, offline render guard and cleanup\n";
}

void game_assets(const fs::path& root) {
    const auto manifest_before = read(root / "manifest.txt");
    std::vector<std::pair<fs::path, std::string>> hashes;
    std::vector<contracts::AssetId> ids;
    auto mixer = audio::AudioEngine::offline({64, 1});
    {
        auto store = AssetStore::load(root / "manifest.txt");
        ids.assign(store.audio_ids().begin(), store.audio_ids().end());
        for (const auto& id : ids) {
            const auto path = root / store.audio(id.value).path;
            hashes.emplace_back(path, moorhuhn::tests::checksum(read(path)));
        }
        mixer.preload(store);
    }
    require(mixer.sound_count() == 19, "did not preload every game asset sound");
    for (const auto& item : ids) {
        const auto& id = item.value;
        const auto info = mixer.sound(id);
        const auto voice = mixer.play(id);
        const auto pcm = mixer.render(info.decoded_frames + 128);
        require(pcm.frames_with_audio == info.decoded_frames, "game asset mixed duration differs");
        zero(std::span(pcm.samples).subspan(info.decoded_frames));
        require(!mixer.playing(voice), "game asset sound did not end");
        std::cout << "sound=" << id << " decoded_frames=" << info.decoded_frames << " onset=" << onset(std::span(pcm.samples).first(info.decoded_frames))
                  << " energy=" << energy(pcm.samples) << '\n';
    }
    // SDL_MixAudio clips sums. Low gains keep this exact linear-sum check below clipping.
    auto shot = mixer.play("shoot22", {0.25F, 0});
    const auto shot_pcm = mixer.render(59555);
    require(shot_pcm.frames_with_audio == 59555, "shot length differs");
    // MIX observes EOF on the next generation when the request ends exactly at EOF.
    zero(mixer.render(1).samples);
    require(!mixer.playing(shot), "shot did not retire after EOF was observed");
    static_cast<void>(mixer.play("reload22", {0.25F, 0}));
    const auto reload_pcm = mixer.render(59555);
    static_cast<void>(mixer.play("shoot22", {0.25F, 0}));
    static_cast<void>(mixer.play("reload22", {0.25F, 0}));
    const auto overlap = mixer.render(59555);
    for (std::size_t n = 0; n < overlap.samples.size(); ++n) {
        const auto expected = shot_pcm.samples[n] + reload_pcm.samples[n];
        require(std::abs(expected) < 0.9F, "game asset linear-sum check has insufficient headroom");
        require(std::abs(overlap.samples[n] - expected) < 1e-5F, "game asset shot/reload overlap differs");
    }
    static_cast<void>(mixer.play("bird22", {1.0F, -1}));
    static_cast<void>(mixer.render(128));
    mixer.stop_all();
    zero(mixer.render(128).samples);
    require(read(root / "manifest.txt") == manifest_before, "game asset manifest changed");
    for (const auto& [path, hash] : hashes) {
        require(moorhuhn::tests::checksum(read(path)) == hash, "game asset audio changed");
    }
    silent_guard();
    std::cout << "game assets: 19 sounds decoded, exact lengths, zero fill, overlap and round stop pass; no device/window/subsystem\n";
}
} // namespace

int main(int argc, char** argv) {
    try {
        fs::path fixture, asset_root, asset_fixture;
        bool dummy_device{};
        for (int n = 1; n < argc; ++n) {
            const std::string_view argument = argv[n];
            if (argument == "--device") {
                dummy_device = true;
                continue;
            }
            require(n + 1 < argc, "missing argument value");
            if (argument == "--fixtures") {
                fixture = argv[++n];
            } else if (argument == "--assets") {
                asset_root = argv[++n];
            } else if (argument == "--asset-fixtures") {
                asset_fixture = argv[++n];
            } else {
                throw std::runtime_error("unknown argument");
            }
        }
        require(!fixture.empty(), "--fixtures is required");
        if (asset_fixture.empty()) {
            asset_fixture = fixture.parent_path() / "assets/loader";
        }
        if (dummy_device) {
            const auto* driver = SDL_getenv("SDL_AUDIODRIVER");
            require(driver && std::string_view(driver) == "dummy", "--device requires SDL_AUDIODRIVER=dummy");
            require(asset_root.empty(), "--device does not accept --assets");
        }
        // Driver locks prevent hardware audio and host video, including on failures.
        require(SDL_SetHintWithPriority(SDL_HINT_AUDIO_DRIVER, dummy_device ? "dummy" : "moorhuhn-disabled", SDL_HINT_OVERRIDE), "cannot lock out host audio");
        require(SDL_SetHintWithPriority(SDL_HINT_VIDEO_DRIVER, "moorhuhn-disabled", SDL_HINT_OVERRIDE), "cannot lock out host video");
        silent_guard();
        if (dummy_device) {
            device(fixture);
        } else if (asset_root.empty()) {
            synthetic(fixture, asset_fixture);
        } else {
            game_assets(asset_root);
        }
        silent_guard();
        SDL_ResetHints();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
