#include "storage/highscores.hpp"

#include "../fixtures/ui.hpp"

#include <chrono>
#include <fstream>
#include <iterator>

namespace {
std::string read(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), {}};
}

struct Temporary {
    std::filesystem::path path = std::filesystem::temp_directory_path() / ("moorhuhn-highscores-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));

    Temporary() {
        if (!std::filesystem::create_directory(path)) {
            throw std::runtime_error("Temporary storage collision");
        }
    }

    ~Temporary() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
};
} // namespace

int main() {
    try {
        using namespace moorhuhn::storage;
        for (const auto& [value, expected] : std::array<std::pair<int, int>, 6>{{{600, 0}, {450, 1}, {500, 1}, {100, -1}, {99, -1}, {0, -1}}}) {
            HighScores scores;
            const auto before = scores;
            test::require(insert(scores, "Player", value) == expected, "Score insertion boundary");
            if (expected < 0) {
                test::require(scores == before, "Rejected score leaves entries unchanged");
            } else {
                test::require(scores.entries[static_cast<std::size_t>(expected)] == ScoreEntry{"Player", value}, "Inserted name/score");
            }
        }
        HighScores ties;
        ties.entries[1].score = 500;
        test::require(insert(ties, "Third", 500) == 1 && ties.entries[2].score == 500, "Equal score advances once across multiple ties");
        test::require(valid_name(std::string(1, 127)) && valid_name("0123456789") && !valid_name("01234567890") && !valid_name("\x80") && !valid_name("a\nb"),
            "Source byte and ten-byte limits");
        Temporary temp;
        const auto file = temp.path / "nested" / "scores.txt";
        test::require(load(file).status == LoadStatus::missing, "Missing file gives defaults");
        HighScores scores;
        scores.player = "Ten letters"; // Eleven bytes must never replace a file.
        test::require(!save(file, scores).empty() && !std::filesystem::exists(file), "Invalid save rejected before IO");
        scores.player = "A B";
        (void)insert(scores, "A B", 600);
        test::require(save(file, scores).empty(), "Atomic initial save");
        test::require(load(file).status == LoadStatus::loaded && load(file).scores == scores, "Fresh load persists all rows/player");
        const auto original = read(file);
        scores.entries[0].score = -1;
        test::require(!save(file, scores).empty() && read(file) == original, "Invalid replacement preserves valid file");
        scores.entries[0].score = 700;
        test::require(save(file, scores).empty() && load(file).scores == scores, "Atomic replacement of existing file");
        const auto directory = temp.path / "blocked.txt";
        std::filesystem::create_directory(directory);
        test::require(!save(directory, scores).empty() && std::filesystem::is_directory(directory), "Rename failure preserves destination");
        test::require(load(directory).status == LoadStatus::io_error, "Unreadable file-kind failure reports IO error");
        test::require(std::distance(std::filesystem::directory_iterator(temp.path), std::filesystem::directory_iterator{}) == 2, "Failure cleans temporary files");
        const auto corrupt = temp.path / "corrupt.txt";
        const HighScores defaults;
        const std::array<std::string, 5> invalid{"", "MHSCORES 2\n", "MHSCORES 1\n\"\"\n", "MHSCORES 1\n\"\"\n\"x\" 999999999999999999999\n", "invalid"};
        for (const auto& data : invalid) {
            {
                std::ofstream output(corrupt);
                output << data;
            }
            const auto loaded = load(corrupt);
            test::require(loaded.status == LoadStatus::invalid && loaded.scores == defaults && !loaded.error.empty(), "Corrupt file reports failure and preserves usable defaults");
        }
        auto malformed = read(file);
        const auto value = malformed.find("700");
        malformed.replace(value, 3, "999999999999999999999");
        {
            std::ofstream output(corrupt);
            output << malformed;
        }
        test::require(load(corrupt).status == LoadStatus::invalid, "Huge numeric score rejected with otherwise complete five rows");
        auto unsorted = read(file);
        unsorted.replace(unsorted.find("700"), 3, "50");
        {
            std::ofstream output(corrupt);
            output << unsorted;
        }
        test::require(load(corrupt).status == LoadStatus::invalid, "Unsorted complete table rejected");
        for (const auto& suffix : {"extra", "\"sixth\" 0"}) {
            std::ofstream output(corrupt);
            output << read(file) << suffix;
            output.close();
            test::require(load(corrupt).status == LoadStatus::invalid, "Trailing data rejected");
        }
        scores.player = "A\"B\\C";
        scores.entries[0].name = "A\"B\\C";
        test::require(save(file, scores).empty() && load(file).scores == scores, "Quoted names round trip");
        std::cout << "PASS highscores " << test::checks << " checks\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
