#include "audio/audio_engine.hpp"

#include <SDL3/SDL.h>
#include <SDL3_mixer/SDL_mixer.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>

namespace moorhuhn::audio {
namespace {

[[noreturn]] void failure(std::string operation, std::string sound, std::string cause) {
    throw AudioError(std::move(operation), std::move(sound), std::move(cause));
}

void check(bool success, std::string_view operation, std::string_view sound = {}) {
    if (!success) {
        failure(std::string(operation), std::string(sound), SDL_GetError());
    }
}

void valid_volume(float value) {
    if (!std::isfinite(value) || value < 0.0F || value > 1.0F) {
        failure("volume", {}, "expected a finite value in [0, 1]");
    }
}

void valid_play(PlayOptions options) {
    valid_volume(options.volume);

    if (options.repeats < -1) {
        failure("repeat", {}, "expected -1 or a nonnegative repeat count");
    }
}

void valid_mixer(MixerOptions options) {
    if (options.voices == 0 || options.voices > 256 || options.channels == 0 || options.channels > 2) {
        failure("create mixer", {}, "expected 1..256 voices and 1..2 channels");
    }
}

struct Library {
    Library() {
        check(MIX_Init(), "MIX_Init");
    }

    ~Library() {
        MIX_Quit();
    }
};

struct MixerDelete {
    void operator()(MIX_Mixer* p) const {
        MIX_DestroyMixer(p);
    }
};

struct AudioDelete {
    void operator()(MIX_Audio* p) const {
        MIX_DestroyAudio(p);
    }
};

struct TrackDelete {
    void operator()(MIX_Track* p) const {
        MIX_DestroyTrack(p);
    }
};

struct IoDelete {
    void operator()(SDL_IOStream* p) const {
        SDL_CloseIO(p);
    }
};

using Mixer = std::unique_ptr<MIX_Mixer, MixerDelete>;
using Audio = std::unique_ptr<MIX_Audio, AudioDelete>;
using Track = std::unique_ptr<MIX_Track, TrackDelete>;
using Io = std::unique_ptr<SDL_IOStream, IoDelete>;

struct Properties {
    SDL_PropertiesID id{};

    Properties() : id(SDL_CreateProperties()) {
        check(id != 0, "SDL_CreateProperties");
    }

    ~Properties() {
        SDL_DestroyProperties(id);
    }

    Properties(const Properties&) = delete;
    Properties& operator=(const Properties&) = delete;
};

void play_track(MIX_Track* track, std::string_view sound, PlayOptions options) {
    Properties properties;
    check(SDL_SetNumberProperty(properties.id, MIX_PROP_PLAY_LOOPS_NUMBER, options.repeats), "set repeats", sound);
    check(SDL_SetNumberProperty(properties.id, MIX_PROP_PLAY_APPEND_SILENCE_FRAMES_NUMBER, 0), "set padding", sound);
    check(MIX_SetTrackGain(track, options.volume), "MIX_SetTrackGain", sound);
    check(MIX_PlayTrack(track, properties.id), "MIX_PlayTrack", sound);
}
} // namespace

AudioError::AudioError(std::string operation, std::string sound, std::string cause)
    : std::runtime_error(operation + " [" + (sound.empty() ? "mixer" : sound) + "]: " + cause), operation_(std::move(operation)), sound_(std::move(sound)),
      cause_(std::move(cause)) {}

struct AudioEngine::Storage {
    struct Loaded {
        Audio audio;
        SoundInfo info;
    };

    struct Voice {
        Track track;
        VoiceId id;
        std::string sound;
    };

    Library library; // Last destroyed, after every MIX object.
    Mixer mixer;
    std::map<std::string, Loaded, std::less<>> sounds;
    std::vector<Voice> voices;

    std::map<std::string, EffectSound, std::less<>> mappings;
    std::uint64_t next_voice{1};
    std::uint64_t sequence{};

    std::uint32_t output_channels{};
    std::uint32_t output_rate{};
    bool device{};

