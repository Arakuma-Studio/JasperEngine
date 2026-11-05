// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// jasperengine-texsynth-tests: the texture synthesis core (editor/texsynth) on generated images.

#include "texsynth/SynthesisRecipe.h"
#include "object/ui/Image.h"
#include "texsynth/TextureSynthesis.h"
#include "util/FileUtils.h"

#include "support/EditorTestSupport.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <thread>

#ifdef __linux__
#include <sys/wait.h>
#include <unistd.h>
#endif

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

// ---- determinism

TEST(TexSynth, IdenticalBytesWhateverTheThreadCount) {
    Request one = basicRequest(Mode::Tileable, 64, 48);
    one.quality = Quality::Balanced;
    one.seed = 1234;
    one.threads = 1;
    Request eight = one;
    eight.threads = 8;
    const Result a = synthesize(one);
    const Result b = synthesize(eight);
    ASSERT_EQ(a.status, Status::Ok);
    ASSERT_EQ(b.status, Status::Ok);
    EXPECT_EQ(b.threads, 8);
    EXPECT_TRUE(a.primary.data == b.primary.data);
    EXPECT_TRUE(a.uncertainty == b.uncertainty);
    ASSERT_EQ(a.coordinates.coords.size(), b.coordinates.coords.size());
    for (size_t i = 0; i < a.coordinates.coords.size(); i++) {
        ASSERT_EQ(a.coordinates.coords[i].x, b.coordinates.coords[i].x);
        ASSERT_EQ(a.coordinates.coords[i].y, b.coordinates.coords[i].y);
    }

    // another seed, another texture
    Request other = one;
    other.seed = 99;
    const Result d = synthesize(other);
    EXPECT_FALSE(a.primary.data == d.primary.data);
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
}

TEST(TexSynth, TileableOutputTiles) {
    Request r = basicRequest(Mode::Tileable, 80, 64, 48);
    r.quality = Quality::Balanced;
    const Result result = synthesize(r);
    ASSERT_EQ(result.status, Status::Ok);
    EXPECT_LT(result.metrics.seamScore, 1.6);
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
            ASSERT_EQ(c.example, (x < 10 && y < 10) ? 1 : 0);
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
    control.previewIntervalSeconds = 1.0;
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
    };
    const Result result = synthesize(r, control);
    ASSERT_EQ(result.status, Status::Ok);
    EXPECT_TRUE(monotonic);
    EXPECT_DOUBLE_EQ(last, 1.0);
    EXPECT_GT(calls, 0);
    EXPECT_GT(previews, 0);
}

