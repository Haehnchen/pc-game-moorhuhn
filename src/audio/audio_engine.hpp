#pragma once

#include "assets/asset_store.hpp"
#include "contracts/events.hpp"

#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace moorhuhn::audio {

class AudioError final : public std::runtime_error {
public:
    AudioError(std::string operation, std::string sound, std::string cause);

    [[nodiscard]] const std::string& operation() const noexcept {
        return operation_;
    }

    [[nodiscard]] const std::string& sound() const noexcept {
        return sound_;
    }

    [[nodiscard]] const std::string& cause() const noexcept {
        return cause_;
    }

private:
    std::string operation_;
    std::string sound_;
    std::string cause_;
};

struct MixerOptions {
    std::uint32_t voices{64}; // Fixed track pool. Exhaustion reports an error.
    std::uint32_t channels{2}; // Offline output is float PCM at 48000 Hz.
};

struct PlayOptions {
    float volume{1.0F}; // Linear gain in [0, 1].
    std::int32_t repeats{}; // Additional passes: 0 once, positive finite, -1 until stopped.
};

struct VoiceId {
    std::uint64_t value{}; // Zero is invalid. Reusing a track expires its old ID.

    auto operator<=>(const VoiceId&) const = default;
};

enum class SoundAction {
    play,
    stop_sound,
    stop_all
};

// The caller maps game effect names to playback or stop actions.
struct EffectSound {
    std::string effect;
    SoundAction action{SoundAction::play};
    contracts::AssetId sound;
    PlayOptions options;
};

struct SoundInfo {
    contracts::AssetId id;
    std::uint32_t decoded_frames{}; // 48000 Hz frames in the decoded audio.
    std::uint32_t original_frames{};
    std::uint32_t original_rate{};
    double source_seconds{};
};

struct EventFailure {
    std::uint64_t sequence{};
    std::string message;
};

struct ConsumeResult {
    std::size_t consumed{};
    std::size_t duplicates{};
    std::size_t started{};
    std::size_t stopped{};
    std::vector<EventFailure> failures;
};

struct RenderedAudio {
    std::vector<float> samples; // Interleaved; includes appended zero fill.
    std::uint32_t frames_with_audio{}; // Excludes the mixer's final zero fill.
};

// Owns MIX initialization, decoded sounds, mixer, and tracks. Use on one main thread.
// No calls change simulation time. Asset views are borrowed only during preload.
class AudioEngine final {
public:
    [[nodiscard]] static AudioEngine offline(MixerOptions options = {});
    // Uses a playback device, including SDL's default playback ID.
    [[nodiscard]] static AudioEngine device(std::uint32_t playback_device, MixerOptions options = {});
    ~AudioEngine();
    AudioEngine(AudioEngine&&) noexcept;
    AudioEngine& operator=(AudioEngine&&) noexcept;
    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    // Transactional replacement, permitted only when every voice is stopped.
    void preload(const assets::AssetStore& store);
    void preload(std::span<const assets::AudioView> sounds);

    [[nodiscard]] std::size_t sound_count() const;
    [[nodiscard]] SoundInfo sound(std::string_view id) const;
    [[nodiscard]] std::uint32_t channels() const;
    [[nodiscard]] std::uint32_t sample_rate() const;
    [[nodiscard]] bool has_device() const;

    [[nodiscard]] VoiceId play(std::string_view sound, PlayOptions options = {});
    void restart(VoiceId voice, PlayOptions options = {});
    void stop(VoiceId voice);
    void stop_sound(std::string_view sound);
    void stop_all();
    void volume(VoiceId voice, float value);
    void master_volume(float value);

    // At an exact source-end request, MIX observes EOF on the next generation.
    [[nodiscard]] bool playing(VoiceId voice) const;
    [[nodiscard]] std::size_t active_voices() const;

    void set_event_map(std::span<const EffectSound> mappings);
    // Validate order first. Skip old sequences; record failed new events once.
    // Unmapped presentation effects are consumed without playing a sound.
    [[nodiscard]] ConsumeResult consume(std::span<const contracts::GameEvent> events);
    void reset_events(); // Stop voices and clear sequence tracking before consuming a new event stream.
    [[nodiscard]] std::uint64_t last_sequence() const;

    [[nodiscard]] RenderedAudio render(std::uint32_t frames); // Offline mixers only.

private:
    struct Storage;
    explicit AudioEngine(std::unique_ptr<Storage> storage);
    [[nodiscard]] Storage& storage();
    [[nodiscard]] const Storage& storage() const;
    std::unique_ptr<Storage> storage_;
};

} // namespace moorhuhn::audio
