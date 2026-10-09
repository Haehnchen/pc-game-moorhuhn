#include "assets/asset_store.hpp"

#include "assets/asset_package.hpp"

#include <SDL3/SDL.h>
#include <SDL3_mixer/SDL_mixer.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <utility>
#include <vector>

namespace moorhuhn::assets {
namespace {
using Bytes = std::vector<std::uint8_t>;
using contracts::RgbaPixel;

struct Context {
    std::string id;
    std::string path;

    [[noreturn]] void fail(std::string cause) const {
        throw AssetError(id, path, std::move(cause));
    }
};

struct ImageSpec {
    std::string_view id;
    std::uint32_t active;
    std::uint32_t stored;
};

// The runtime roster fixes IDs and frame counts. Active ranges may omit stored frames.
// clang-format off
constexpr std::array image_specs{
    ImageSpec{"balloon", 21, 21},
    ImageSpec{"balloon2", 20, 20},
    ImageSpec{"big1", 15, 15},
    ImageSpec{"bigfont", 135, 135},
    ImageSpec{"bighit", 17, 17},
    ImageSpec{"chickd", 19, 19},
    ImageSpec{"chickd2", 19, 19},
    ImageSpec{"chickd3", 19, 19},
    ImageSpec{"chickie", 19, 19},
    ImageSpec{"chickl", 19, 19},
    ImageSpec{"chickl2", 19, 19},
    ImageSpec{"chickl3", 19, 19},
    ImageSpec{"chickr", 19, 19},
    ImageSpec{"chickr2", 19, 19},
    ImageSpec{"chickr3", 19, 19},
    ImageSpec{"cursors", 3, 3},
    ImageSpec{"font3", 247, 247},
    ImageSpec{"guck", 14, 14},
    ImageSpec{"hat", 20, 20},
    ImageSpec{"highfly", 22, 22},
    ImageSpec{"highscor", 1, 1},
    ImageSpec{"hole", 1, 1},
    ImageSpec{"hole2", 1, 1},
    ImageSpec{"huls", 20, 20},
    ImageSpec{"layer0", 1, 1},
    ImageSpec{"layer1", 1, 1},
    ImageSpec{"layer2", 1, 1},
    ImageSpec{"layer3", 1, 1},
    ImageSpec{"layer4", 1, 1},
    ImageSpec{"layer5", 1, 1},
    ImageSpec{"leaf", 19, 20},
    ImageSpec{"leafhit", 25, 25},
    ImageSpec{"main", 1, 1},
    ImageSpec{"mask", 14, 14},
    ImageSpec{"mschiess", 1, 1},
    ImageSpec{"plane", 20, 20},
    ImageSpec{"scare", 1, 1},
    ImageSpec{"score", 1, 1},
    ImageSpec{"shield", 1, 1},
    ImageSpec{"sign", 1, 1},
    ImageSpec{"target", 1, 1},
    ImageSpec{"virtuell", 1, 1},
    ImageSpec{"windmill", 1, 1},
    ImageSpec{"wing", 37, 37},
    ImageSpec{"credits", 1, 1},
    ImageSpec{"credits2", 1, 1},
};
// clang-format on

constexpr std::array<std::string_view, 19> sound_ids{"shoot22", "reload22", "tree22", "bird22", "empty22", "hit22", "fall22", "shield22", "plane22", "plane222", "explo22",
    "balon22", "typo22", "intro22", "ready22", "timeup22", "over22", "hit222", "big22"};
constexpr std::array<std::string_view, 3> palette_ids{"moorhuhn.pal", "credits.pal", "credits2.pal"};

constexpr std::array<std::string_view, 2> lookup_ids{"indices/ClutDtl.png", "indices/ClutTrns.png"};

constexpr std::uint64_t u32_max = std::numeric_limits<std::uint32_t>::max();

// Versioned whitespace-separated records use fixed fields and explicit counts.
class MetadataReader {
public:
    MetadataReader(const Bytes& bytes, Context context) : text_(bytes.begin(), bytes.end()), context_(std::move(context)) {
        if (bytes.size() > 1'000'000) {
            context_.fail("oversized metadata");
        }
    }

