#include "render/renderer.hpp"

#include "render/lookup.hpp"

#include <algorithm>
#include <array>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace {

using namespace moorhuhn::contracts;
using namespace moorhuhn::render;
constexpr RgbaPixel black{0, 0, 0, 255};
constexpr RgbaPixel base{10, 20, 30, 255};
constexpr RgbaPixel red{200, 0, 0, 255};
constexpr RgbaPixel green{0, 200, 0, 128};
constexpr RgbaPixel hidden{0, 0, 200, 0};
constexpr RgbaPixel yellow{200, 200, 0, 255};
constexpr RgbaPixel cyan{0, 200, 200, 255};
constexpr RgbaPixel magenta{200, 0, 200, 255};

void require(bool result, const std::string& message) {
    if (!result) {
        throw std::runtime_error(message);
    }
}

template <typename Function> void rejected(Function function, const std::string& expected) {
    try {
        function();
    } catch (const std::exception& error) {
        require(std::string(error.what()).find(expected) != std::string::npos, error.what());
        return;
    }
    throw std::runtime_error("Expected rejection: " + expected);
}

struct Fixture {
    std::array<RgbaPixel, 21> pixels{};
    std::array<std::uint8_t, 21> indices{};
    std::array<AtlasRect, 1> frames{{{1, 1, 3, 2}}};
    std::array<std::uint8_t, 8> coverage{{255, 128, 0, 99, 99, 0, 255, 255}};
    std::array<MaskView, 1> masks{{{3, 2, 5, coverage}}};

    Fixture() {
        pixels.fill({71, 72, 73, 74});
        pixels[8] = red;
        pixels[9] = green;
        pixels[10] = hidden;
        pixels[15] = yellow;
        pixels[16] = cyan;
        pixels[17] = magenta;
        indices.fill(99);
        indices[8] = 10;
        indices[9] = 18;
        indices[10] = 30;
        indices[15] = 40;
        indices[16] = 50;
        indices[17] = 60;
    }

    AssetView view() const {
        return {AssetId{"sprite"}, 5, 3, 7, frames, pixels, indices, masks};
    }
};

SpriteDraw sprite(ScreenPoint destination, SpriteBlend blend = SpriteBlend::copy) {
    return {FrameId{AssetId{"sprite"}, 0}, destination, blend};
}

void exact(const Framebuffer& image, const std::vector<RgbaPixel>& expected, const char* name) {
    require(image.pixels().size() == expected.size(), "Framebuffer dimensions differ");
    for (std::size_t index = 0; index < expected.size(); ++index) {
        if (image.pixels()[index] != expected[index]) {
            throw std::runtime_error(std::string(name) + " pixel " + std::to_string(index % 640) + "," + std::to_string(index / 640) + " differs");
        }
    }
}

std::vector<RgbaPixel> canvas(RgbaPixel clear = black) {
    return std::vector<RgbaPixel>(640 * 480, clear);
}

void put(std::vector<RgbaPixel>& image, int x, int y, RgbaPixel value) {
    image[static_cast<std::size_t>(y) * 640 + x] = value;
}

