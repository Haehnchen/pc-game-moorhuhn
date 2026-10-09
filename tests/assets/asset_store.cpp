#include "assets/asset_store.hpp"

#include "../fixtures/checksum.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <span>
#include <sstream>
#include <type_traits>
#include <vector>

namespace {
using Bytes = std::vector<std::uint8_t>;
using moorhuhn::assets::AssetError;
using moorhuhn::assets::AssetStore;
using moorhuhn::tests::checksum;
namespace fs = std::filesystem;

static_assert(!std::is_copy_constructible_v<AssetStore>);
static_assert(std::is_nothrow_move_constructible_v<AssetStore>);
static_assert(std::is_const_v<typename decltype(moorhuhn::contracts::AssetView::rgba)::element_type>);
static_assert(std::is_const_v<typename decltype(moorhuhn::assets::AudioView::encoded)::element_type>);

void require(bool condition, std::string_view cause) {
    if (!condition) {
        throw std::runtime_error(std::string(cause));
    }
}

Bytes read(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    require(bool(stream), "cannot read test input");
    return Bytes(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
}

void write(const fs::path& path, const Bytes& data) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    require(bool(stream), "cannot write temporary fixture");
}

std::string text(const fs::path& path) {
    const auto bytes = read(path);
    return {bytes.begin(), bytes.end()};
}

void write_text(const fs::path& path, const std::string& value) {
    write(path, Bytes(value.begin(), value.end()));
}

void replace(std::string& value, std::string_view before, std::string_view after) {
    const auto position = value.find(before);
    require(position != std::string::npos, "fixture text not found");
    value.replace(position, before.size(), after);
}

struct Temporary {
    fs::path root;

    explicit Temporary(const fs::path& fixture) {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        for (int attempt = 0; attempt < 100; ++attempt) {
            root = fs::temp_directory_path() / ("moorhuhn-loader-" + std::to_string(stamp) + "-" + std::to_string(attempt));
            if (fs::create_directory(root)) {
                break;
            }
            root.clear();
        }
        require(!root.empty(), "cannot create temporary fixture");
        fs::copy(fixture, root, fs::copy_options::recursive | fs::copy_options::overwrite_existing);
    }

    ~Temporary() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }
};

void error(const std::function<void()>& action, std::string_view cause, std::string_view id = {}) {
    try {
        action();
    } catch (const AssetError& failure) {
        require(!failure.id().empty(), "error omitted asset ID");
        require(!failure.path().empty(), "error omitted path");
        require(failure.cause().find(cause) != std::string::npos, "error cause differs");
        if (!id.empty()) {
            require(failure.id() == id, "error asset ID differs");
        }
        require(std::string(failure.what()).find(failure.path()) != std::string::npos, "error message omitted path");
        return;
    }
    throw std::runtime_error("expected AssetError: " + std::string(cause));
}

Bytes rgba_bytes(std::span<const moorhuhn::contracts::RgbaPixel> pixels) {
    Bytes result;
    result.reserve(pixels.size() * 4);
    for (auto pixel : pixels) {
        result.insert(result.end(), {pixel.r, pixel.g, pixel.b, pixel.a});
    }
    return result;
}