    void context(Context value) {
        context_ = std::move(value);
    }

    std::string token() {
        const auto start = text_.find_first_not_of(" \t\r\n", position_);
        if (start == std::string::npos) {
            context_.fail("truncated metadata");
        }
        const auto end = text_.find_first_of(" \t\r\n", start);
        position_ = end == std::string::npos ? text_.size() : end;
        if (position_ - start > 240) {
            context_.fail("oversized metadata token");
        }
        return text_.substr(start, position_ - start);
    }

    void expect(std::string_view expected) {
        if (token() != expected) {
            context_.fail("expected " + std::string(expected));
        }
    }

    std::uint32_t integer(std::uint32_t low = 0, std::uint32_t high = UINT32_MAX) {
        const auto value = token();
        std::uint32_t result{};
        const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result);
        if (error != std::errc{} || end != value.data() + value.size() || result < low || result > high) {
            context_.fail("integer out of bounds");
        }
        return result;
    }

    double real() {
        const auto value = token();
        double result{};
        const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result);
        if (error != std::errc{} || end != value.data() + value.size() || !std::isfinite(result)) {
            context_.fail("invalid numeric source duration");
        }
        return result;
    }

    void finish() {
        expect("end");
        if (text_.find_first_not_of(" \t\r\n", position_) != std::string::npos) {
            context_.fail("unexpected metadata data");
        }
    }

private:
    std::string text_;
    Context context_;
    std::size_t position_{};
};

Bytes read_file(const std::filesystem::path& path, const Context& c) {
    std::error_code ec;

    if (!std::filesystem::is_regular_file(path, ec) || ec) {
        c.fail("missing or non-regular file");
    }

    const auto size = std::filesystem::file_size(path, ec);

    if (ec || size > u32_max) {
        c.fail("cannot determine file size or file exceeds uint32 length");
    }

    std::ifstream stream(path, std::ios::binary);

    if (!stream) {
        c.fail("cannot open file");
    }

    Bytes bytes(static_cast<std::size_t>(size));

    if (!bytes.empty()) {
        stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }

    if (!stream || stream.peek() != std::char_traits<char>::eof()) {
        c.fail("cannot read complete file");
    }

    return bytes;
}

bool safe_component(std::string_view value) {
    if (value.empty() || value.front() == '.' || value.back() == '.') {
        return false;
    }

    bool previous_dot = false;

    for (auto ch : value) {
        const bool dot = ch == '.';

        if (dot && previous_dot) {
            return false;
        }

        if (!dot && !(ch >= 'a' && ch <= 'z') && !(ch >= 'A' && ch <= 'Z') && !(ch >= '0' && ch <= '9') && ch != '_' && ch != '-') {
            return false;
        }

        previous_dot = dot;
    }

    auto base = std::string(value.substr(0, value.find('.')));

    for (auto& ch : base) {
        if (ch >= 'A' && ch <= 'Z') {
            ch = static_cast<char>(ch - 'A' + 'a');
        }
    }

    if (base == "con" || base == "prn" || base == "aux" || base == "nul") {
        return false;
    }

    if (base.size() == 4 && (base.starts_with("com") || base.starts_with("lpt")) && base[3] >= '1' && base[3] <= '9') {
        return false;
    }

    return true;
}

void relative_path(std::string_view relative, const Context& c) {
    if (relative.empty() || relative.size() > 240) {
        c.fail("unsafe relative path");
    }

    std::size_t start{};

    while (true) {
        const auto end = relative.find('/', start);

        if (!safe_component(relative.substr(start, end == std::string_view::npos ? end : end - start))) {
            c.fail("unsafe relative path");
        }

        if (end == std::string_view::npos) {
            break;
        }

        start = end + 1;
    }
}

