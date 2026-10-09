#pragma once

#include "audio/audio_engine.hpp"

namespace moorhuhn::app {

// Shared production mapping for playback and headless runtime checks.
inline std::span<const audio::EffectSound> game_sound_map() {
    using enum audio::SoundAction;
    static const std::vector<audio::EffectSound> sounds{
        {"round.ambience.start", play, {"bird22"}, {1, -1}},
        {"world.sound.big.start", play, {"big22"}, {}},
        {"world.sound.plane.start", play, {"plane22"}, {0.177827941F, -1}},
        {"world.sound.plane.stop", stop_sound, {"plane22"}, {}},
        {"world.sound.plane_hit.stop", stop_sound, {"plane222"}, {}},
        {"world.sound.explosion", play, {"explo22"}, {0.562341325F, 0}},
        {"world.sound.balloon.stop", stop_sound, {"balon22"}, {}},
        {"world.sound.fall.large", play, {"fall22"}, {0.562341325F, 0}},
        {"world.sound.fall.medium", play, {"fall22"}, {0.316227766F, 0}},
        {"world.sound.fall.small", play, {"fall22"}, {0.1F, 0}},
        {"world.sound.fall.close", play, {"fall22"}, {}},
        {"world.sound.fall.balloon", play, {"fall22"}, {0.177827941F, 0}},
        {"specials.sound.hit.large", play, {"hit22"}, {0.562341325F, 0}},
        {"specials.sound.hit.medium", play, {"hit22"}, {0.316227766F, 0}},
        {"specials.sound.hit.wing", play, {"hit22"}, {0.316227766F, 0}},
        {"specials.sound.hit.scare", play, {"hit22"}, {0.316227766F, 0}},
        {"specials.sound.hit.small", play, {"hit22"}, {0.1F, 0}},
        {"specials.sound.hit.hat", play, {"hit22"}, {0.562341325F, 0}},
        {"specials.sound.hit.close", play, {"hit222"}, {}},
        {"specials.sound.balloon.start", play, {"balon22"}, {1, -1}},
        {"specials.sound.plane_hit.start", play, {"plane222"}, {0.177827941F, -1}},
    };

    return sounds;
}

} // namespace moorhuhn::app