void synthetic(const fs::path& fixture) {
    std::size_t checks{};
    const auto run = [&](std::string_view name, const std::function<void()>& action) {
        action();
        ++checks;
        std::cout << "pass: " << name << '\n';
    };
    run("SDL CRC32 known vectors", [] {
        require(checksum({}) == "00000000", "empty checksum");
        const Bytes abc{'a', 'b', 'c'};
        require(checksum(abc) == "352441c2", "abc checksum");
        const std::string long_text = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
        require(checksum(Bytes(long_text.begin(), long_text.end())) == "171a3f5f", "long-text checksum");
        require(checksum(Bytes(1000000, 'a')) == "dc25bfbc", "million-a checksum");
    });
    run("exact pixels, red indices, metadata, audio, tables, move", [&] {
        auto store = AssetStore::load(fixture / "manifest.txt");
        const auto view = store.image("credits");
        std::istringstream expected(text(fixture / "pixels.txt"));
        require(view.width == 3 && view.height == 2 && view.row_stride == 3, "decoded layout differs");
        require(view.rgba.size() == 6 && view.palette_indices.size() == 6 && view.masks.empty(), "view sizes differ");
        for (std::size_t i = 0; i < 6; ++i) {
            unsigned r{}, g{}, b{}, alpha{}, index{};
            require(bool(expected >> r >> g >> b >> alpha >> index), "missing pixel fixture");
            require(
                view.rgba[i]
                    == moorhuhn::contracts::RgbaPixel{static_cast<std::uint8_t>(r), static_cast<std::uint8_t>(g), static_cast<std::uint8_t>(b), static_cast<std::uint8_t>(alpha)},
                "RGBA pixel differs");
            require(view.palette_indices[i] == index, "red index changed");
        }
        const auto metadata = store.image_metadata("credits");
        require(metadata.palette.value == "credits.pal" && metadata.active_frames == 1 && metadata.stored_frames == 1 && metadata.color_key == 18, "metadata differs");
        require(store.frame({{"credits"}, 0}) == moorhuhn::contracts::AtlasRect{0, 0, 3, 2}, "frame differs");
        require(store.audio("typo22").encoded.size() == fs::file_size(fixture / "audio/typo22.mp3") && store.audio("typo22").decoded_48000hz_samples == 1067, "audio differs");
        require(store.palette("credits.pal").size() == 256 && store.tabel1().size() == 600 && store.tabel2().size() == 600, "tables differ");
        auto moved = std::move(store);
        require(moved.image("credits").rgba.data() == view.rgba.data() && view.rgba[1].r == 13 && view.rgba[1].a == 0, "move invalidated view");
        require(moved.image_ids().size() == 1 && moved.audio_ids().size() == 1, "ID registry differs");
        error(
            [&] {
                static_cast<void>(moved.image("Credits"));
            },
            "unknown image ID", "Credits");
        error(
            [&] {
                static_cast<void>(moved.audio("credits"));
            },
            "unknown sound ID", "credits");
        error(
            [&] {
                static_cast<void>(moved.frame({{"credits"}, 1}));
            },
            "unknown frame index", "credits");
        error(
            [&] {
                static_cast<void>(moved.palette("missing"));
            },
            "unknown palette ID", "missing");
        error(
            [&] {
                static_cast<void>(moved.lookup("missing"));
            },
            "unknown lookup ID", "missing");
    });
    const auto mutate = [&](std::string_view name, std::string_view cause, const std::function<void(std::string&, const fs::path&)>& change) {
        run(name, [&] {
            Temporary temp(fixture);
            auto manifest = text(temp.root / "manifest.txt");
            change(manifest, temp.root);
            write_text(temp.root / "manifest.txt", manifest);
            error(
                [&] {
                    static_cast<void>(AssetStore::load(temp.root / "manifest.txt"));
                },
                cause);
        });
    };
    const auto replacement = [&](std::string_view before, std::string_view after, std::string_view cause) {
        mutate(before, cause, [&](std::string& manifest, const fs::path&) {
            replace(manifest, before, after);
        });
    };
    replacement("MHMANIFEST 1", "MHMANIFEST 2", "expected 1");
    replacement("tables tables.txt", "missing tables.txt", "expected tables");
    replacement("image credits ", "image Credits ", "unknown image ID");
    replacement("sound typo22 ", "sound unknown ", "unknown sound ID");
    replacement("credits credits.pal", "credits unknown.pal", "unknown or missing palette ID");
    replacement("indices/credits.png 73 1", "indices/credits.png 73 0", "integer out of bounds");
    replacement("indices/credits.png 73", "indices/credits.png 1", "byte count mismatch");
    replacement("frame 0 0 3 2", "frame 2 0 2 2", "frame rectangle exceeds atlas");
    replacement("frame 0 0 3 2", "frame 4294967295 0 2 2", "frame rectangle exceeds atlas");
    replacement("frame 0 0 3 2", "frame 4294967296 0 3 2", "integer out of bounds");
    replacement("frame 0 0 3 2", "frame -1 0 3 2", "integer out of bounds");
    replacement("frame 0 0 3 2", "frame 0.5 0 3 2", "integer out of bounds");
    replacement("frame 0 0 3 2", "frame 0 0 0 2", "integer out of bounds");
    replacement("1067 mp3 2", "1000 mp3 2", "MP3 sample length mismatch");
    replacement("mp3 2", "mp3 1", "expected stereo MP3");
    replacement("48000 1067", "0 1067", "integer out of bounds");
    replacement("0.022229166666666668", "1", "source duration disagrees");
    replacement("0.022229166666666668", "nan", "invalid numeric source duration");
    replacement("lookups 0", "lookups 1\nlookup indices/unknown.png 1", "unknown lookup ID");
    replacement("end", "end extra", "unexpected metadata data");
    mutate("truncated manifest", "truncated metadata", [](auto& manifest, const fs::path&) {
        manifest.resize(10);
    });
    mutate("duplicate image", "duplicate image ID", [](auto& manifest, const fs::path&) {
        replace(manifest, "images 1", "images 2");
        replace(manifest, "audio 1", "image credits credits.pal 18 indices/credits.png 73 1\nframe 0 0 3 2\naudio 1");
    });
    mutate("duplicate sound", "duplicate sound ID", [](auto& manifest, const fs::path&) {
        replace(manifest, "audio 1", "audio 2");
        const auto begin = manifest.find("sound ");
        const auto end = manifest.find('\n', begin);
        manifest.insert(end + 1, manifest.substr(begin, end - begin + 1));
    });
    for (const auto path : {"../escape.png", "/absolute.png", "C:/asset.png", "indices\\credits.png", "indices//credits.png", "indices/./credits.png", "indices/credits..png",
             "indices/credits.png.", "indices/CON.png", "indices/com1.png"}) {
        replacement("indices/credits.png", path, "unsafe relative path");
    }
    replacement("indices/credits.png", "audio/typo22.mp3", "expected role path");
    mutate("missing PNG", "cannot resolve file", [](auto&, const fs::path& root) {
        fs::remove(root / "indices/credits.png");
    });
    mutate("directory PNG", "expected regular file", [](auto&, const fs::path& root) {
        fs::remove(root / "indices/credits.png");
        fs::create_directory(root / "indices/credits.png");
    });
    mutate("corrupt PNG", "SDL PNG decode failed", [](auto& manifest, const fs::path& root) {
        write(root / "indices/credits.png", {'b', 'a', 'd'});
        replace(manifest, "indices/credits.png 73", "indices/credits.png 3");
    });
    mutate("truncated PNG", "SDL PNG decode failed", [](auto& manifest, const fs::path& root) {
        auto bytes = read(root / "indices/credits.png");
        bytes.resize(40);
        write(root / "indices/credits.png", bytes);
        replace(manifest, "indices/credits.png 73", "indices/credits.png 40");
    });
    mutate("corrupt MP3", "SDL MP3 decode failed", [](auto& manifest, const fs::path& root) {
        write(root / "audio/typo22.mp3", {'b', 'a', 'd'});
        replace(manifest, "audio/typo22.mp3 1196", "audio/typo22.mp3 3");
    });
    mutate("unknown palette", "unknown palette ID", [](auto&, const fs::path& root) {
        auto tables = text(root / "tables.txt");
        replace(tables, "palette credits.pal", "palette unknown.pal");
        write_text(root / "tables.txt", tables);
    });
    mutate("palette byte overflow", "integer out of bounds", [](auto&, const fs::path& root) {
        auto tables = text(root / "tables.txt");
        const auto start = tables.find('\n', tables.find("palette credits.pal")) + 1;
        tables.replace(start, tables.find(' ', start) - start, "256");
        write_text(root / "tables.txt", tables);
    });
    mutate("truncated table", "truncated metadata", [](auto&, const fs::path& root) {
        auto tables = text(root / "tables.txt");
        tables.resize(tables.find("tabel2") + 7);
        write_text(root / "tables.txt", tables);
    });
    mutate("duplicate palette", "duplicate palette ID", [](auto&, const fs::path& root) {
        auto tables = text(root / "tables.txt");
        replace(tables, "palettes 1", "palettes 2");
        const auto begin = tables.find("palette credits.pal");
        const auto end = tables.find("tabel1");
        tables.insert(end, tables.substr(begin, end - begin));
        write_text(root / "tables.txt", tables);
    });
    run("symlink confinement", [&] {
        Temporary temp(fixture);
        Temporary outside(fixture);
        fs::remove(temp.root / "indices/credits.png");
        std::error_code ec;
        fs::create_symlink(outside.root / "indices/credits.png", temp.root / "indices/credits.png", ec);
        if (ec) {
            std::cout << "skip: symlink creation unsupported: " << ec.message() << '\n';
            return;
        }
        error(
            [&] {
                static_cast<void>(AssetStore::load(temp.root / "manifest.txt"));
            },
            "path escapes asset root", "credits");
        fs::remove(temp.root / "indices/credits.png");
        fs::copy_file(outside.root / "indices/credits.png", temp.root / "saved.png");
        fs::create_symlink(temp.root / "saved.png", temp.root / "indices/credits.png");
        const auto store = AssetStore::load(temp.root / "manifest.txt");
        require(store.image("credits").rgba.size() == 6, "inside symlink failed");
    });
    run("manifest root independent of working directory", [&] {
        const auto absolute = fs::absolute(fixture / "manifest.txt");
        const auto before = fs::current_path();
        struct Restore {
            fs::path path;
            ~Restore() {
                std::error_code ec;
                fs::current_path(path, ec);
            }
        } restore{before};
        fs::current_path(fs::temp_directory_path());
        const auto store = AssetStore::load(absolute);
        require(store.image("credits").rgba.size() == 6, "working directory selected asset root");
    });
    run("partial failure releases SDL resources", [&] {
        Temporary temp(fixture);
        auto manifest = text(temp.root / "manifest.txt");
        replace(manifest, "audio/typo22.mp3 1196", "audio/typo22.mp3 1");
        write_text(temp.root / "manifest.txt", manifest);
        const auto allocations = SDL_GetNumAllocations();
        for (int repeat = 0; repeat < 40; ++repeat) {
            error(
                [&] {
                    static_cast<void>(AssetStore::load(temp.root / "manifest.txt"));
                },
                "byte count mismatch", "typo22");
            {
                const auto store = AssetStore::load(fixture / "manifest.txt");
                require(store.image("credits").rgba.size() == 6, "reload after failure");
            }
            require(SDL_GetNumAllocations() == allocations, "SDL resources leaked on partial failure");
        }
    });
    std::cout << "synthetic checks: " << checks << '\n';
}

