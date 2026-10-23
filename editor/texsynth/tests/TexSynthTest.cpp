// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// jasperengine-texsynth-tests: the texture synthesis core (editor/texsynth) on generated images.

#include "texsynth/SynthesisRecipe.h"
#include "texsynth/Synthesizer.h"
#include "texsynth/TextureSynthesis.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <thread>

using namespace jasperengine::editor::texsynth;
namespace fs = std::filesystem;

namespace {

    // Smooth value noise (a few octaves), RGB(A) 8-bit: a texture with structure at several
    // scales, not periodic (its edges don't match)
    Image noiseImage(int w, int h, uint32_t seed, int channels = 3, int bitDepth = 8) {
        Image image = Image::make(w, h, channels, bitDepth);
        std::mt19937 rng(seed);
        std::uniform_real_distribution<float> uni(0.0f, 1.0f);
        const int grid = 9;
        std::vector<float> lattice(static_cast<size_t>(grid) * grid * 4 * 3);
        for (float& v : lattice) v = uni(rng);
        auto value = [&](float x, float y, int octave, int c) {
            const int gx = static_cast<int>(x), gy = static_cast<int>(y);
            const float fx = x - gx, fy = y - gy;
            auto at = [&](int ix, int iy) {
                return lattice[((static_cast<size_t>(octave) * grid + (iy % grid)) * grid + (ix % grid)) * 3 + c];
            };
            const float a = at(gx, gy) + (at(gx + 1, gy) - at(gx, gy)) * fx;
            const float b = at(gx, gy + 1) + (at(gx + 1, gy + 1) - at(gx, gy + 1)) * fx;
            return a + (b - a) * fy;
        };
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                float rgb[3];
                for (int c = 0; c < 3; c++) {
                    float v = 0.0f, amp = 0.5f;
                    for (int o = 0; o < 4; o++) {
                        const float scale = static_cast<float>(1 << o) * 3.0f / static_cast<float>(std::max(w, h));
                        v += amp * value(x * scale, y * scale, o, c);
                        amp *= 0.5f;
                    }
                    rgb[c] = std::clamp(v * 1.07f, 0.0f, 1.0f);
                }
                uint8_t* p = image.pixel(x, y);
                for (int c = 0; c < channels; c++) {
                    const float v = c < 3 ? rgb[std::min(c, 2)] : 1.0f;
                    if (bitDepth == 16) {
                        const uint16_t s = static_cast<uint16_t>(std::lround(v * 65535.0f));
                        std::memcpy(p + 2 * c, &s, 2);
                    } else {
                        p[c] = static_cast<uint8_t>(std::lround(v * 255.0f));
                    }
                }
            }
        }
        return image;
    }

    // A unique value per pixel (16-bit grey): proves texel-exact copies
    Image indexImage16(int w, int h) {
        Image image = Image::make(w, h, 1, 16);
        for (int i = 0; i < w * h; i++) {
            const uint16_t v = static_cast<uint16_t>(i * 7 + 3);
            std::memcpy(image.data.data() + 2 * i, &v, 2);
        }
        return image;
    }

    Request basicRequest(Mode mode, int w, int h, int exampleSize = 40) {
        Request request;
        request.mode = mode;
        request.width = w;
        request.height = h;
        request.quality = Quality::Draft;
        request.examples.push_back(Example::fromImage(noiseImage(exampleSize, exampleSize, 7)));
        return request;
    }

    fs::path tempDir() {
        const fs::path dir = fs::temp_directory_path() / ("jasper-texsynth-" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()) + "-" +
                                                          ::testing::UnitTest::GetInstance()->current_test_info()->name());
        fs::create_directories(dir);
        return dir;
    }

    std::vector<uint8_t> readFile(const fs::path& path) {
        std::ifstream file(path, std::ios::binary);
        return std::vector<uint8_t>((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    }

    void writeFile(const fs::path& path, const std::vector<uint8_t>& bytes) {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }

}

// ---- k-NN

TEST(TexSynthKnn, GridMatchesBruteForce) {
    struct Case {
        int w, h, cell;
        bool tiling;
        double density;
    };
    const Case cases[] = {{37, 23, 4, false, 0.02}, {37, 23, 4, true, 0.02}, {64, 64, 4, true, 0.6}, {50, 41, 32, false, 0.3},
                          {50, 41, 32, true, 0.05}, {18, 10, 4, true, 1.0}, {9, 7, 4, true, 0.5},   {200, 3, 4, true, 0.2},
                          {61, 45, 4, false, 0.9},  {61, 45, 4, true, 0.4},  {90, 90, 32, false, 0.7}};
    std::mt19937 rng(11);
    for (const Case& c : cases) {
        detail::KnnGrid grid;
        grid.init(c.w, c.h, c.tiling, c.cell);
        std::vector<std::pair<int, int>> points;
        std::uniform_real_distribution<double> uni(0.0, 1.0);
        for (int y = 0; y < c.h; y++) {
            for (int x = 0; x < c.w; x++) {
                if (uni(rng) < c.density) {
                    grid.insert(x, y);
                    points.push_back({x, y});
                }
            }
        }
        std::vector<uint8_t> present(static_cast<size_t>(c.w) * c.h, 0);
        for (auto [x, y] : points) present[static_cast<size_t>(y) * c.w + x] = 1;
        const detail::DenseKnn dense(12);
        std::vector<uint64_t> keys;
        std::vector<detail::KnnGrid::Neighbor> got(64), spiral(64);
        for (int q = 0; q < 300; q++) {
            const int qx = static_cast<int>(rng() % c.w), qy = static_cast<int>(rng() % c.h);
            const int k = 1 + static_cast<int>(rng() % 40);
            std::vector<uint64_t> expected;
            for (auto [x, y] : points) {
                int dx = x - qx, dy = y - qy;
                if (c.tiling) {
                    if (dx < -(c.w / 2)) dx += c.w;
                    else if (dx >= c.w - c.w / 2) dx -= c.w;
                    if (dy < -(c.h / 2)) dy += c.h;
                    else if (dy >= c.h - c.h / 2) dy -= c.h;
                }
                expected.push_back((static_cast<uint64_t>(dx * dx + dy * dy) << 32) | (static_cast<uint64_t>(dy + 32768) << 16) |
                                   static_cast<uint64_t>(dx + 32768));
            }
            std::sort(expected.begin(), expected.end());
            expected.resize(std::min<size_t>(expected.size(), static_cast<size_t>(k)));
            const int n = grid.query(qx, qy, k, keys, got.data());
            ASSERT_EQ(n, static_cast<int>(expected.size())) << c.w << "x" << c.h << " cell " << c.cell << " tiling " << c.tiling;
            for (int i = 0; i < n; i++) {
                const uint64_t key = (static_cast<uint64_t>(got[i].d2) << 32) | (static_cast<uint64_t>(got[i].dy + 32768) << 16) |
                                     static_cast<uint64_t>(got[i].dx + 32768);
                ASSERT_EQ(key, expected[i]) << c.w << "x" << c.h << " cell " << c.cell << " tiling " << c.tiling << " query " << qx << "," << qy
                                            << " k " << k << " i " << i;
                ASSERT_EQ(got[i].d2, got[i].dx * got[i].dx + got[i].dy * got[i].dy);
            }
            // the dense form agrees whenever its disc holds k points
            const int m = dense.query(present.data(), c.w, c.h, c.tiling, qx, qy, k, spiral.data());
            if (m >= 0) {
                ASSERT_EQ(m, n);
                for (int i = 0; i < n; i++) {
                    ASSERT_EQ(spiral[i].dx, got[i].dx) << c.w << "x" << c.h << " tiling " << c.tiling << " query " << qx << "," << qy << " i " << i;
                    ASSERT_EQ(spiral[i].dy, got[i].dy);
                }
            }
        }
    }
}

TEST(TexSynthKnn, StagePixelCounts) {
    Params p;
    p.backtrackPercent = 0.5f;
    p.backtrackStages = 3;
    const std::vector<int64_t> counts = detail::stagePixelCounts(1000, p);
    ASSERT_EQ(counts.size(), 4u);
    EXPECT_EQ(counts[0], 125);
    EXPECT_EQ(counts[1], 250);
    EXPECT_EQ(counts[2], 500);
    EXPECT_EQ(counts[3], 1000);
    p.backtrackStages = 0;
    EXPECT_EQ(detail::stagePixelCounts(77, p), std::vector<int64_t>({77}));
}

// ---- determinism

TEST(TexSynth, IdenticalBytesWhateverTheThreadCount) {
    Request one = basicRequest(Mode::Tileable, 96, 80);
    one.quality = Quality::Balanced;
    one.seed = 1234;
    one.threads = 1;
    Request eight = one;
    eight.threads = 8;
    Request three = one;
    three.threads = 3;
    const Result a = synthesize(one);
    const Result b = synthesize(eight);
    const Result c = synthesize(three);
    ASSERT_EQ(a.status, Status::Ok);
    ASSERT_EQ(b.status, Status::Ok);
    ASSERT_EQ(c.status, Status::Ok);
    EXPECT_EQ(b.threads, 8);
    EXPECT_TRUE(a.primary.data == b.primary.data);
    EXPECT_TRUE(a.primary.data == c.primary.data);
    EXPECT_TRUE(a.uncertainty == b.uncertainty);
    ASSERT_EQ(a.coordinates.coords.size(), b.coordinates.coords.size());
    for (size_t i = 0; i < a.coordinates.coords.size(); i++) {
        ASSERT_EQ(a.coordinates.coords[i].x, b.coordinates.coords[i].x);
        ASSERT_EQ(a.coordinates.coords[i].y, b.coordinates.coords[i].y);
    }
    std::vector<uint8_t> pngA, pngB;
    ASSERT_TRUE(encodePng(a.primary, pngA));
    ASSERT_TRUE(encodePng(b.primary, pngB));
    EXPECT_TRUE(pngA == pngB);

    // another seed, another texture
    Request other = one;
    other.seed = 99;
    const Result d = synthesize(other);
    EXPECT_FALSE(a.primary.data == d.primary.data);
}

TEST(TexSynth, DeterministicAcrossThreadsInEveryMode) {
    for (Mode mode : {Mode::Extend, Mode::MakeTileable, Mode::Inpaint, Mode::Guided}) {
        Request r = basicRequest(mode, mode == Mode::Extend ? 64 : 0, mode == Mode::Extend ? 48 : 0, 48);
        if (mode == Mode::Inpaint) r.inpaintMask.image = ImageInput::fromImage([] {
            Image m = Image::make(48, 48, 1, 8);
            for (int y = 10; y < 30; y++) for (int x = 12; x < 40; x++) m.data[y * 48 + x] = 255;
            return m;
        }());
        if (mode == Mode::Guided) r.targetGuide = ImageInput::fromImage(noiseImage(56, 40, 3));
        r.threads = 1;
        Request r8 = r;
        r8.threads = 8;
        const Result a = synthesize(r), b = synthesize(r8);
        ASSERT_EQ(a.status, Status::Ok) << toString(mode) << (a.errors.empty() ? "" : a.errors[0].message);
        ASSERT_EQ(b.status, Status::Ok) << toString(mode);
        EXPECT_TRUE(a.primary.data == b.primary.data) << toString(mode);
    }
}

// ---- modes

TEST(TexSynth, ModesProduceTheRightSizes) {
    {
        Request r = basicRequest(Mode::Extend, 70, 50);
        const Result result = synthesize(r);
        ASSERT_EQ(result.status, Status::Ok);
        EXPECT_EQ(result.primary.width, 70);
        EXPECT_EQ(result.primary.height, 50);
        EXPECT_EQ(result.primary.channels, 3);   // the example's format
        EXPECT_EQ(result.uncertaintyMap.width, 70);
        EXPECT_EQ(result.uncertainty.size(), 70u * 50u);
    }
    {
        Request r = basicRequest(Mode::MakeTileable, 0, 0, 52);
        const Result result = synthesize(r);
        ASSERT_EQ(result.status, Status::Ok);
        EXPECT_EQ(result.primary.width, 52);
        EXPECT_EQ(result.primary.height, 52);
    }
    {
        Request r = basicRequest(Mode::MakeTileable, 64, 64, 52);   // a size that is not the image's
        const Result result = synthesize(r);
        EXPECT_EQ(result.status, Status::InvalidRequest);
    }
    {
        Request r = basicRequest(Mode::Guided, 0, 0);
        r.targetGuide = ImageInput::fromImage(noiseImage(33, 21, 5));
        const Result result = synthesize(r);
        ASSERT_EQ(result.status, Status::Ok);
        EXPECT_EQ(result.primary.width, 33);
        EXPECT_EQ(result.primary.height, 21);
    }
}

TEST(TexSynth, InpaintKeepsTheUnmaskedPixelsByteIdentical) {
    const int s = 56;
    Image image = noiseImage(s, s, 21, 4);
    Image mask = Image::make(s, s, 3, 8);
    for (int y = 15; y < 40; y++)
        for (int x = 20; x < 45; x++) std::memset(mask.pixel(x, y), 255, 3);
    // garbage in the hole: never copied
    for (int y = 15; y < 40; y++)
        for (int x = 20; x < 45; x++) {
            uint8_t* p = image.pixel(x, y);
            p[0] = 255, p[1] = 0, p[2] = 255;
        }
    Request r;
    r.mode = Mode::Inpaint;
    r.quality = Quality::Draft;
    r.examples.push_back(Example::fromImage(image));
    r.companions.push_back({"height", {ImageInput::fromImage(indexImage16(s, s))}});
    r.inpaintMask.image = ImageInput::fromImage(mask);
    const Result result = synthesize(r);
    ASSERT_EQ(result.status, Status::Ok);
    ASSERT_TRUE(result.primary.sameFormat(image));
    const Image& height = result.companions.at(0).image;
    size_t changed = 0;
    for (int y = 0; y < s; y++) {
        for (int x = 0; x < s; x++) {
            const bool hole = x >= 20 && x < 45 && y >= 15 && y < 40;
            if (!hole) {
                ASSERT_EQ(0, std::memcmp(result.primary.pixel(x, y), image.pixel(x, y), 4)) << x << "," << y;
                ASSERT_EQ(0, std::memcmp(height.pixel(x, y), r.companions[0].images[0].image.pixel(x, y), 2));
            } else {
                const uint8_t* p = result.primary.pixel(x, y);
                EXPECT_FALSE(p[0] == 255 && p[1] == 0 && p[2] == 255) << "copied from the hole at " << x << "," << y;
                const SourceCoord c = result.coordinates.coords[static_cast<size_t>(y) * s + x];
                EXPECT_FALSE(c.x >= 20 && c.x < 45 && c.y >= 15 && c.y < 40);
                changed++;
            }
        }
    }
    EXPECT_EQ(result.metrics.lockedPixels, static_cast<int64_t>(s * s - 25 * 25));
    EXPECT_EQ(result.metrics.synthesizedPixels, 25 * 25);
    EXPECT_EQ(changed, 625u);
}

TEST(TexSynth, InpaintFromTheImagesOwnAlpha) {
    Image image = noiseImage(40, 40, 4, 4);
    for (int y = 5; y < 12; y++)
        for (int x = 5; x < 30; x++) image.pixel(x, y)[3] = 0;
    Request r;
    r.mode = Mode::Inpaint;
    r.quality = Quality::Draft;
    r.examples.push_back(Example::fromImage(image));
    const Result result = synthesize(r);
    ASSERT_EQ(result.status, Status::Ok);
    EXPECT_EQ(result.metrics.synthesizedPixels, 7 * 25);
    for (int y = 5; y < 12; y++)
        for (int x = 5; x < 30; x++) EXPECT_EQ(result.primary.pixel(x, y)[3], 255);   // copied from opaque pixels

    // no alpha, no mask: refused
    Request rgb;
    rgb.mode = Mode::Inpaint;
    rgb.examples.push_back(Example::fromImage(noiseImage(20, 20, 4, 3)));
    EXPECT_EQ(synthesize(rgb).status, Status::InvalidRequest);
}

TEST(TexSynth, MakeTileableBecomesSeamlessAndKeepsTheInterior) {
    const int s = 72;
    Request r;
    r.mode = Mode::MakeTileable;
    r.quality = Quality::Balanced;
    r.examples.push_back(Example::fromImage(noiseImage(s, s, 31)));
    const Image input = r.examples[0].image.image;
    double inX = 0, inY = 0;
    seamScores(toRgba8(input), inX, inY);
    ASSERT_GT(std::max(inX, inY), 2.0) << "the test image should not tile";
    const Result result = synthesize(r);
    ASSERT_EQ(result.status, Status::Ok);
    const int band = static_cast<int>(std::lround(0.125 * s));
    for (int y = band; y < s - band; y++)
        for (int x = band; x < s - band; x++) ASSERT_EQ(0, std::memcmp(result.primary.pixel(x, y), input.pixel(x, y), 3));
    EXPECT_LT(result.metrics.seamScore, 1.6) << "input seam " << std::max(inX, inY);
    EXPECT_LT(result.metrics.seamScore, std::max(inX, inY) / 2);
    // wrap continuity: the step across the wrap is like the steps inside
    double seamX = 0, seamY = 0;
    seamScores(toRgba8(result.primary), seamX, seamY);
    EXPECT_NEAR(seamX, result.metrics.seamScoreX, 1e-9);
}

TEST(TexSynth, TileableOutputTiles) {
    Request r = basicRequest(Mode::Tileable, 80, 64, 48);
    r.quality = Quality::Balanced;
    const Result result = synthesize(r);
    ASSERT_EQ(result.status, Status::Ok);
    EXPECT_LT(result.metrics.seamScore, 1.6);
    Request e = basicRequest(Mode::Extend, 80, 64, 48);
    e.quality = Quality::Balanced;
    const Result extended = synthesize(e);
    ASSERT_EQ(extended.status, Status::Ok);
    EXPECT_GT(extended.metrics.seamScore, result.metrics.seamScore);
}

TEST(TexSynth, GuidedFollowsTheTargetGuide) {
    // the example: dark on the left, bright on the right; the target guide the other way round
    const int s = 48;
    Image example = noiseImage(s, s, 8);
    for (int y = 0; y < s; y++)
        for (int x = 0; x < s; x++)
            for (int c = 0; c < 3; c++) {
                uint8_t& v = example.pixel(x, y)[c];
                v = static_cast<uint8_t>(x < s / 2 ? v / 4 : 192 + v / 4);
            }
    Image guide = Image::make(64, 64, 1, 8);
    for (int y = 0; y < 64; y++)
        for (int x = 0; x < 64; x++) guide.data[y * 64 + x] = x < 32 ? 255 : 0;
    Request r;
    r.mode = Mode::Guided;
    r.quality = Quality::Draft;
    r.examples.push_back(Example::fromImage(example));
    r.targetGuide = ImageInput::fromImage(guide);
    const Result result = synthesize(r);
    ASSERT_EQ(result.status, Status::Ok);
    double left = 0, right = 0;
    for (int y = 0; y < 64; y++)
        for (int x = 0; x < 64; x++) (x < 32 ? left : right) += result.primary.pixel(x, y)[0];
    EXPECT_GT(left, right * 1.5) << "the bright half of the guide should get the bright texture";
}

// ---- examples, masks, companions

TEST(TexSynth, UnsampleableExamplesAreNeverCopiedAndMapIdsStayOriginal) {
    Request r = basicRequest(Mode::Extend, 40, 40);
    Example ignored = Example::fromImage(noiseImage(30, 30, 99));
    ignored.sampleable = false;
    r.examples.insert(r.examples.begin(), ignored);   // example 0 ignored, example 1 sampled
    const Result result = synthesize(r);
    ASSERT_EQ(result.status, Status::Ok);
    for (const SourceCoord& c : result.coordinates.coords) ASSERT_EQ(c.example, 1);

    // inpaint from another example only: the hole maps to example 1 (upstream mixed the ids up)
    Request in;
    in.mode = Mode::Inpaint;
    in.quality = Quality::Draft;
    Example first = Example::fromImage(noiseImage(32, 32, 1, 4));
    first.sampleable = false;
    for (int y = 0; y < 10; y++)
        for (int x = 0; x < 10; x++) first.image.image.pixel(x, y)[3] = 0;
    in.examples.push_back(first);
    in.examples.push_back(Example::fromImage(noiseImage(24, 24, 2)));
    const Result inpainted = synthesize(in);
    ASSERT_EQ(inpainted.status, Status::Ok);
    for (int y = 0; y < 32; y++)
        for (int x = 0; x < 32; x++) {
            const SourceCoord c = inpainted.coordinates.coords[y * 32 + x];
            if (x < 10 && y < 10) ASSERT_EQ(c.example, 1);
            else ASSERT_EQ(c.example, 0);
        }
    EXPECT_EQ(inpainted.primary.channels, 4);   // RGBA + RGB examples: the union
}

TEST(TexSynth, SampleMaskLimitsTheSources) {
    Request r = basicRequest(Mode::Extend, 40, 40, 40);
    Image mask = Image::make(40, 40, 1, 8);
    for (int y = 0; y < 40; y++)
        for (int x = 0; x < 20; x++) mask.data[y * 40 + x] = 255;
    r.examples[0].sampleMask = ImageInput::fromImage(mask);
    const Result result = synthesize(r);
    ASSERT_EQ(result.status, Status::Ok);
    for (const SourceCoord& c : result.coordinates.coords) ASSERT_LT(c.x, 20);
}

TEST(TexSynth, CompanionsAreExactCopiesInTheirOwnFormat) {
    const int s = 36;
    Request r = basicRequest(Mode::Tileable, 50, 44, s);
    Image normal = noiseImage(s, s, 77, 3);       // 8-bit RGB
    Image rough = noiseImage(s, s, 78, 4, 16);    // 16-bit RGBA
    r.companions.push_back({"height", {ImageInput::fromImage(indexImage16(s, s))}});
    r.companions.push_back({"normal", {ImageInput::fromImage(normal)}});
    r.companions.push_back({"orm", {ImageInput::fromImage(rough)}});
    const Result result = synthesize(r);
    ASSERT_EQ(result.status, Status::Ok);
    ASSERT_EQ(result.companions.size(), 3u);
    for (const CompanionOutput& out : result.companions) {
        const Image& src = out.name == "height" ? r.companions[0].images[0].image : out.name == "normal" ? normal : rough;
        ASSERT_TRUE(out.image.sameFormat(src)) << out.name << " " << describeFormat(out.image);
        ASSERT_EQ(out.image.width, 50);
        for (int y = 0; y < 44; y++)
            for (int x = 0; x < 50; x++) {
                const SourceCoord c = result.coordinates.coords[y * 50 + x];
                ASSERT_EQ(0, std::memcmp(out.image.pixel(x, y), src.pixel(c.x, c.y), src.bytesPerPixel())) << out.name;
            }
    }
    // and through a PNG file: 16-bit samples survive
    const fs::path dir = tempDir();
    for (const CompanionOutput& out : result.companions) {
        std::string error;
        ASSERT_TRUE(writePngWithRecipe(dir / (out.name + ".png"), out.image, nlohmann::json{{"role", out.name}}, error)) << error;
        Image back;
        ASSERT_TRUE(loadImage(dir / (out.name + ".png"), back, error)) << error;
        ASSERT_TRUE(back.sameFormat(out.image)) << out.name << " " << describeFormat(back);
        EXPECT_TRUE(back.data == out.image.data) << out.name;
    }
    fs::remove_all(dir);
}

TEST(TexSynth, CompanionSizeMismatchIsAnError) {
    Request r = basicRequest(Mode::Extend, 30, 30, 32);
    r.companions.push_back({"normal", {ImageInput::fromImage(noiseImage(16, 16, 1))}});
    const Result result = synthesize(r);
    ASSERT_EQ(result.status, Status::InvalidRequest);
    ASSERT_FALSE(result.errors.empty());
    EXPECT_EQ(result.errors[0].field, "companions[0].images[0]");
    EXPECT_NE(result.errors[0].message.find("resize it beforehand"), std::string::npos);
}

TEST(TexSynth, CompanionNaming) {
    char sep = 0;
    EXPECT_EQ(companionSuffix("rock_albedo", "rock_normal", sep), "normal");
    EXPECT_EQ(sep, '_');
    EXPECT_EQ(companionSuffix("rock_albedo", "rock_ao", sep), "ao");
    EXPECT_EQ(companionSuffix("rock", "rock_normal_gl", sep), "normal_gl");
    EXPECT_EQ(companionSuffix("albedo", "normal", sep), "normal");
    EXPECT_EQ(companionSuffix("T-Rock-D", "T-Rock-N", sep), "N");
    EXPECT_EQ(sep, '-');
    EXPECT_EQ(companionSuffix("rock", "rock", sep), "");
    EXPECT_EQ(companionOutputPath("/out/rock_tile.png", "/in/rock_albedo.png", "/in/rock_normal.png"), fs::path("/out/rock_tile_normal.png"));
    EXPECT_EQ(companionOutputPath("/out/tile.png", "/in/T-Rock-D.png", "/in/T-Rock-N.tga"), fs::path("/out/tile-N.png"));
}

// ---- parameters

TEST(TexSynth, ParameterValidation) {
    Params p = presetParams(Quality::Balanced);
    EXPECT_TRUE(validateParams(p).empty());
    p.cauchyDispersion = 0.0f;
    p.backtrackStages = 11;
    p.backtrackPercent = 1.0f;
    p.nearestNeighbors = 0;
    p.guideAlpha = std::nanf("");
    const std::vector<Problem> problems = validateParams(p);
    ASSERT_EQ(problems.size(), 5u);
    EXPECT_EQ(problems[0].field, "params.nearestNeighbors");
    EXPECT_EQ(problems[1].field, "params.cauchyDispersion");
    EXPECT_NE(problems[1].message.find("(0, 1]"), std::string::npos);

    Request r = basicRequest(Mode::Extend, 20, 20);
    r.overrides.cauchyDispersion = 0.0f;
    EXPECT_EQ(synthesize(r).status, Status::InvalidRequest);

    EXPECT_EQ(resolveParams(Quality::Draft, {}).nearestNeighbors, 20);
    ParamOverrides o;
    o.nearestNeighbors = 33;
    EXPECT_EQ(resolveParams(Quality::High, o).nearestNeighbors, 33);
    EXPECT_EQ(resolveParams(Quality::High, o).randomSampleLocations, 80);
}

TEST(TexSynth, ManyBacktrackStagesOnATinyExample) {
    // upstream crashed: its blur pyramid scaled an 8 px example down to 0 px
    Request r = basicRequest(Mode::Tileable, 24, 24, 8);
    r.overrides.backtrackStages = 10;
    r.overrides.backtrackPercent = 0.9f;
    const Result result = synthesize(r);
    ASSERT_EQ(result.status, Status::Ok);
    EXPECT_EQ(result.stageSeconds.size(), 11u);
    Request zero = basicRequest(Mode::Extend, 24, 24, 8);
    zero.overrides.backtrackStages = 0;
    EXPECT_EQ(synthesize(zero).status, Status::Ok);
}

TEST(TexSynth, RequestProblemsAreReportedTogether) {
    Request r;
    r.mode = Mode::Extend;
    const Result empty = synthesize(r);
    EXPECT_EQ(empty.status, Status::InvalidRequest);
    EXPECT_GE(empty.errors.size(), 2u);   // no example, no size

    Request missing = basicRequest(Mode::Extend, 10, 10);
    missing.examples.push_back(Example::fromPath("/nonexistent/texture.png"));
    const Result result = synthesize(missing);
    EXPECT_EQ(result.status, Status::LoadFailed);
    ASSERT_EQ(result.errors.size(), 1u);
    EXPECT_EQ(result.errors[0].field, "examples[1].image");
}

// ---- progress and cancellation

TEST(TexSynth, ProgressReportsStagesAndPreviews) {
    Request r = basicRequest(Mode::Extend, 64, 64);
    Control control;
    control.progressIntervalSeconds = 0.0;
    control.previewIntervalSeconds = 0.0001;
    control.previewMaxSize = 32;
    double last = -1.0;
    int previews = 0, calls = 0, lastStage = -1;
    bool monotonic = true;
    control.onProgress = [&](const Progress& p) {
        calls++;
        monotonic = monotonic && p.fraction >= last && p.stage >= lastStage;
        last = p.fraction;
        lastStage = p.stage;
        if (p.preview) {
            previews++;
            EXPECT_EQ(p.preview->width, 32);
            EXPECT_EQ(p.preview->channels, 4);
        }
        EXPECT_EQ(p.stageCount, 4);   // draft: 3 backtracking stages
    };
    const Result result = synthesize(r, control);
    ASSERT_EQ(result.status, Status::Ok);
    EXPECT_TRUE(monotonic);
    EXPECT_DOUBLE_EQ(last, 1.0);
    EXPECT_GT(calls, 8);
    EXPECT_GT(previews, 0);
}

TEST(TexSynth, CancelReturnsPromptly) {
    Request r = basicRequest(Mode::Extend, 512, 512, 64);
    r.quality = Quality::High;
    std::atomic<bool> cancel{false};
    Control control;
    control.cancel = &cancel;
    std::thread canceller([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        cancel = true;
    });
    const auto start = std::chrono::steady_clock::now();
    const Result result = synthesize(r, control);
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    canceller.join();
    EXPECT_EQ(result.status, Status::Cancelled);
    EXPECT_LT(seconds, 1.5);

    // from the progress callback
    std::atomic<bool> stop{false};
    Control fromCallback;
    fromCallback.cancel = &stop;
    fromCallback.progressIntervalSeconds = 0.0;
    fromCallback.onProgress = [&](const Progress& p) {
        if (p.fraction > 0.02) stop = true;
    };
    Request small = basicRequest(Mode::Extend, 160, 160, 48);
    EXPECT_EQ(synthesize(small, fromCallback).status, Status::Cancelled);
}

// ---- recipe

TEST(TexSynthRecipe, RoundTripAndGarbledChunks) {
    const fs::path dir = tempDir();
    Request r = basicRequest(Mode::Tileable, 32, 32, 24);
    const fs::path examplePath = dir / "rock_albedo.png";
    {
        std::vector<uint8_t> png;
        ASSERT_TRUE(encodePng(r.examples[0].image.image, png));
        writeFile(examplePath, png);
    }
    r.examples[0].image = ImageInput::fromPath(examplePath);
    r.seed = 42;
    const Result result = synthesize(r);
    ASSERT_EQ(result.status, Status::Ok);
    EXPECT_EQ(r.examples[0].image.sha1.size(), 40u);   // loaded into the request

    RecipePaths paths;
    paths.projectDir = dir;
    paths.outputs["primary"] = dir / "out" / "tile.png";
    const nlohmann::json recipe = makeRecipe(r, result, "primary", paths);
    EXPECT_EQ(recipe["mode"], "tileable");
    EXPECT_EQ(recipe["seed"], 42);
    EXPECT_EQ(recipe["examples"][0]["path"], "rock_albedo.png");
    EXPECT_EQ(recipe["outputs"]["primary"], "out/tile.png");
    EXPECT_EQ(recipe["params"]["nearestNeighbors"], 20);
    EXPECT_EQ(recipe["pathBase"], "project");

    fs::create_directories(dir / "out");
    const fs::path out = dir / "out" / "tile.png";
    std::string error;
    ASSERT_TRUE(writePngWithRecipe(out, result.primary, recipe, error)) << error;
    const std::optional<nlohmann::json> back = readRecipe(out, &error);
    ASSERT_TRUE(back.has_value()) << error;
    EXPECT_EQ(*back, recipe);

    // the image still decodes (decoders skip the chunk), and the chunk sits right after IHDR
    Image decoded;
    ASSERT_TRUE(loadImage(out, decoded, error)) << error;
    EXPECT_TRUE(decoded.data == result.primary.data);
    std::vector<uint8_t> bytes = readFile(out);
    ASSERT_GT(bytes.size(), 45u);
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(&bytes[37]), 4), "iTXt");

    // sources: unchanged, then changed, then missing
    std::vector<SourceState> states = checkRecipeSources(*back, out, dir);
    ASSERT_EQ(states.size(), 1u);
    EXPECT_EQ(states[0].state, "unchanged");
    std::vector<uint8_t> example = readFile(examplePath);
    example.push_back(0);   // a trailing byte: other content, same pixels
    writeFile(examplePath, example);
    EXPECT_EQ(checkRecipeSources(*back, out, dir)[0].state, "changed");
    fs::remove(examplePath);
    EXPECT_EQ(checkRecipeSources(*back, out, dir)[0].state, "missing");

    // inserting again replaces the chunk
    std::vector<uint8_t> twice;
    ASSERT_TRUE(insertRecipeChunk(bytes, "{\"format\":1,\"x\":2}", twice, error)) << error;
    EXPECT_EQ(readRecipeTextFromPng(twice.data(), twice.size()).value_or(""), "{\"format\":1,\"x\":2}");
    size_t count = 0;
    for (size_t i = 0; i + 4 < twice.size(); i++) count += std::memcmp(&twice[i], "jasper.synthesis", 16) == 0;
    EXPECT_EQ(count, 1u);

    // garbled: one byte of the text flipped -> CRC mismatch
    std::vector<uint8_t> garbled = bytes;
    garbled[60] ^= 0x20;
    EXPECT_FALSE(readRecipeTextFromPng(garbled.data(), garbled.size(), &error).has_value());
    EXPECT_NE(error.find("CRC"), std::string::npos);

    // truncated inside IDAT: the recipe (before it) is still read
    std::vector<uint8_t> truncated(bytes.begin(), bytes.end() - 20);
    EXPECT_TRUE(readRecipeTextFromPng(truncated.data(), truncated.size(), &error).has_value());

    // no recipe, not a PNG
    std::vector<uint8_t> plain;
    ASSERT_TRUE(encodePng(result.primary, plain));
    error = "x";
    EXPECT_FALSE(readRecipeTextFromPng(plain.data(), plain.size(), &error).has_value());
    EXPECT_TRUE(error.empty());
    const std::string text = "not a png at all";
    EXPECT_FALSE(readRecipeTextFromPng(reinterpret_cast<const uint8_t*>(text.data()), text.size(), &error).has_value());
    EXPECT_EQ(error, "not a PNG");

    // a tEXt chunk with the keyword is read too (another writer)
    std::vector<uint8_t> withText(plain.begin(), plain.begin() + 33);
    const std::string payload = std::string("jasper.synthesis") + '\0' + "{\"format\":1}";
    const uint32_t len = static_cast<uint32_t>(payload.size());
    const uint8_t lenBytes[4] = {static_cast<uint8_t>(len >> 24), static_cast<uint8_t>(len >> 16), static_cast<uint8_t>(len >> 8), static_cast<uint8_t>(len)};
    withText.insert(withText.end(), lenBytes, lenBytes + 4);
    std::vector<uint8_t> typeAndData = {'t', 'E', 'X', 't'};
    typeAndData.insert(typeAndData.end(), payload.begin(), payload.end());
    withText.insert(withText.end(), typeAndData.begin(), typeAndData.end());
    // CRC over type + data (the same polynomial as the writer's)
    uint32_t crc = 0xFFFFFFFFu;
    for (uint8_t b : typeAndData) {
        crc ^= b;
        for (int k = 0; k < 8; k++) crc = (crc & 1) ? 0xEDB88320u ^ (crc >> 1) : crc >> 1;
    }
    crc = ~crc;
    const uint8_t crcBytes[4] = {static_cast<uint8_t>(crc >> 24), static_cast<uint8_t>(crc >> 16), static_cast<uint8_t>(crc >> 8), static_cast<uint8_t>(crc)};
    withText.insert(withText.end(), crcBytes, crcBytes + 4);
    withText.insert(withText.end(), plain.begin() + 33, plain.end());
    EXPECT_EQ(readRecipeTextFromPng(withText.data(), withText.size(), &error).value_or(""), "{\"format\":1}");
    fs::remove_all(dir);
}