void pixel_checks() {
    Fixture fixture;
    const std::array assets{fixture.view()};
    const auto original_pixels = fixture.pixels;
    const auto original_indices = fixture.indices;
    const auto original_mask = fixture.coverage;
    Renderer renderer;
    auto expected = canvas();
    put(expected, 0, 0, cyan);
    put(expected, 1, 0, magenta);
    exact(renderer.render(DrawList{{sprite({-1, -1})}}, assets), expected, "negative clip");
    expected = canvas();
    put(expected, 638, 478, red);
    put(expected, 639, 478, green);
    put(expected, 638, 479, yellow);
    put(expected, 639, 479, cyan);
    exact(renderer.render(DrawList{{sprite({638, 478})}}, assets), expected, "right bottom clip");

    expected = canvas(base);
    put(expected, 10, 20, red);
    put(expected, 11, 20, {5, 110, 15, 255});
    put(expected, 10, 21, yellow);
    put(expected, 11, 21, cyan);
    put(expected, 12, 21, magenta);
    const DrawList alpha_draws{{FillDraw{{0, 0}, 640, 480, base}, sprite({10, 20}, SpriteBlend::source_over)}};
    exact(renderer.render(alpha_draws, assets), expected, "alpha source over");
    auto snapshot = renderer.snapshot();

    auto key = sprite({10, 20});
    key.transparent_index = 18;
    expected = canvas(base);
    put(expected, 10, 20, red);
    put(expected, 12, 20, hidden);
    put(expected, 10, 21, yellow);
    put(expected, 11, 21, cyan);
    put(expected, 12, 21, magenta);
    exact(renderer.render(DrawList{{FillDraw{{0, 0}, 640, 480, base}, key}}, assets), expected, "explicit key");

    auto mask = sprite({10, 20});
    mask.mask = SpriteMask{0, MaskMode::index_key, 0};
    expected = canvas(base);
    put(expected, 10, 20, red);
    put(expected, 11, 20, green);
    put(expected, 11, 21, cyan);
    put(expected, 12, 21, magenta);
    exact(renderer.render(DrawList{{FillDraw{{0, 0}, 640, 480, base}, mask}}, assets), expected, "mask key");
    mask.blend = SpriteBlend::source_over;
    mask.mask->mode = MaskMode::coverage;
    expected = canvas(base);
    put(expected, 10, 20, red);
    put(expected, 11, 20, {7, 65, 22, 255});
    put(expected, 11, 21, cyan);
    put(expected, 12, 21, magenta);
    exact(renderer.render(DrawList{{FillDraw{{0, 0}, 640, 480, base}, mask}}, assets), expected, "mask coverage");
    mask.transparent_index = 10;
    put(expected, 10, 20, base);
    exact(renderer.render(DrawList{{FillDraw{{0, 0}, 640, 480, base}, mask}}, assets), expected, "combined mask key");

    expected = canvas();
    for (int y = 2; y < 4; ++y) {
        for (int x = 1; x < 4; ++x) {
            put(expected, x, y, red);
        }
    }
    put(expected, 2, 3, cyan);
    put(expected, 3, 3, cyan);
    put(expected, 2, 4, cyan);
    put(expected, 3, 4, cyan);
    exact(renderer.render(DrawList{{FillDraw{{1, 2}, 3, 2, red}, FillDraw{{2, 3}, 2, 2, cyan}}}, {}), expected, "painter order");
    expected = canvas();
    put(expected, 1, 2, {17, 23, 31, 42});
    exact(renderer.render(DrawList{{FillDraw{{1, 2}, 1, 1, {17, 23, 31, 42}}}}, {}), expected, "exact RGBA fill");
    expected = canvas(red);
    exact(renderer.render(DrawList{{FillDraw{{-10, -10}, std::numeric_limits<int>::max(), std::numeric_limits<int>::max(), red}}}, {}), expected, "large fill");
    exact(renderer.render(
              DrawList{{sprite({std::numeric_limits<int>::min(), 0}), sprite({std::numeric_limits<int>::max(), std::numeric_limits<int>::max()}), FillDraw{{0, 0}, 0, 480, red}}},
              assets),
        canvas(), "outside extremes");
    require(snapshot.at({11, 20}) == RgbaPixel{5, 110, 15, 255}, "Snapshot changed after another render");
    require(fixture.pixels == original_pixels && fixture.indices == original_indices && fixture.coverage == original_mask, "Renderer changed source data");
    require(renderer.framebuffer().bytes().size() == 640U * 480 * 4, "Byte buffer size differs");
    rejected(
        [&] {
            (void)renderer.framebuffer().at({640, 0});
        },
        "outside");
}

