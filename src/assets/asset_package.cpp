#include "assets/asset_package.hpp"

#include <SDL3/SDL_stdinc.h>

#include <algorithm>
#include <fstream>
#include <stdexcept>

namespace moorhuhn::assets::detail {
namespace {

std::uint64_t number(std::span<const std::uint8_t> bytes, std::size_t at, std::size_t width) {
    if (at > bytes.size() || width > bytes.size() - at) {
        throw std::runtime_error("Truncated asset package");
    }

    std::uint64_t value{};

    for (std::size_t i = 0; i < width; ++i) {
        value |= std::uint64_t(bytes[at + i]) << (i * 8);
    }

    return value;
}

void require(bool condition, const char* cause) {
    if (!condition) {
        throw std::runtime_error(cause);
    }
}

bool safe_name(std::string_view name) {
    if (name.empty() || name.size() > 240) {
        return false;
    }

    std::size_t start{};

    while (start < name.size()) {
        const auto end = name.find('/', start);
        const auto part = name.substr(start, end == std::string_view::npos ? end : end - start);

        if (part.empty() || part.front() == '.' || part.back() == '.') {
            return false;
        }

        for (char ch : part) {
            if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_' || ch == '-' || ch == '.')) {
                return false;
            }
        }

        if (end == std::string_view::npos) {
            return true;
        }

        start = end + 1;
    }

    return false;
}
} // namespace

AssetPackage::AssetPackage(const std::filesystem::path& path) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    require(!ec && size >= 16 && size <= 100'000'000, "Missing or oversized asset package");
    std::ifstream file(path, std::ios::binary);
    require(bool(file), "Cannot open asset package");
    bytes_.resize(static_cast<std::size_t>(size));
    file.read(reinterpret_cast<char*>(bytes_.data()), static_cast<std::streamsize>(bytes_.size()));
    require(bool(file), "Cannot read asset package");
    const std::span<const std::uint8_t> bytes(bytes_);
    require(std::equal(bytes.begin(), bytes.begin() + 8, "MHASSETS"), "Invalid asset package magic");
    const auto count = number(bytes, 8, 4);
    const auto index_size = number(bytes, 12, 4);
    require(count > 0 && count <= 256 && index_size <= size - 16, "Invalid asset package index");
    const auto payload_start = static_cast<std::size_t>(16 + index_size);
    std::size_t cursor = 16;
    std::size_t expected_offset = payload_start;
    std::string previous;

    for (std::uint64_t i = 0; i < count; ++i) {
        require(cursor + 22 <= payload_start, "Truncated asset package entry");
        const auto name_size = number(bytes, cursor, 2);
        const auto offset = number(bytes, cursor + 2, 8);
        const auto length = number(bytes, cursor + 10, 8);
        const auto checksum = number(bytes, cursor + 18, 4);
        require(name_size > 0 && name_size <= 240 && name_size <= payload_start - cursor - 22, "Invalid asset package name length");
        const std::string name(reinterpret_cast<const char*>(bytes.data() + cursor + 22), static_cast<std::size_t>(name_size));
        require(safe_name(name) && (previous.empty() || previous < name), "Unsafe or unordered asset path");
        require(offset == expected_offset && length <= size - expected_offset, "Invalid asset package offset");
        require(SDL_crc32(0, bytes.data() + expected_offset, static_cast<std::size_t>(length)) == checksum, "Asset package checksum mismatch");
        entries_.emplace(name, std::pair{expected_offset, static_cast<std::size_t>(length)});
        previous = name;
        expected_offset += static_cast<std::size_t>(length);
        cursor += 22 + static_cast<std::size_t>(name_size);
    }

    require(cursor == payload_start && expected_offset == bytes.size(), "Asset package contains extra data");
}

std::span<const std::uint8_t> AssetPackage::read(std::string_view name) const {
    const auto found = entries_.find(std::string(name));

    if (found == entries_.end()) {
        throw std::runtime_error("Missing asset package entry: " + std::string(name));
    }

    return std::span(bytes_).subspan(found->second.first, found->second.second);
}

} // namespace moorhuhn::assets::detail