void game_assets(const fs::path& root, const fs::path& fixture) {
    std::istringstream expected(text(fixture / "game-pixels.txt"));
    const auto manifest_hash = checksum(read(root / "manifest.txt"));
    std::vector<std::pair<fs::path, std::string>> original_files;
    for (const auto& entry : fs::recursive_directory_iterator(root)) {
        if (entry.is_regular_file()) {
            original_files.emplace_back(entry.path(), checksum(read(entry.path())));
        }
    }
    const auto store = AssetStore::load(root / "manifest.txt");
    require(store.image_ids().size() == 46 && store.audio_ids().size() == 19, "complete roster differs");
    std::size_t pixels{}, frames{}, lookups{};
    unsigned count{};
    require(bool(expected >> count) && count == store.image_ids().size(), "Expected image count differs");
    for (unsigned record = 0; record < count; ++record) {
        std::string id, rgba_hash, index_hash;
        int width{}, height{};
        std::size_t length{};
        require(bool(expected >> id >> width >> height >> rgba_hash >> index_hash >> length), "Missing expected image");
        const auto view = store.image(id);
        require(view.width == width && view.height == height, "game asset dimensions differ");
        require(checksum(rgba_bytes(view.rgba)) == rgba_hash, "game asset RGBA pixels differ: " + id);
        require(checksum(view.palette_indices) == index_hash, "game asset red indices differ: " + id);
        require(view.palette_indices.size() == length, "game asset index length differs");
        pixels += view.rgba.size();
        frames += view.frames.size();
        for (std::uint32_t index = 0; index < view.frames.size(); ++index) {
            require(store.frame({{id}, index}) == view.frames[index], "game asset frame lookup differs");
        }
    }
    require(bool(expected >> count), "Missing lookup count");
    for (unsigned record = 0; record < count; ++record) {
        std::string id, rgba_hash, red_hash;
        int width{}, height{};
        require(bool(expected >> id >> width >> height >> rgba_hash >> red_hash), "Missing lookup fixture");
        const auto view = store.lookup(id);
        require(view.width == width && view.height == height, "game asset lookup dimensions differ");
        require(checksum(rgba_bytes(view.rgba)) == rgba_hash && checksum(view.red) == red_hash, "game asset lookup pixels differ");
        ++lookups;
    }
    require(frames == 860 && lookups == 2, "game asset stored frames/lookups differ");
    require(store.image_metadata("leaf").active_frames == 19 && store.image("leaf").frames.size() == 20, "leaf extra frame was discarded");
    for (const auto& id : store.audio_ids()) {
        require(!store.audio(id.value).encoded.empty(), "game asset audio unavailable");
    }
    for (const auto& [path, expected_checksum] : original_files) {
        require(checksum(read(path)) == expected_checksum, "game asset file changed during loading");
    }
    require(checksum(read(root / "manifest.txt")) == manifest_hash, "game asset manifest changed during loading");
    std::cout << "game assets: 46 index atlases, " << pixels << " exact RGBA pixels and red indices, " << frames << " frames, " << lookups << " lookups, 19 MP3 files\n";
}