    Storage(MixerOptions options, bool use_device, std::uint32_t playback_device) : device(use_device) {
        const SDL_AudioSpec requested{SDL_AUDIO_F32, static_cast<int>(options.channels), 48000};
        mixer.reset(use_device ? MIX_CreateMixerDevice(playback_device, &requested) : MIX_CreateMixer(&requested));
        check(mixer != nullptr, use_device ? "MIX_CreateMixerDevice" : "MIX_CreateMixer");
        SDL_AudioSpec actual{};
        check(MIX_GetMixerFormat(mixer.get(), &actual), "MIX_GetMixerFormat");
        output_channels = static_cast<std::uint32_t>(actual.channels);
        output_rate = static_cast<std::uint32_t>(actual.freq);
        voices.reserve(options.voices);

        for (std::uint32_t n = 0; n < options.voices; ++n) {
            Track track(MIX_CreateTrack(mixer.get()));
            check(track != nullptr, "MIX_CreateTrack");
            voices.push_back({std::move(track), {}, {}});
        }
    }

    Loaded& loaded(std::string_view id) {
        auto found = sounds.find(id);

        if (found == sounds.end()) {
            failure("lookup sound", std::string(id), "sound was not preloaded");
        }

        return found->second;
    }

    const Loaded& loaded(std::string_view id) const {
        auto found = sounds.find(id);

        if (found == sounds.end()) {
            failure("lookup sound", std::string(id), "sound was not preloaded");
        }

        return found->second;
    }

    Voice* find_voice(VoiceId id) {
        if (id.value == 0) {
            return nullptr;
        }

        auto found = std::find_if(voices.begin(), voices.end(), [id](const Voice& voice) {
            return voice.id == id;
        });

        return found == voices.end() ? nullptr : &*found;
    }