void source_transform_checks() {
    Fixture fixture;
    const std::array assets{fixture.view()};
    Renderer renderer;
    auto draw = sprite({10, 20});
    draw.source_rect = AtlasRect{1, 0, 2, 2};
    auto expected = canvas();
    put(expected, 10, 20, green);
    put(expected, 11, 20, hidden);
    put(expected, 10, 21, cyan);
    put(expected, 11, 21, magenta);
    exact(renderer.render(DrawList{{draw}}, assets), expected, "frame-relative source crop");

    draw.mirror_x = true;
    draw.mirror_y = true;
    expected = canvas();
    put(expected, 10, 20, magenta);
    put(expected, 11, 20, cyan);
    put(expected, 10, 21, hidden);
    put(expected, 11, 21, green);
    exact(renderer.render(DrawList{{draw}}, assets), expected, "crop mirrored on both axes");

    draw.transparent_index = 18;
    draw.mask = SpriteMask{0, MaskMode::index_key, 0};
    expected = canvas(base);
    put(expected, 10, 20, magenta);
    put(expected, 11, 20, cyan);
    exact(renderer.render(DrawList{{FillDraw{{0, 0}, 640, 480, base}, draw}}, assets), expected, "cropped mask and key use transformed frame coordinates");
    draw.destination = {-1, 0};
    expected = canvas(base);
    put(expected, 0, 0, cyan);
    exact(renderer.render(DrawList{{FillDraw{{0, 0}, 640, 480, base}, draw}}, assets), expected, "mirrored crop clipped at screen edge");
    for (const auto invalid : {AtlasRect{-1, 0, 1, 1}, AtlasRect{0, 0, 0, 1}, AtlasRect{2, 0, 2, 1}, AtlasRect{0, 1, 1, 2}, AtlasRect{std::numeric_limits<int>::max(), 0, 2, 1}}) {
        draw.source_rect = invalid;
        rejected(
            [&] {
                (void)renderer.render(DrawList{{draw}}, assets);
            },
            "outside the frame");
        exact(renderer.framebuffer(), expected, "invalid crop preserves prior frame");
    }
}

void copy_row_checks() {
    Fixture fixture;
    const std::array assets{fixture.view()};
    Renderer renderer;
    auto draw = sprite({-1, 5});
    draw.transparent_index = 18;
    auto expected = canvas(base);
    put(expected, 1, 5, hidden);
    put(expected, 0, 6, cyan);
    put(expected, 1, 6, magenta);
    exact(renderer.render(DrawList{{draw}}, assets, {base}), expected, "keyed padded rows clip left and copy zero alpha");

    draw.destination = {638, 479};
    draw.source_rect = AtlasRect{1, 0, 2, 2};
    expected = canvas(base);
    put(expected, 639, 479, hidden);
    exact(renderer.render(DrawList{{draw}}, assets, {base}), expected, "keyed crop clips bottom without changing rejected pixels");

    draw.transparent_index.reset();
    draw.destination = {-1, -1};
    expected = canvas(base);
    put(expected, 0, 0, magenta);
    exact(renderer.render(DrawList{{draw}}, assets, {base}), expected, "unkeyed crop clips both source axes with padded rows");
}

void region_checks() {
    Fixture fixture;
    const std::array assets{fixture.view()};
    Renderer renderer;
    const DrawList draws{{sprite({10, 20}), FillDraw{{11, 20}, 1, 1, yellow}}};
    const auto& full_frame = renderer.render(draws, assets);
    const std::vector<RgbaPixel> full(full_frame.pixels().begin(), full_frame.pixels().end());
    RenderOptions options{base};
    options.region = AtlasRect{10, 20, 2, 2};
    const auto& partial = renderer.render(draws, assets, options);
    auto expected = canvas(base);

    for (int y = 20; y < 22; ++y) {
        for (int x = 10; x < 12; ++x) {
            put(expected, x, y, full[static_cast<std::size_t>(y) * 640 + x]);
        }
    }

    exact(partial, expected, "region-limited sprite and fill drawing");
}

