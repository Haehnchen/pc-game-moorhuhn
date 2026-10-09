#include "app/audio_clock.hpp"

#include <cstdint>
#include <iostream>
#include <stdexcept>

int main() {
    try {
        moorhuhn::app::AudioFrameClock clock;
        std::uint64_t frames{};
        for (std::int64_t us = 1; us < 1'000'000; us += 7'919) {
            frames += clock.advance({us});
        }
        frames += clock.advance({1'000'000});
        if (frames != 48'000 || clock.advance({1'000'000}) != 0) {
            throw std::runtime_error("Fractional frames were lost");
        }
        bool rejected{};
        try {
            static_cast<void>(clock.advance({999'999}));
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        if (!rejected) {
            throw std::runtime_error("Backward time was accepted");
        }
        std::cout << "Audio clock passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