TEST(TexSynth, CancelReturnsPromptly) {
    Request r = basicRequest(Mode::Extend, 64, 64);
    std::atomic<bool> cancel{false};
    Control control;
    control.cancel = &cancel;
    control.progressIntervalSeconds = 0.0;
    control.onProgress = [&](const Progress& p) {
        if (p.fraction > 0.02) cancel = true;
    };
    const auto start = std::chrono::steady_clock::now();
    const Result result = synthesize(r, control);
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    EXPECT_TRUE(cancel.load()) << "synthesis must have begun before cancellation";
    EXPECT_EQ(result.status, Status::Cancelled);
    EXPECT_LT(seconds, 1.5);
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

// ---- recipe text: file names that are not UTF-8, garbled recipes (never an exception)

TEST(TexSynthRecipe, NonUtf8PathsAreReportedNotThrown) {
    EXPECT_TRUE(isValidUtf8("assets/rock_albedo.png"));
    EXPECT_TRUE(isValidUtf8("assets/caf\xC3\xA9.png"));            // é in UTF-8
    EXPECT_TRUE(isValidUtf8("\xF0\x9F\x99\x82"));                  // U+1F642
    EXPECT_FALSE(isValidUtf8("assets/caf\xE9.png"));               // é in Latin-1
    EXPECT_FALSE(isValidUtf8("\xC0\xAF"));                         // overlong '/'
    EXPECT_FALSE(isValidUtf8("\xED\xA0\x80"));                     // a surrogate
    EXPECT_FALSE(isValidUtf8("\xF4\x90\x80\x80"));                 // beyond U+10FFFF
    EXPECT_FALSE(isValidUtf8("\xE2\x82"));                         // truncated
    EXPECT_EQ(printableUtf8("caf\xE9-\xC3\xA9.png"), "caf\\xE9-\xC3\xA9.png");

    const nlohmann::json bad = {{"examples", nlohmann::json::array({{{"path", "caf\xE9.png"}}})}};
    std::string text, error;
    EXPECT_FALSE(jsonText(bad, text, error));
    EXPECT_NE(error.find("caf\\xE9.png"), std::string::npos) << error;
    EXPECT_NE(error.find("examples[0].path"), std::string::npos) << error;
    EXPECT_FALSE(recipeText(bad, text, error));
    EXPECT_NE(error.find("rename"), std::string::npos) << error;
    EXPECT_NE(jsonTextLossy(bad).find("\xEF\xBF\xBD"), std::string::npos);   // U+FFFD

    // a finished synthesis whose example has a Latin-1 name: the recipe can't be written, an error says so
    const fs::path dir = tempDir();
    Request r = basicRequest(Mode::Extend, 16, 16, 16);
    const Result result = synthesize(r);
    ASSERT_EQ(result.status, Status::Ok);
    r.examples[0].image.path = dir / "caf\xE9.png";
    RecipePaths paths;
    paths.projectDir = dir;
    paths.outputs["primary"] = dir / "out.png";
    const nlohmann::json recipe = makeRecipe(r, result, "primary", paths);
    EXPECT_FALSE(isValidUtf8(recipePath(r.examples[0].image.path, paths)));
    std::vector<uint8_t> png;
    EXPECT_FALSE(encodePngWithRecipe(result.primary, recipe, png, error));
    EXPECT_FALSE(writePngWithRecipe(dir / "out.png", result.primary, recipe, error));
    EXPECT_NE(error.find("caf\\xE9.png"), std::string::npos) << error;
    EXPECT_FALSE(fs::exists(dir / "out.png"));
    fs::remove_all(dir);
}

TEST(TexSynthRecipe, GarbledRecipesAreReportedNotThrown) {
    using nlohmann::json;
    const json good = json::parse(R"({"format":1,"mode":"tileable","role":"primary","pathBase":"project","seed":"18446744073709551615",
        "size":[64,64],"params":{"nearestNeighbors":20,"cauchyDispersion":1.0,"guideDecay":true},
        "examples":[{"path":"a.png","sha1":"x","sampleable":true,"sampleMask":{"path":"m.png","sha1":"y"}}],
        "companions":[{"name":"normal","sources":[{"path":"n.png","sha1":"z"}]}],
        "inpaintMask":{"source":"alpha","invert":false},"outputs":{"primary":"o.png"}})");
    std::string error;
    EXPECT_TRUE(validateRecipe(good, error)) << error;
    EXPECT_EQ(recipeSeed(good).value_or(0), 18446744073709551615ULL);
    EXPECT_EQ(jsonString(good, "mode"), "tileable");
    EXPECT_EQ(jsonString(good, "size", "?"), "?");          // another type: the fallback
    EXPECT_EQ(jsonString(json(5), "mode", "f"), "f");       // not an object
    EXPECT_TRUE(jsonBool(good["examples"][0], "sampleable", false));

    const std::vector<std::pair<std::string, std::string>> garbled = {
        {R"({"format":1,"examples":[{"path":5}]})", "examples[0].path is a number, expected a string"},
        {R"({"format":1,"examples":[{"path":"a.png","sha1":7}]})", "examples[0].sha1"},
        {R"({"format":1,"examples":[5]})", "examples[0] is a number, expected an object"},
        {R"({"format":1,"examples":{"path":"a.png"}})", "examples is an object, expected an array"},
        {R"({"format":1,"pathBase":3})", "pathBase is a number"},
        {R"({"format":1,"role":[]})", "role is an array"},
        {R"({"format":"1"})", "format is a string"},
        {R"({"mode":"tileable"})", "format is missing"},
        {R"({"format":1,"seed":-3})", "seed"},
        {R"({"format":1,"seed":1.5})", "seed"},
        {R"({"format":1,"seed":"12x"})", "seed"},
        {R"({"format":1,"seed":"99999999999999999999"})", "seed"},
        {R"({"format":1,"size":[1]})", "size"},
        {R"({"format":1,"size":["a",2]})", "size.width"},
        {R"({"format":1,"params":{"nearestNeighbors":1.5}})", "params.nearestNeighbors"},
        {R"({"format":1,"params":{"cauchyDispersion":"x"}})", "params.cauchyDispersion is a string"},
        {R"({"format":1,"params":{"cauchyDispersion":1e300}})", "params.cauchyDispersion"},
        {R"({"format":1,"params":{"guideDecay":1}})", "params.guideDecay"},
        {R"({"format":1,"params":7})", "params is a number"},
        {R"({"format":1,"companions":[{"name":1}]})", "companions[0].name"},
        {R"({"format":1,"companions":[{"name":"n","sources":[{"path":false}]}]})", "companions[0].sources[0].path"},
        {R"({"format":1,"companions":[{"name":"n","sources":{}}]})", "companions[0].sources is an object"},
        {R"({"format":1,"inpaintMask":{"invert":"yes"}})", "inpaintMask.invert"},
        {R"({"format":1,"targetGuide":"g.png"})", "targetGuide is a string"},
        {R"({"format":1,"outputs":{"primary":3}})", "outputs.primary is a number"},
        {R"([1,2])", "the recipe is an array"},
    };
    for (const auto& [text, needle] : garbled) {
        const json recipe = json::parse(text);
        error.clear();
        EXPECT_FALSE(validateRecipe(recipe, error)) << text;
        EXPECT_NE(error.find(needle), std::string::npos) << text << " -> " << error;
        // the readers never throw on it
        EXPECT_NO_THROW(checkRecipeSources(recipe, "/tmp/x.png", "/tmp", [](const fs::path&) { return std::string("h"); })) << text;
        EXPECT_NO_THROW(recipeSeed(recipe)) << text;
    }
    // what checkRecipeSources makes of the wrong types: no path -> "unknown", a sha1 of another type -> "changed"
    const std::vector<SourceState> states =
        checkRecipeSources(json::parse(R"({"format":1,"pathBase":3,"examples":[{"path":5},{"path":"a.png","sha1":7}]})"), "/p/x.png", "/p",
                           [](const fs::path&) { return std::string("h"); });
    ASSERT_EQ(states.size(), 2u);
    EXPECT_EQ(states[0].state, "unknown");
    EXPECT_EQ(states[1].state, "changed");
    EXPECT_EQ(states[1].resolved, fs::path("/p/a.png"));   // pathBase of another type: "file", the PNG's directory
}

