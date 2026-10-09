#pragma once

#include "contracts/assets.hpp"

#include <array>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace moorhuhn::assets {

class AssetError final : public std::runtime_error {
public:
    AssetError(std::string id, std::string path, std::string cause);

    [[nodiscard]] const std::string& id() const noexcept {
        return id_;
    }

    [[nodiscard]] const std::string& path() const noexcept {
        return path_;
    }

    [[nodiscard]] const std::string& cause() const noexcept {
        return cause_;
    }

private:
    std::string id_;
    std::string path_;
    std::string cause_;
};

struct ImageMetadata {
    contracts::AssetId id;
    contracts::AssetId palette;
    std::uint32_t active_frames{}; // Animation range; stored frames remain addressable.
    std::uint32_t stored_frames{};
    std::optional<std::uint8_t> color_key;
};

// Validated stereo 48000 Hz MP3. AudioEngine owns decoded playback data.
struct AudioView {
    contracts::AssetId id;
    std::string path;
    std::uint32_t original_sample_frames{};
    std::uint32_t original_sample_rate{};
    std::uint32_t decoded_48000hz_samples{};
    double source_duration_seconds{}; // Source frames / source rate, before resampling.
    std::span<const std::uint8_t> encoded;
};

// Bytes 0..2 are RGB. Byte 3 is metadata, not alpha.
using PaletteEntry = std::array<std::uint8_t, 4>;

struct LookupView {
    std::string id; // Exact manifest path; lookup records have no name.
    std::int32_t width{};
    std::int32_t height{};
    std::uint32_t row_stride{}; // Pixels per row.
    std::span<const contracts::RgbaPixel> rgba;
    std::span<const std::uint8_t> red;
};

// Loads PNG/MP3 assets from a directory or paired packages without extraction.
// Failure publishes no store; all temporary storage is released.
// Views borrow storage. Moving preserves source views; assignment expires destination views.
// RGBA and indices stay separate and read-only. Consumers supply masks and index keys.
class AssetStore final {
public:
    [[nodiscard]] static AssetStore load(const std::filesystem::path& manifest);
    [[nodiscard]] static AssetStore load_package(const std::filesystem::path& directory);
    ~AssetStore();
    AssetStore(AssetStore&&) noexcept;
    AssetStore& operator=(AssetStore&&) noexcept;
    AssetStore(const AssetStore&) = delete;
    AssetStore& operator=(const AssetStore&) = delete;

    [[nodiscard]] contracts::AssetView image(std::string_view id) const;
    [[nodiscard]] ImageMetadata image_metadata(std::string_view id) const;
    [[nodiscard]] contracts::AtlasRect frame(const contracts::FrameId& id) const;
    [[nodiscard]] AudioView audio(std::string_view id) const;
    [[nodiscard]] std::span<const PaletteEntry> palette(std::string_view id) const;
    [[nodiscard]] LookupView lookup(std::string_view id) const;
    [[nodiscard]] std::span<const std::uint32_t> tabel1() const;
    [[nodiscard]] std::span<const std::uint32_t> tabel2() const;
    [[nodiscard]] std::span<const contracts::AssetId> image_ids() const;
    [[nodiscard]] std::span<const contracts::AssetId> audio_ids() const;
    [[nodiscard]] const std::filesystem::path& root() const;

private:
    struct Storage;
    [[nodiscard]] static AssetStore load_impl(const std::filesystem::path& path, bool packaged);
    explicit AssetStore(std::unique_ptr<Storage> storage);
    [[nodiscard]] const Storage& storage() const;
    std::unique_ptr<Storage> storage_;
};

} // namespace moorhuhn::assets
