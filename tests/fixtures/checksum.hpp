#pragma once

#include <SDL3/SDL_stdinc.h>

#include <cstdint>
#include <format>
#include <span>
#include <string>

namespace moorhuhn::tests {

inline std::string checksum(std::span<const std::uint8_t> bytes) {
    return std::format("{:08x}", SDL_crc32(0, bytes.data(), bytes.size()));
}

} // namespace moorhuhn::tests