void failure_checks() {
    Fixture fixture;
    Renderer renderer;
    const std::array assets{fixture.view()};
    (void)renderer.render(DrawList{{FillDraw{{0, 0}, 640, 480, base}}}, {});
    auto bad = sprite({0, 0});
    bad.frame.image.value = "missing";
    rejected(
        [&] {
            (void)renderer.render(DrawList{{bad}}, assets);
        },
        "command 0 image 'missing'");
    bad = sprite({0, 0});
    bad.frame.index = 1;
    rejected(
        [&] {
            (void)renderer.render(DrawList{{bad}}, assets);
        },
        "Frame index");
    auto modified = assets;
    modified[0].row_stride = 4;
    rejected(
        [&] {
            (void)renderer.render(DrawList{{sprite({0, 0})}}, modified);
        },
        "row stride");
    modified = assets;
    modified[0].rgba = modified[0].rgba.first(5);
    rejected(
        [&] {
            (void)renderer.render(DrawList{{sprite({0, 0})}}, modified);
        },
        "truncated");
    modified = assets;
    modified[0].palette_indices = {};
    bad = sprite({0, 0});
    bad.transparent_index = 18;
    rejected(
        [&] {
            (void)renderer.render(DrawList{{bad}}, modified);
        },
        "Index storage");
    std::array<AtlasRect, 1> frames{{{std::numeric_limits<int>::max(), 0, 1, 1}}};
    modified = assets;
    modified[0].frames = frames;
    rejected(
        [&] {
            (void)renderer.render(DrawList{{sprite({0, 0})}}, modified);
        },
        "outside the atlas");
    bad = sprite({0, 0});
    bad.mask = SpriteMask{1};
    rejected(
        [&] {
            (void)renderer.render(DrawList{{bad}}, assets);
        },
        "Mask index");
    bad.mask->index = 0;
    auto masks = fixture.masks;
    masks[0].width = 4;
    modified = assets;
    modified[0].masks = masks;
    rejected(
        [&] {
            (void)renderer.render(DrawList{{bad}}, modified);
        },
        "Mask dimensions");
    masks = fixture.masks;
    masks[0].indices = masks[0].indices.first(5);
    rejected(
        [&] {
            (void)renderer.render(DrawList{{bad}}, modified);
        },
        "Mask dimensions");
    bad.mask->mode = static_cast<MaskMode>(99);
    rejected(
        [&] {
            (void)renderer.render(DrawList{{bad}}, assets);
        },
        "Unknown mask");
    bad = sprite({0, 0});
    bad.blend = static_cast<SpriteBlend>(99);
    rejected(
        [&] {
            (void)renderer.render(DrawList{{bad}}, assets);
        },
        "Unknown sprite blend");
    const std::array duplicates{assets[0], assets[0]};
    rejected(
        [&] {
            (void)renderer.render(DrawList{}, duplicates);
        },
        "Duplicate image");
    rejected(
        [&] {
            (void)renderer.render(DrawList{{FillDraw{{0, 0}, -1, 1, red}}}, {});
        },
        "cannot be negative");

    exact(renderer.framebuffer(), canvas(base), "failed draw preserves completed image");
}

void lookup_checks() {
    const std::array<std::uint8_t, 6> values{3, 2, 99, 1, 0, 99};
    const IndexTableView table{2, 2, 3, values};
    const std::array colors{red, green, cyan, magenta};
    require(palette_color(colors, lookup_index(table, 0, 0)) == magenta, "Lookup axes changed");
    require(palette_color(colors, lookup_index(table, 1, 0)) == cyan, "Lookup row changed");
    require(palette_color(colors, lookup_index(table, 0, 1)) == green, "Lookup stride changed");
    require(palette_color(colors, lookup_index(table, 1, 1)) == red, "Lookup index zero changed");
    rejected(
        [&] {
            (void)lookup_index(table, 2, 0);
        },
        "outside");
    rejected(
        [&] {
            (void)lookup_index({2, 2, 1, values}, 0, 0);
        },
        "stride");
    rejected(
        [&] {
            (void)lookup_index({2, 2, 3, std::span(values).first(3)}, 0, 0);
        },
        "truncated");
    rejected(
        [&] {
            (void)palette_color(colors, 4);
        },
        "unavailable");
    require(source_over({200, 40, 10, 128}, {20, 100, 220, 64}) == RgbaPixel{164, 52, 52, 160}, "Straight alpha or nearest rounding differs");
    require(source_over(hidden, base) == base && source_over(red, hidden) == red && source_over(green, hidden) == green, "Alpha endpoints differ");
}

} // namespace

int main() {
    try {
        pixel_checks();
        source_transform_checks();
        copy_row_checks();
        region_checks();
        failure_checks();
        lookup_checks();
        std::cout << "Pixel-exact clipping, painter order, alpha, keys, masks, lookups, immutability and failure checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