std::filesystem::path resolve(const std::filesystem::path& root, std::string_view relative, const Context& c) {
    relative_path(relative, c);
    std::error_code ec;
    // Resolve symlinks before checking every component against the asset root.
    const auto target = std::filesystem::canonical(root / std::filesystem::path(relative), ec);

    if (ec) {
        c.fail("cannot resolve file: " + ec.message());
    }

    auto root_part = root.begin();
    auto target_part = target.begin();

    while (root_part != root.end() && target_part != target.end() && *root_part == *target_part) {
        ++root_part;
        ++target_part;
    }

    if (root_part != root.end()) {
        c.fail("path escapes asset root");
    }

    if (!std::filesystem::is_regular_file(target, ec) || ec) {
        c.fail("expected regular file");
    }

    return target;
}

struct Source {
    std::filesystem::path root;
    const detail::AssetPackage* images{};
    const detail::AssetPackage* audio{};
    mutable std::set<std::string> visited;

    [[nodiscard]] Bytes read(std::string_view path, const Context& c) const {
        if (!images) {
            return read_file(resolve(root, path, c), c);
        }

        relative_path(path, c);
        const auto* package = path.starts_with("audio/") ? audio : images;

        try {
            const auto bytes = package->read(path);
            visited.emplace(path);

            return Bytes(bytes.begin(), bytes.end());
        } catch (const std::runtime_error& error) {
            c.fail(error.what());
        }
    }

    void validate_complete() const {
        if (!images) {
            return;
        }

        if (visited.size() != images->entries().size() + audio->entries().size()) {
            throw AssetError("packages", root.string(), "unlisted asset package entries");
        }
    }
};

Bytes checked_file(const Source& source, std::string_view path, std::uint32_t size, std::string_view expected, const std::string& id) {
    const Context actual{id, std::string(path)};
    const auto bytes = source.read(path, actual);
    if (path != expected) {
        actual.fail("expected role path " + std::string(expected));
    }
    if (bytes.size() != size) {
        actual.fail("byte count mismatch");
    }
    return bytes;
}

struct DecodedImage {
    std::int32_t width{};
    std::int32_t height{};
    std::vector<RgbaPixel> rgba;
    Bytes red;
};

DecodedImage decode(const Bytes& bytes, const Context& c, bool with_rgba = true) {
    using Io = std::unique_ptr<SDL_IOStream, decltype(&SDL_CloseIO)>;
    using Surface = std::unique_ptr<SDL_Surface, decltype(&SDL_DestroySurface)>;
    Io io(SDL_IOFromConstMem(bytes.data(), bytes.size()), SDL_CloseIO);

    if (!io) {
        c.fail("SDL input allocation failed: " + std::string(SDL_GetError()));
    }

    Surface source(SDL_LoadPNG_IO(io.get(), false), SDL_DestroySurface);

    if (!source) {
        c.fail("SDL PNG decode failed: " + std::string(SDL_GetError()));
    }

    const auto width = source->w;
    const auto height = source->h;

    Surface rgba(SDL_ConvertSurface(source.get(), SDL_PIXELFORMAT_RGBA32), SDL_DestroySurface);

    if (!rgba) {
        c.fail("SDL RGBA conversion failed: " + std::string(SDL_GetError()));
    }

    DecodedImage result{width, height, {}, {}};
    const auto count = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);

    if (with_rgba) {
        result.rgba.resize(count);
    }

    result.red.resize(count);

    if (!SDL_LockSurface(rgba.get())) {
        c.fail("SDL surface lock failed: " + std::string(SDL_GetError()));
    }

    const auto* pixels = static_cast<const std::uint8_t*>(rgba->pixels);

    for (std::int32_t y = 0; y < height; ++y) {
        const auto* row = pixels + static_cast<std::size_t>(y) * static_cast<std::size_t>(rgba->pitch);

        for (std::int32_t x = 0; x < width; ++x) {
            const auto pos = static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x);
            const auto p = static_cast<std::size_t>(x) * 4;

            if (with_rgba) {
                result.rgba[pos] = {row[p], row[p + 1], row[p + 2], row[p + 3]};
            }

            result.red[pos] = row[p];
        }
    }

    SDL_UnlockSurface(rgba.get());

    return result;
}