// ---- writing a set of files (the CLI): all or nothing

TEST(TexSynthRecipe, WriteFilesAllOrNothing) {
    const fs::path dir = tempDir();
    auto bytes = [](const std::string& text) { return std::vector<uint8_t>(text.begin(), text.end()); };
    auto text = [&](const fs::path& path) {
        const std::vector<uint8_t> b = readFile(path);
        return std::string(b.begin(), b.end());
    };
    auto listing = [&]() {
        std::vector<std::string> names;
        for (const auto& entry : fs::recursive_directory_iterator(dir)) names.push_back(fs::relative(entry.path(), dir).generic_string());
        std::sort(names.begin(), names.end());
        return names;
    };
    writeFile(dir / "primary.png", bytes("old primary"));
    writeFile(dir / "blocker", bytes("a file where a directory is expected"));
    std::string failed, error;

    // staging fails (a path component is a file): the primary keeps its content, nothing is left behind
    EXPECT_FALSE(writeFilesAllOrNothing({{dir / "primary.png", bytes("new primary")}, {dir / "new" / "deep" / "normal.png", bytes("n")},
                                         {dir / "blocker" / "uncertainty.png", bytes("u")}},
                                        failed, error));
    EXPECT_EQ(failed, jasperengine::editor::FileUtils::pathToUtf8((dir / "blocker" / "uncertainty.png")));
    EXPECT_NE(error.find("nothing was written"), std::string::npos) << error;
    EXPECT_EQ(text(dir / "primary.png"), "old primary");
    EXPECT_EQ(listing(), (std::vector<std::string>{"blocker", "primary.png"}));

    // committing fails (the target is a directory that is not empty): the replaced primary comes back
    fs::create_directories(dir / "taken" / "inside");
    EXPECT_FALSE(writeFilesAllOrNothing({{dir / "primary.png", bytes("new primary")}, {dir / "taken", bytes("t")}}, failed, error));
    EXPECT_EQ(failed, jasperengine::editor::FileUtils::pathToUtf8((dir / "taken")));
    EXPECT_EQ(text(dir / "primary.png"), "old primary");
    EXPECT_EQ(listing(), (std::vector<std::string>{"blocker", "primary.png", "taken", "taken/inside"}));

    // success: written and replaced, no staged or previous copies left
    ASSERT_TRUE(writeFilesAllOrNothing({{dir / "primary.png", bytes("new primary")}, {dir / "sub" / "normal.png", bytes("n")}}, failed, error)) << error;
    EXPECT_EQ(text(dir / "primary.png"), "new primary");
    EXPECT_EQ(text(dir / "sub" / "normal.png"), "n");
    EXPECT_EQ(listing(), (std::vector<std::string>{"blocker", "primary.png", "sub", "sub/normal.png", "taken", "taken/inside"}));
    fs::remove_all(dir);
}