void packaged(const fs::path& root, const fs::path& directory) {
    {
        Temporary temp(root);
        fs::copy_file(directory / "images.pak", temp.root / "images.pak");
        fs::copy_file(directory / "audio.pak", temp.root / "audio.pak");
        auto bytes = read(temp.root / "audio.pak");
        bytes.back() ^= 1;
        write(temp.root / "audio.pak", bytes);
        try {
            static_cast<void>(AssetStore::load_package(temp.root));
            throw std::runtime_error("corrupt package accepted");
        } catch (const std::runtime_error& failure) {
            require(std::string_view(failure.what()).find("checksum mismatch") != std::string_view::npos, "corrupt package error differs");
        }
    }
    const auto baseline = AssetStore::load(root / "manifest.txt");
    auto store = AssetStore::load_package(directory);
    require(store.root() == fs::canonical(directory), "package root differs");
    const auto equal = [](const auto& left, const auto& right) {
        return std::ranges::equal(left, right);
    };
    require(equal(store.image_ids(), baseline.image_ids()) && equal(store.audio_ids(), baseline.audio_ids()), "package roster differs");
    std::size_t pixels{}, frames{}, audio_bytes{};
    for (const auto& id : baseline.image_ids()) {
        const auto actual = store.image(id.value);
        const auto expected = baseline.image(id.value);
        require(actual.id == expected.id && actual.width == expected.width && actual.height == expected.height && actual.row_stride == expected.row_stride,
            "package layout differs: " + id.value);
        require(equal(actual.rgba, expected.rgba), "package RGBA differs: " + id.value);
        require(equal(actual.palette_indices, expected.palette_indices), "package indices differ: " + id.value);
        require(equal(actual.frames, expected.frames) && actual.masks.empty(), "package frames differ: " + id.value);
        const auto a = store.image_metadata(id.value), b = baseline.image_metadata(id.value);
        require(a.id == b.id && a.palette == b.palette && a.active_frames == b.active_frames && a.stored_frames == b.stored_frames && a.color_key == b.color_key,
            "package metadata differs: " + id.value);
        require(equal(store.palette(a.palette.value), baseline.palette(b.palette.value)), "package palette differs: " + id.value);
        for (std::uint32_t i = 0; i < actual.frames.size(); ++i) {
            require(store.frame({id, i}) == baseline.frame({id, i}), "package frame lookup differs");
        }
        pixels += actual.rgba.size();
        frames += actual.frames.size();
    }
    require(equal(store.tabel1(), baseline.tabel1()) && equal(store.tabel2(), baseline.tabel2()), "package tables differ");
    for (const auto id : {"indices/ClutDtl.png", "indices/ClutTrns.png"}) {
        const auto a = store.lookup(id), b = baseline.lookup(id);
        require(
            a.id == b.id && a.width == b.width && a.height == b.height && a.row_stride == b.row_stride && equal(a.rgba, b.rgba) && equal(a.red, b.red), "package lookup differs");
    }
    for (const auto& id : baseline.audio_ids()) {
        const auto a = store.audio(id.value), b = baseline.audio(id.value);
        require(a.id == b.id && a.path == b.path && a.original_sample_frames == b.original_sample_frames && a.original_sample_rate == b.original_sample_rate
                    && a.decoded_48000hz_samples == b.decoded_48000hz_samples && a.source_duration_seconds == b.source_duration_seconds && equal(a.encoded, b.encoded),
            "package MP3 differs: " + id.value);
        audio_bytes += a.encoded.size();
    }
    const auto borrowed = store.image("credits");
    const auto encoded = store.audio("typo22").encoded;
    auto moved = std::move(store);
    require(moved.image("credits").rgba.data() == borrowed.rgba.data() && moved.audio("typo22").encoded.data() == encoded.data(), "package move invalidated views");
    std::cout << "package equivalent: " << baseline.image_ids().size() << " index atlases, " << pixels << " RGBA pixels and indices, " << frames
              << " frames, 3 palettes, 2 lookups, " << audio_bytes << " MP3 bytes\n";
}