    Voice& voice(VoiceId id) {
        auto* result = find_voice(id);

        if (!result) {
            failure("lookup voice", {}, "unknown or expired voice ID");
        }

        return *result;
    }
};

AudioEngine::AudioEngine(std::unique_ptr<Storage> storage) : storage_(std::move(storage)) {}

AudioEngine::~AudioEngine() = default;
AudioEngine::AudioEngine(AudioEngine&&) noexcept = default;
AudioEngine& AudioEngine::operator=(AudioEngine&&) noexcept = default;

AudioEngine AudioEngine::offline(MixerOptions options) {
    valid_mixer(options);

    return AudioEngine(std::make_unique<Storage>(options, false, 0));
}

AudioEngine AudioEngine::device(std::uint32_t playback_device, MixerOptions options) {
    valid_mixer(options);

    if (playback_device == 0 || playback_device == SDL_AUDIO_DEVICE_DEFAULT_RECORDING) {
        failure("create device", {}, "expected an explicit playback device ID");
    }

    return AudioEngine(std::make_unique<Storage>(options, true, playback_device));
}

AudioEngine::Storage& AudioEngine::storage() {
    if (!storage_) {
        failure("audio engine", {}, "moved-from engine");
    }

    return *storage_;
}

const AudioEngine::Storage& AudioEngine::storage() const {
    if (!storage_) {
        failure("audio engine", {}, "moved-from engine");
    }

    return *storage_;
}

void AudioEngine::preload(const assets::AssetStore& store) {
    std::vector<assets::AudioView> views;
    views.reserve(store.audio_ids().size());

    for (const auto& id : store.audio_ids()) {
        views.push_back(store.audio(id.value));
    }

    preload(views);
}

void AudioEngine::preload(std::span<const assets::AudioView> views) {
    auto& data = storage();

    if (active_voices() != 0) {
        failure("preload", {}, "stop voices before replacing sounds");
    }

    decltype(data.sounds) loaded;

    for (const auto& view : views) {
        const auto& id = view.id.value;

        if (id.empty() || loaded.contains(id)) {
            failure("preload", id, "empty or duplicate sound ID");
        }

        if (view.encoded.empty() || view.original_sample_rate == 0 || view.original_sample_frames == 0 || view.decoded_48000hz_samples == 0) {
            failure("preload", id, "invalid audio source or sample metadata");
        }

        Io io(SDL_IOFromConstMem(view.encoded.data(), view.encoded.size()));
        check(io != nullptr, "SDL_IOFromConstMem", id);
        Audio audio(MIX_LoadAudio_IO(data.mixer.get(), io.get(), true, false));
        check(audio != nullptr, "MIX_LoadAudio_IO", id);
        SDL_AudioSpec format{};
        check(MIX_GetAudioFormat(audio.get(), &format), "MIX_GetAudioFormat", id);
        const auto length = MIX_GetAudioDuration(audio.get());
        const auto properties = MIX_GetAudioProperties(audio.get());
        check(properties != 0, "MIX_GetAudioProperties", id);
        const std::string_view decoder = SDL_GetStringProperty(properties, MIX_PROP_AUDIO_DECODER_STRING, "");

        if (decoder != "DRMP3" || format.format != SDL_AUDIO_F32LE || format.freq != 48000 || format.channels != 2) {
            failure("preload", id, "expected stereo 48000 Hz MP3 decoding");
        }

        if (length != view.decoded_48000hz_samples) {
            failure("preload", id, "decoded frame count differs from validated MP3 length");
        }

        const auto expected = std::llround(static_cast<double>(view.original_sample_frames) * 48000.0 / view.original_sample_rate);

        if (length < expected - 1 || length - expected >= 1152 || !std::isfinite(view.source_duration_seconds)
            || std::abs(view.source_duration_seconds - static_cast<double>(view.original_sample_frames) / view.original_sample_rate) > 1e-9) {
            failure("preload", id, "original and decoded sample lengths disagree");
        }

        SoundInfo info{view.id, view.decoded_48000hz_samples, view.original_sample_frames, view.original_sample_rate, view.source_duration_seconds};

        loaded.emplace(id, Storage::Loaded{std::move(audio), std::move(info)});
    }

    for (auto& voice : data.voices) {
        check(MIX_SetTrackAudio(voice.track.get(), nullptr), "clear track audio");
        voice.id = {};
        voice.sound.clear();
    }

    data.sounds.swap(loaded);
    data.mappings.clear();
}

std::size_t AudioEngine::sound_count() const {
    return storage().sounds.size();
}

SoundInfo AudioEngine::sound(std::string_view id) const {
    return storage().loaded(id).info;
}

std::uint32_t AudioEngine::channels() const {
    return storage().output_channels;
}

std::uint32_t AudioEngine::sample_rate() const {
    return storage().output_rate;
}

bool AudioEngine::has_device() const {
    return storage().device;
}

VoiceId AudioEngine::play(std::string_view id, PlayOptions options) {
    valid_play(options);
    auto& data = storage();
    auto& audio = data.loaded(id);
    auto found = std::find_if(data.voices.begin(), data.voices.end(), [](const auto& voice) {
        return !MIX_TrackPlaying(voice.track.get());
    });

    if (found == data.voices.end()) {
        failure("play", std::string(id), "voice capacity exhausted");
    }

    if (data.next_voice == std::numeric_limits<std::uint64_t>::max()) {
        failure("play", std::string(id), "voice ID counter exhausted");
    }

    found->id = {};
    found->sound = id;
    check(MIX_SetTrackAudio(found->track.get(), audio.audio.get()), "MIX_SetTrackAudio", id);
    play_track(found->track.get(), id, options);
    found->id = VoiceId{data.next_voice++};

    return found->id;
}

void AudioEngine::restart(VoiceId id, PlayOptions options) {
    valid_play(options);
    auto& voice = storage().voice(id);
    // SDL_mixer 3.2.4 retains buffered converted PCM when restarting a live track.
    check(MIX_StopTrack(voice.track.get(), 0), "MIX_StopTrack", voice.sound);
    play_track(voice.track.get(), voice.sound, options);
}

void AudioEngine::stop(VoiceId id) {
    auto& voice = storage().voice(id);
    check(MIX_StopTrack(voice.track.get(), 0), "MIX_StopTrack", voice.sound);
}

void AudioEngine::stop_sound(std::string_view id) {
    auto& data = storage();
    static_cast<void>(data.loaded(id));

    for (auto& voice : data.voices) {
        if (voice.sound == id) {
            check(MIX_StopTrack(voice.track.get(), 0), "MIX_StopTrack", id);
        }
    }
}

void AudioEngine::stop_all() {
    check(MIX_StopAllTracks(storage().mixer.get(), 0), "MIX_StopAllTracks");
}

void AudioEngine::volume(VoiceId id, float value) {
    valid_volume(value);
    auto& voice = storage().voice(id);
    check(MIX_SetTrackGain(voice.track.get(), value), "MIX_SetTrackGain", voice.sound);
}

void AudioEngine::master_volume(float value) {
    valid_volume(value);
    check(MIX_SetMixerGain(storage().mixer.get(), value), "MIX_SetMixerGain");
}

bool AudioEngine::playing(VoiceId id) const {
    const auto& data = storage();
    auto found = std::find_if(data.voices.begin(), data.voices.end(), [id](const auto& voice) {
        return id.value != 0 && voice.id == id;
    });

    return found != data.voices.end() && MIX_TrackPlaying(found->track.get());
}

std::size_t AudioEngine::active_voices() const {
    const auto& voices = storage().voices;

    return static_cast<std::size_t>(std::count_if(voices.begin(), voices.end(), [](const auto& voice) {
        return MIX_TrackPlaying(voice.track.get());
    }));
}

void AudioEngine::set_event_map(std::span<const EffectSound> mappings) {
    auto& data = storage();
    std::map<std::string, EffectSound, std::less<>> next;

    for (const auto& mapping : mappings) {
        valid_play(mapping.options);

        if (mapping.action != SoundAction::play && mapping.action != SoundAction::stop_sound && mapping.action != SoundAction::stop_all) {
            failure("event map", mapping.sound.value, "unknown sound action");
        }

        if (mapping.action != SoundAction::stop_all) {
            static_cast<void>(data.loaded(mapping.sound.value));
        }

        if (mapping.effect.empty() || !next.emplace(mapping.effect, mapping).second) {
            failure("event map", mapping.sound.value, "empty or duplicate effect name");
        }
    }

    data.mappings.swap(next);
}

ConsumeResult AudioEngine::consume(std::span<const contracts::GameEvent> events) {
    auto& data = storage();
    std::uint64_t previous{};

    for (const auto& event : events) {
        if (event.sequence == 0 || event.sequence <= previous) {
            failure("consume events", {}, "batch sequences must be nonzero and strictly increasing");
        }

        previous = event.sequence;
    }

    ConsumeResult result;

    for (const auto& event : events) {
        if (event.sequence <= data.sequence) {
            ++result.duplicates;
            continue;
        }

        data.sequence = event.sequence;
        ++result.consumed;

        try {
            if (const auto* sound_event = std::get_if<contracts::PlaySound>(&event.payload)) {
                static_cast<void>(play(sound_event->sound.value));
                ++result.started;
            } else {
                const auto& effect = std::get<contracts::PresentationEffect>(event.payload);
                auto found = data.mappings.find(effect.name);

                if (found == data.mappings.end()) {
                    continue;
                }

                const auto& mapping = found->second;

                if (mapping.action == SoundAction::play) {
                    static_cast<void>(play(mapping.sound.value, mapping.options));
                    ++result.started;
                } else {
                    if (mapping.action == SoundAction::stop_all) {
                        stop_all();
                    } else {
                        stop_sound(mapping.sound.value);
                    }

                    ++result.stopped;
                }
            }
        } catch (const AudioError& error) {
            result.failures.push_back({event.sequence, error.what()});
        }
    }

    return result;
}

void AudioEngine::reset_events() {
    stop_all();
    storage().sequence = 0;
}

std::uint64_t AudioEngine::last_sequence() const {
    return storage().sequence;
}

RenderedAudio AudioEngine::render(std::uint32_t frames) {
    auto& data = storage();

    if (data.device) {
        failure("render", {}, "PCM generation requires an offline mixer");
    }

    const auto sample_count = static_cast<std::uint64_t>(frames) * data.output_channels;

    if (sample_count > static_cast<std::uint64_t>(std::numeric_limits<int>::max()) / sizeof(float)) {
        failure("render", {}, "PCM request exceeds MIX_Generate byte limit");
    }

    RenderedAudio result{std::vector<float>(static_cast<std::size_t>(sample_count)), 0};

    if (frames == 0) {
        return result;
    }

    const auto bytes = static_cast<int>(sample_count * sizeof(float));
    const auto mixed = MIX_Generate(data.mixer.get(), result.samples.data(), bytes);
    check(mixed >= 0, "MIX_Generate");

    if (mixed > bytes || mixed % static_cast<int>(data.output_channels * sizeof(float)) != 0) {
        failure("render", {}, "unexpected MIX_Generate byte count");
    }

    result.frames_with_audio = static_cast<std::uint32_t>(mixed) / (data.output_channels * sizeof(float));

    return result;
}

} // namespace moorhuhn::audio
