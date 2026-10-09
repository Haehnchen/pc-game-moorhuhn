#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace moorhuhn::storage {
struct ScoreEntry {
    std::string name;
    std::int32_t score{};

    bool operator==(const ScoreEntry&) const = default;
};

struct HighScores {
    std::string player;
    std::array<ScoreEntry, 5> entries{{{"Witan", 500}, {"Witan", 400}, {"Witan", 300}, {"Witan", 200}, {"Witan", 100}}};

    bool operator==(const HighScores&) const = default;
};

[[nodiscard]] bool valid_name(std::string_view);
void validate(const HighScores&);
// Advance once past the first equal score. Return -1 when the table is unchanged.
[[nodiscard]] int insert(HighScores&, std::string_view name, std::int32_t score);
enum class LoadStatus {
    loaded,
    missing,
    invalid,
    io_error
};

struct LoadResult {
    HighScores scores;
    LoadStatus status;
    std::string error;
};

[[nodiscard]] LoadResult load(const std::filesystem::path&);
// Same-directory atomic rename. Any failure preserves the old destination.
[[nodiscard]] std::string save(const std::filesystem::path&, const HighScores&);
} // namespace moorhuhn::storage