std::uint64_t audio_samples(const Bytes& bytes, const Context& c) {
    if (!MIX_Init()) {
        c.fail("SDL_mixer initialization failed: " + std::string(SDL_GetError()));
    }

    struct Library {
        ~Library() {
            MIX_Quit();
        }
    } library;

    using Io = std::unique_ptr<SDL_IOStream, decltype(&SDL_CloseIO)>;
    using Audio = std::unique_ptr<MIX_Audio, decltype(&MIX_DestroyAudio)>;
    Io io(SDL_IOFromConstMem(bytes.data(), bytes.size()), SDL_CloseIO);

    if (!io) {
        c.fail("SDL input allocation failed: " + std::string(SDL_GetError()));
    }

    Audio audio(MIX_LoadAudio_IO(nullptr, io.get(), true, false), MIX_DestroyAudio);
    SDL_AudioSpec format{};

    if (!audio || !MIX_GetAudioFormat(audio.get(), &format)) {
        c.fail("SDL MP3 decode failed: " + std::string(SDL_GetError()));
    }

    const auto properties = MIX_GetAudioProperties(audio.get());
    const std::string_view decoder = SDL_GetStringProperty(properties, MIX_PROP_AUDIO_DECODER_STRING, "");
    const auto length = MIX_GetAudioDuration(audio.get());

    if (decoder != "DRMP3" || format.freq != 48000 || format.channels != 2 || length <= 0 || static_cast<std::uint64_t>(length) > u32_max) {
        c.fail("expected stereo 48000 Hz MP3");
    }

    return static_cast<std::uint64_t>(length);
}

} // namespace

struct AssetStore::Storage {
    struct Image {
        ImageMetadata metadata;
        DecodedImage pixels;
        Bytes indices;
        std::vector<contracts::AtlasRect> frames;
    };

    struct Audio {
        AudioView metadata;
        Bytes encoded;
    };

    std::filesystem::path root;
    std::map<std::string, Image, std::less<>> images;
    std::map<std::string, Audio, std::less<>> sounds;
    std::map<std::string, std::vector<PaletteEntry>, std::less<>> palettes;
    std::map<std::string, DecodedImage, std::less<>> lookups;
    std::array<std::uint32_t, 600> tabel1{};
    std::array<std::uint32_t, 600> tabel2{};
    std::vector<contracts::AssetId> image_ids;
    std::vector<contracts::AssetId> audio_ids;
};

AssetError::AssetError(std::string id, std::string path, std::string cause)
    : std::runtime_error("asset '" + id + "', path '" + path + "': " + cause), id_(std::move(id)), path_(std::move(path)), cause_(std::move(cause)) {}

AssetStore::AssetStore(std::unique_ptr<Storage> storage) : storage_(std::move(storage)) {}

AssetStore::~AssetStore() = default;
AssetStore::AssetStore(AssetStore&&) noexcept = default;
AssetStore& AssetStore::operator=(AssetStore&&) noexcept = default;

AssetStore AssetStore::load(const std::filesystem::path& manifest) {
    return load_impl(manifest, false);
}

AssetStore AssetStore::load_package(const std::filesystem::path& directory) {
    return load_impl(directory, true);
}

