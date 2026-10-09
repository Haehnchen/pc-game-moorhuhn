#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace moorhuhn::assets::detail {

// MHASSETS: 8-byte magic, LE32 count and index size, then sorted entries.
// Each entry is LE16 name size, LE64 offset and size, LE32 CRC32, and name bytes.
// Stored payloads follow the index without padding or recompression.
class AssetPackage {
public:
    explicit AssetPackage(const std::filesystem::path& path);
    [[nodiscard]] std::span<const std::uint8_t> read(std::string_view name) const;

    [[nodiscard]] const std::map<std::string, std::pair<std::size_t, std::size_t>>& entries() const noexcept {
        return entries_;
    }

private:
    std::vector<std::uint8_t> bytes_;
    std::map<std::string, std::pair<std::size_t, std::size_t>> entries_;
};

} // namespace moorhuhn::assets::detail