// ---- memory, cancellation and failures

namespace {

    // A PNG of the given size without its pixels (IHDR, an empty IDAT, IEND): stbi_info reads
    // the header up to the first IDAT, so a huge image can be probed without writing it
    std::vector<uint8_t> pngHeaderOnly(uint32_t w, uint32_t h) {
        std::vector<uint8_t> png = {137, 80, 78, 71, 13, 10, 26, 10};
        auto chunk = [&](const char* type, const std::vector<uint8_t>& data) {
            const uint32_t n = static_cast<uint32_t>(data.size());
            const uint8_t length[4] = {static_cast<uint8_t>(n >> 24), static_cast<uint8_t>(n >> 16), static_cast<uint8_t>(n >> 8), static_cast<uint8_t>(n)};
            png.insert(png.end(), length, length + 4);
            const size_t start = png.size();
            png.insert(png.end(), type, type + 4);
            png.insert(png.end(), data.begin(), data.end());
            uint32_t crc = 0xFFFFFFFFu;
            for (size_t i = start; i < png.size(); i++) {
                crc ^= png[i];
                for (int k = 0; k < 8; k++) crc = (crc & 1) ? 0xEDB88320u ^ (crc >> 1) : crc >> 1;
            }
            crc = ~crc;
            const uint8_t c[4] = {static_cast<uint8_t>(crc >> 24), static_cast<uint8_t>(crc >> 16), static_cast<uint8_t>(crc >> 8), static_cast<uint8_t>(crc)};
            png.insert(png.end(), c, c + 4);
        };
        chunk("IHDR", {static_cast<uint8_t>(w >> 24), static_cast<uint8_t>(w >> 16), static_cast<uint8_t>(w >> 8), static_cast<uint8_t>(w),
                       static_cast<uint8_t>(h >> 24), static_cast<uint8_t>(h >> 16), static_cast<uint8_t>(h >> 8), static_cast<uint8_t>(h), 8, 2, 0, 0, 0});
        chunk("IDAT", {});
        chunk("IEND", {});
        return png;
    }

}

