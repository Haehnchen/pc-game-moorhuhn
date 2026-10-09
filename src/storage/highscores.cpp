#include "storage/highscores.hpp"

#include <atomic>
#include <charconv>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace moorhuhn::storage {
namespace {
HighScores decode(std::string_view contents) {
    std::istringstream input{std::string(contents)};
    std::string header;
    std::getline(input, header);
    if (header != "MHSCORES 1") {
        throw std::invalid_argument("Unsupported high-score format");
    }
    HighScores result;
    if (input.peek() != '"' || !(input >> std::quoted(result.player))) {
        throw std::invalid_argument("Invalid player name");
    }
    for (auto& row : result.entries) {
        input >> std::ws;
        std::string score;
        if (input.peek() != '"' || !(input >> std::quoted(row.name) >> score)) {
            throw std::invalid_argument("High scores require five rows");
        }
        const auto [end, error] = std::from_chars(score.data(), score.data() + score.size(), row.score);
        if (error != std::errc{} || end != score.data() + score.size() || row.score < 0) {
            throw std::invalid_argument("High-score value outside int32 range");
        }
    }
    input >> std::ws;
    if (!input.eof()) {
        throw std::invalid_argument("Unexpected high-score data");
    }
    validate(result);
    return result;
}
} // namespace

bool valid_name(std::string_view name) {
    if (name.size() > 10) {
        return false;
    }

    for (unsigned char c : name) {
        if (c < 32 || c > 127) {
            return false;
        }
    }

    return true;
}

void validate(const HighScores& scores) {
    if (!valid_name(scores.player)) {
        throw std::invalid_argument("Invalid player name");
    }

    std::int32_t prior = std::numeric_limits<std::int32_t>::max();

    for (const auto& row : scores.entries) {
        if (!valid_name(row.name) || row.score < 0 || row.score > prior) {
            throw std::invalid_argument("Invalid or unsorted high-score row");
        }

        prior = row.score;
    }
}

int insert(HighScores& scores, std::string_view name, std::int32_t score) {
    validate(scores);

    if (!valid_name(name) || score < 0) {
        throw std::invalid_argument("Invalid score insertion");
    }

    std::size_t index = 0;

    while (index < 5 && scores.entries[index].score > score) {
        ++index;
    }

    if (index < 5 && scores.entries[index].score == score) {
        ++index;
    }

    if (index >= 5) {
        return -1;
    }

    for (std::size_t i = 4; i > index; --i) {
        scores.entries[i] = scores.entries[i - 1];
    }

    scores.entries[index] = {std::string(name), score};

    return static_cast<int>(index);
}

LoadResult load(const std::filesystem::path& path) {
    std::error_code error;
    const bool exists = std::filesystem::exists(path, error);

    if (error) {
        return {{}, LoadStatus::io_error, error.message()};
    }

    if (!exists) {
        return {{}, LoadStatus::missing, {}};
    }

    const auto size = std::filesystem::file_size(path, error);

    if (error) {
        return {{}, LoadStatus::io_error, error.message()};
    }

    if (size > 65536) {
        return {{}, LoadStatus::invalid, "High-score file exceeds 65536 bytes"};
    }

    std::ifstream input(path, std::ios::binary);

    if (!input) {
        return {{}, LoadStatus::io_error, "Cannot read high-score file"};
    }

    try {
        const std::string contents{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};

        if (input.bad()) {
            return {{}, LoadStatus::io_error, "Cannot finish reading high-score file"};
        }

        return {decode(contents), LoadStatus::loaded, {}};
    } catch (const std::exception& failure) {
        return {{}, LoadStatus::invalid, failure.what()};
    }
}

std::string save(const std::filesystem::path& path, const HighScores& scores) {
    std::filesystem::path temporary;

    try {
        validate(scores);

        if (path.empty() || path.filename().empty()) {
            throw std::runtime_error("Invalid high-score path");
        }

        auto parent = path.parent_path();

        if (parent.empty()) {
            parent = ".";
        }

        std::filesystem::create_directories(parent);

        if (std::filesystem::is_symlink(std::filesystem::symlink_status(path))) {
            throw std::runtime_error("High-score destination is a symlink");
        }

        static std::atomic<std::uint64_t> serial{};
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();

        for (int attempt = 0; attempt < 32; ++attempt) {
            temporary = parent / (".moorhuhn-scores-" + std::to_string(stamp) + "-" + std::to_string(serial++));

            if (std::filesystem::create_directory(temporary)) {
                break;
            }

            temporary.clear();
        }

        if (temporary.empty()) {
            throw std::runtime_error("Cannot reserve high-score temporary directory");
        }

        const auto staged = temporary / "scores.txt";
        std::ofstream output(staged, std::ios::binary | std::ios::trunc);

        if (!output) {
            throw std::runtime_error("Cannot write temporary high-score file");
        }

        output << "MHSCORES 1\n" << std::quoted(scores.player) << '\n';
        for (const auto& row : scores.entries) {
            output << std::quoted(row.name) << ' ' << row.score << '\n';
        }
        output.flush();

        if (!output) {
            throw std::runtime_error("Cannot flush temporary high-score file");
        }

        output.close();

        if (!output) {
            throw std::runtime_error("Cannot close temporary high-score file");
        }

        std::filesystem::rename(staged, path);
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);

        return {};
    } catch (const std::exception& failure) {
        if (!temporary.empty()) {
            std::error_code ignored;
            std::filesystem::remove_all(temporary, ignored);
        }

        return failure.what();
    }
}
} // namespace moorhuhn::storage