AssetStore AssetStore::load_impl(const std::filesystem::path& path, bool packaged) {
    const Context mc{"manifest", packaged ? (path / "images.pak").string() : path.string()};
    auto result = std::make_unique<Storage>();
    Source source{};
    std::optional<detail::AssetPackage> images_package;
    std::optional<detail::AssetPackage> audio_package;
    Bytes manifest_bytes;

    // Directory and package loading feed the same manifest validation below.
    if (packaged) {
        std::error_code ec;
        source.root = std::filesystem::canonical(path, ec);

        if (ec || !std::filesystem::is_directory(source.root)) {
            mc.fail("cannot resolve package directory");
        }

        images_package.emplace(resolve(source.root, "images.pak", {"images.pak", "images.pak"}));
        audio_package.emplace(resolve(source.root, "audio.pak", {"audio.pak", "audio.pak"}));
        source.images = &*images_package;
        source.audio = &*audio_package;
        manifest_bytes = source.read("manifest.txt", mc);
    } else {
        std::error_code ec;
        const auto absolute = std::filesystem::canonical(path, ec);

        if (ec) {
            mc.fail("cannot resolve manifest: " + ec.message());
        }

        source.root = absolute.parent_path();
        manifest_bytes = read_file(absolute, mc);
    }

    result->root = source.root;
    MetadataReader document(manifest_bytes, mc);
    document.expect("MHMANIFEST");
    document.expect("1");
    document.expect("tables");
    const auto tables_path = document.token();
    if (tables_path != "tables.txt") {
        mc.fail("expected role path tables.txt");
    }
    const Context tc{"tables", tables_path};
    MetadataReader tables(source.read(tables_path, tc), tc);
    tables.expect("MHTABLES");
    tables.expect("1");
    tables.expect("palettes");
    const auto palette_count = tables.integer(1, palette_ids.size());
    for (std::uint32_t i = 0; i < palette_count; ++i) {
        tables.expect("palette");
        const auto id = tables.token();
        const Context pc{id, "tables.txt.palettes." + id};
        tables.context(pc);
        if (std::find(palette_ids.begin(), palette_ids.end(), id) == palette_ids.end()) {
            pc.fail("unknown palette ID");
        }
        if (result->palettes.contains(id)) {
            pc.fail("duplicate palette ID");
        }
        auto& entries = result->palettes[id];
        entries.resize(256);
        for (auto& entry : entries) {
            for (auto& channel : entry) {
                channel = static_cast<std::uint8_t>(tables.integer(0, 255));
            }
        }
    }
    tables.context(tc);
    for (auto name : {"tabel1", "tabel2"}) {
        tables.expect(name);
        auto& target = std::string_view(name) == "tabel1" ? result->tabel1 : result->tabel2;
        for (auto& value : target) {
            value = tables.integer();
        }
    }
    tables.finish();
    document.expect("images");
    const auto image_count = document.integer(1, image_specs.size());

    // Decode indexed atlases, then build their RGBA pixels from the palette.
    for (std::uint32_t record = 0; record < image_count; ++record) {
        document.context(mc);
        document.expect("image");
        const auto id = document.token();
        const Context c{id, "manifest.images." + id};
        document.context(c);
        const auto spec = std::find_if(image_specs.begin(), image_specs.end(), [&](const auto& item) {
            return item.id == id;
        });

        if (spec == image_specs.end()) {
            c.fail("unknown image ID");
        }

        if (result->images.contains(id)) {
            c.fail("duplicate image ID");
        }

        Storage::Image image;
        image.metadata.id = {id};
        image.metadata.palette = {document.token()};

        if (!result->palettes.contains(image.metadata.palette.value)) {
            c.fail("unknown or missing palette ID " + image.metadata.palette.value);
        }

        image.metadata.active_frames = spec->active;
        image.metadata.stored_frames = spec->stored;
        const auto key = document.token();
        if (key != "none") {
            unsigned value{};
            const auto [end, error] = std::from_chars(key.data(), key.data() + key.size(), value);
            if (error != std::errc{} || end != key.data() + key.size() || value > 255) {
                c.fail("color key out of bounds");
            }
            image.metadata.color_key = static_cast<std::uint8_t>(value);
        }
        const auto path = document.token();
        const auto size = document.integer(1);
        const auto frame_count = document.integer(image.metadata.stored_frames, image.metadata.stored_frames);
        const auto index_path = "indices/" + id + ".png";
        auto index_bytes = checked_file(source, path, size, index_path, id);
        auto indices = decode(index_bytes, {id, index_path}, false);
        image.pixels.width = indices.width;
        image.pixels.height = indices.height;
        image.indices = std::move(indices.red);
        for (std::uint32_t i = 0; i < frame_count; ++i) {
            const Context fc{id, index_path + ".frames[" + std::to_string(i) + "]"};
            document.context(fc);
            document.expect("frame");
            const auto x = document.integer();
            const auto y = document.integer();
            const auto width = document.integer(1, 16384);
            const auto height = document.integer(1, 16384);
            if (static_cast<std::uint64_t>(x) + width > static_cast<std::uint64_t>(image.pixels.width)
                || static_cast<std::uint64_t>(y) + height > static_cast<std::uint64_t>(image.pixels.height)) {
                fc.fail("frame rectangle exceeds atlas");
            }
            image.frames.push_back({static_cast<std::int32_t>(x), static_cast<std::int32_t>(y), static_cast<std::int32_t>(width), static_cast<std::int32_t>(height)});
        }

        // Color is fully determined by the palette and indexed frame coverage.
        const auto& palette = result->palettes.at(image.metadata.palette.value);
        image.pixels.rgba.resize(image.indices.size());

        for (std::size_t pixel = 0; pixel < image.indices.size(); ++pixel) {
            const auto& entry = palette[image.indices[pixel]];
            image.pixels.rgba[pixel] = {entry[0], entry[1], entry[2], 0};
        }

        for (const auto& frame : image.frames) {
            for (std::int32_t y = frame.y; y < frame.y + frame.height; ++y) {
                for (std::int32_t x = frame.x; x < frame.x + frame.width; ++x) {
                    const auto pixel = static_cast<std::size_t>(y) * static_cast<std::size_t>(image.pixels.width) + static_cast<std::size_t>(x);
                    const auto index = image.indices[pixel];
                    image.pixels.rgba[pixel].a = image.metadata.color_key && index == *image.metadata.color_key ? 0 : 255;
                }
            }
        }

        result->images.emplace(id, std::move(image));
        result->image_ids.push_back({id});
    }

    // Validate MP3 metadata and keep the file bytes for the audio engine.
    document.context(mc);
    document.expect("audio");
    const auto audio_count = document.integer(1, sound_ids.size());
    for (std::uint32_t record = 0; record < audio_count; ++record) {
        document.expect("sound");
        const auto id = document.token();
        const auto path = document.token();
        const Context c{id, path};
        document.context(c);
        if (std::find(sound_ids.begin(), sound_ids.end(), id) == sound_ids.end()) {
            c.fail("unknown sound ID");
        }

        if (result->sounds.contains(id)) {
            c.fail("duplicate sound ID");
        }

        Storage::Audio sound;
        sound.metadata.id = {id};
        sound.metadata.path = path;
        const auto size = document.integer(1);
        sound.metadata.original_sample_frames = document.integer(1);
        sound.metadata.original_sample_rate = document.integer(1, 192000);
        sound.metadata.decoded_48000hz_samples = document.integer(1);
        const auto codec = document.token();
        const auto channels = document.integer();
        if (codec != "mp3" || channels != 2) {
            c.fail("expected stereo MP3");
        }
        sound.metadata.source_duration_seconds = document.real();
        const auto expected_duration = static_cast<double>(sound.metadata.original_sample_frames) / sound.metadata.original_sample_rate;

        if (!std::isfinite(sound.metadata.source_duration_seconds) || sound.metadata.source_duration_seconds <= 0 || sound.metadata.source_duration_seconds > u32_max
            || std::abs(sound.metadata.source_duration_seconds - expected_duration) > 1e-9) {
            c.fail("source duration disagrees with original sample length");
        }

        sound.encoded = checked_file(source, path, size, "audio/" + id + ".mp3", id);
        const auto samples = audio_samples(sound.encoded, c);

        if (samples != sound.metadata.decoded_48000hz_samples) {
            c.fail("MP3 sample length mismatch");
        }

        const auto expected = std::llround(static_cast<double>(sound.metadata.original_sample_frames) * 48000 / sound.metadata.original_sample_rate);

        // MP3 can retain less than one frame of encoder padding at the end.
        if (static_cast<std::int64_t>(samples) < expected - 1 || static_cast<std::int64_t>(samples) - expected >= 1152) {
            c.fail("decoded length exceeds MP3 padding allowance");
        }

        result->sounds.emplace(id, std::move(sound));
        result->audio_ids.push_back({id});
    }

    document.context(mc);
    document.expect("lookups");
    const auto lookup_count = document.integer(0, lookup_ids.size());
    for (std::uint32_t record = 0; record < lookup_count; ++record) {
        document.expect("lookup");
        const auto id = document.token();
        const Context c{id, id};
        document.context(c);
        const auto size = document.integer(1);
        const auto position = std::find(lookup_ids.begin(), lookup_ids.end(), id);

        if (position == lookup_ids.end()) {
            c.fail("unknown lookup ID");
        }

        if (result->lookups.contains(id)) {
            c.fail("duplicate lookup ID");
        }

        const auto bytes = checked_file(source, id, size, id, id);
        auto pixels = decode(bytes, c);

        if (pixels.width != 256 || pixels.height != (position == lookup_ids.begin() ? 256 : 1)) {
            c.fail("lookup dimensions mismatch");
        }

        result->lookups.emplace(id, std::move(pixels));
    }

    document.finish();

    if (packaged
        && (result->images.size() != image_specs.size() || result->sounds.size() != sound_ids.size() || result->palettes.size() != palette_ids.size()
            || result->lookups.size() != lookup_ids.size())) {
        mc.fail("incomplete runtime roster");
    }

    source.validate_complete();

    return AssetStore(std::move(result));
}