TEST(TexSynth, CancelReachesThePreparation) {
    // a large example in guided style transfer with 10 stages: seconds of preparation (decode,
    // blur, histogram, pyramids) before the first stage, which used to ignore the cancel flag
    Request r;
    r.mode = Mode::Guided;
    r.quality = Quality::High;
    r.overrides.backtrackStages = 10;
    r.examples.push_back(Example::fromImage(Image::make(3072, 3072, 3, 8)));
    r.targetGuide = ImageInput::fromImage(noiseImage(64, 64, 4));
    std::atomic<bool> cancel{false};
    std::atomic<int> progressCalls{0};
    Control control;
    control.cancel = &cancel;
    // called at the start of every stage: the cancel, 100 ms into seconds of preparation, ends
    // the synthesis before the first one (the old code prepared to the end and started it)
    control.onProgress = [&](const Progress&) { progressCalls++; };
    std::thread canceller([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        cancel = true;
    });
    const auto start = std::chrono::steady_clock::now();
    const Result result = synthesize(r, control);
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    canceller.join();
    EXPECT_EQ(result.status, Status::Cancelled);
    EXPECT_EQ(progressCalls.load(), 0) << "a stage started after the cancel";
    // how soon is the benchmark run's (an instrumented build or a loaded builder is slower)
    std::printf("[bench] cancelled synthesis returned after %.2f s (budget 1.5; the old code took 2.75)\n", seconds);
    if (const double slack = jasperengine::editor::test::perfSlack(); slack > 0.0) {
        EXPECT_LT(seconds, 1.5 * slack);
    }
}

TEST(TexSynth, OversizedInputsAreRefusedBeforeDecoding) {
    const fs::path dir = tempDir();
    // a 9000 x 9000 example: refused from its header (it has no pixel data at all)
    writeFile(dir / "huge.png", pngHeaderOnly(9000, 9000));
    Request r = basicRequest(Mode::Extend, 32, 32);
    r.examples[0] = Example::fromPath(dir / "huge.png");
    Result result = synthesize(r);
    EXPECT_EQ(result.status, Status::InvalidRequest);
    ASSERT_FALSE(result.errors.empty());
    EXPECT_EQ(result.errors[0].field, "examples[0].image");
    EXPECT_NE(result.errors[0].message.find("9000x9000, at most 8192 per side"), std::string::npos) << result.errors[0].message;
    // decoding checks the header first too
    Image image;
    std::string error;
    const std::vector<uint8_t> header = pngHeaderOnly(9000, 9000);
    EXPECT_FALSE(loadImageFromMemory(header.data(), header.size(), image, error, 8192));
    EXPECT_NE(error.find("at most 8192 per side"), std::string::npos) << error;

    // five 8192 x 8192 inputs: 335 megapixels, over the budget, refused before decoding
    writeFile(dir / "big.png", pngHeaderOnly(8192, 8192));
    Request many = basicRequest(Mode::Extend, 32, 32);
    many.examples.clear();
    for (int i = 0; i < 5; i++) many.examples.push_back(Example::fromPath(dir / "big.png"));
    result = synthesize(many);
    EXPECT_EQ(result.status, Status::InvalidRequest);
    ASSERT_FALSE(result.errors.empty());
    EXPECT_NE(result.errors[0].message.find("335.5 megapixels, at most 268.4"), std::string::npos) << result.errors[0].message;
    fs::remove_all(dir);
}

// Not under ASan: the address-space limit is the process size plus 40 MB, and an ASan process's
// size is its shadow reservation (terabytes): nothing would fail. ASan's allocator also aborts on
// an allocation it cannot make instead of failing it, which is what this checks the handling of.
#if defined(__linux__) && !defined(JASPERENGINE_SANITIZE_ADDRESS) && !defined(__SANITIZE_ADDRESS__)
// In a process of its own (jasperengine-texsynth-oom, built next to this binary, see
// standalone/SynthesisOutOfMemory.cpp): an address-space limit makes the synthesis' allocations
// fail, and the result is a failed status, not a terminate
TEST(TexSynthOutOfMemory, IsAFailedStatusNotATerminate) {
    char self[4096] = {};
    const ssize_t length = ::readlink("/proc/self/exe", self, sizeof(self) - 1);
    ASSERT_GT(length, 0);
    std::string program(self, static_cast<size_t>(length));
    program = program.substr(0, program.rfind('/') + 1) + "jasperengine-texsynth-oom";
    const int status = std::system(program.c_str());
    ASSERT_TRUE(WIFEXITED(status)) << "it crashed";
    EXPECT_EQ(WEXITSTATUS(status), 0) << "the synthesis did not report out of memory (its stderr says why)";
}
#endif