void load_timings(const fs::path& root, const fs::path& package_dir, int repeats) {
    std::vector<double> directory, packaged;
    const auto measure = [](const auto& load) {
        const auto start = std::chrono::steady_clock::now();
        const auto store = load();
        require(store.image_ids().size() == 46 && store.audio_ids().size() == 19, "timed load roster differs");
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    };
    const auto disk_load = [&] {
        return AssetStore::load(root / "manifest.txt");
    };
    const auto package_load = [&] {
        return AssetStore::load_package(package_dir);
    };
    for (int i = 0; i < repeats; ++i) {
        if (i % 2 == 0) {
            directory.push_back(measure(disk_load));
            packaged.push_back(measure(package_load));
        } else {
            packaged.push_back(measure(package_load));
            directory.push_back(measure(disk_load));
        }
    }
    const auto report = [](std::string_view name, std::vector<double>& values) {
        std::ranges::sort(values);
        std::cout << "load timing: " << name << " repeats=" << values.size() << " median_ms=" << values[values.size() / 2] << " min_ms=" << values.front()
                  << " max_ms=" << values.back() << '\n';
    };
    report("directory", directory);
    report("package", packaged);
}

} // namespace

int main(int argc, char** argv) {
    try {
        fs::path fixture, assets, package_dir;
        int repeats{};
        for (int i = 1; i < argc; ++i) {
            const std::string_view argument(argv[i]);
            if (argument == "--fixtures" && i + 1 < argc) {
                fixture = argv[++i];
            } else if (argument == "--assets" && i + 1 < argc) {
                assets = argv[++i];
            } else if (argument == "--package" && i + 1 < argc) {
                package_dir = argv[++i];
            } else if (argument == "--load-timings" && i + 1 < argc) {
                repeats = std::stoi(argv[++i]);
            } else {
                throw std::runtime_error("usage: --fixtures <root> [--assets <root>] [--package <directory>] [--load-timings <count>]");
            }
        }
        require(!fixture.empty(), "--fixtures is required");
        require(package_dir.empty() || !assets.empty(), "--package requires --assets");
        require(repeats >= 0 && repeats <= 9 && (repeats == 0 || (!assets.empty() && !package_dir.empty())), "invalid load timing count or missing package");
        if (!package_dir.empty()) {
            packaged(assets, package_dir);
        } else if (assets.empty()) {
            synthetic(fixture);
        } else {
            game_assets(assets, fixture);
        }
        if (repeats) {
            load_timings(assets, package_dir, repeats);
        }
        require(SDL_WasInit(0) == 0, "asset loading initialized an SDL subsystem");
        return 0;
    } catch (const std::exception& failure) {
        std::cerr << "failure: " << failure.what() << '\n';
        return 1;
    }
}