const AssetStore::Storage& AssetStore::storage() const {
    if (!storage_) {
        throw AssetError("store", "", "store has been moved");
    }

    return *storage_;
}

contracts::AssetView AssetStore::image(std::string_view id) const {
    const auto& s = storage();
    const auto found = s.images.find(id);

    if (found == s.images.end()) {
        throw AssetError(std::string(id), "indices/" + std::string(id) + ".png", "unknown image ID");
    }

    const auto& image = found->second;

    return {{std::string(id)}, image.pixels.width, image.pixels.height, static_cast<std::uint32_t>(image.pixels.width), image.frames, image.pixels.rgba, image.indices, {}};
}

ImageMetadata AssetStore::image_metadata(std::string_view id) const {
    static_cast<void>(image(id));

    return storage().images.find(id)->second.metadata;
}

contracts::AtlasRect AssetStore::frame(const contracts::FrameId& id) const {
    const auto view = image(id.image.value);

    if (id.index >= view.frames.size()) {
        throw AssetError(id.image.value, "indices/" + id.image.value + ".png", "unknown frame index " + std::to_string(id.index));
    }

    return view.frames[id.index];
}

AudioView AssetStore::audio(std::string_view id) const {
    const auto& s = storage();
    const auto found = s.sounds.find(id);

    if (found == s.sounds.end()) {
        throw AssetError(std::string(id), "audio/" + std::string(id) + ".mp3", "unknown sound ID");
    }

    auto view = found->second.metadata;
    view.encoded = found->second.encoded;

    return view;
}

std::span<const PaletteEntry> AssetStore::palette(std::string_view id) const {
    const auto& s = storage();
    const auto found = s.palettes.find(id);

    if (found == s.palettes.end()) {
        throw AssetError(std::string(id), "tables.txt", "unknown palette ID");
    }

    return found->second;
}

LookupView AssetStore::lookup(std::string_view id) const {
    const auto& s = storage();
    const auto found = s.lookups.find(id);

    if (found == s.lookups.end()) {
        throw AssetError(std::string(id), std::string(id), "unknown lookup ID");
    }

    const auto& image = found->second;

    return {std::string(id), image.width, image.height, static_cast<std::uint32_t>(image.width), image.rgba, image.red};
}

std::span<const std::uint32_t> AssetStore::tabel1() const {
    return storage().tabel1;
}

std::span<const std::uint32_t> AssetStore::tabel2() const {
    return storage().tabel2;
}

std::span<const contracts::AssetId> AssetStore::image_ids() const {
    return storage().image_ids;
}

std::span<const contracts::AssetId> AssetStore::audio_ids() const {
    return storage().audio_ids;
}

const std::filesystem::path& AssetStore::root() const {
    return storage().root;
}

} // namespace moorhuhn::assets
