// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S83's generators on the GPU (docs/specs/S83-title-effects-generators.md §7), each preset as
// Pachislop's harness draws it (tests/harness/cases.ts generatorProject: a following generator
// follows the standard title, above or below as its preset places it; the others alone on the
// 1920 x 1080 canvas), through the whole renderer at 0.25:
// - TitleGenGpu.PresetGoldens (AC3): every preset of every S83 generator against
//   golden/titleart/gen/<type>_<preset>.png (written on glcore, Vulkan matches), with S80's
//   golden limits.
// - TitleGenParityGpu.PachislopPresets (AC4): the ported presets against Pachislop's references
//   (golden/titleart/pachislop/generator/), trimmed and centred, with S80 AC12's metric.
// - TitleGenGpu.Deterministic (AC5): a preset rendered twice from fresh renderers is identical;
//   another seed changes it.
// - TitleGenGpu.FallbackWithinClass (AC6): the RGBA8 path (TITLE_ART_TEST_NO_FLOAT_TARGETS, and
//   TITLE_ART_TEST_NO_FLOAT_BLEND) agrees with the float path within each definition's class.
// - QuadBatchGpu (AC13): a 0.3 px quad keeps its area x colour, quads at and above min_px match
//   their analytic coverage, 65,536 instances draw in one call.

#include <gtest/gtest.h>

#include "pool/ShaderPool.h"
#include "texture/TexturePrepare.h"
#include "titleart/gpu/QuadBatch.h"
#include "titleart/render/TitleArtCaps.h"
#include "titleart/render/TitleEffectContext.h"
#include "titleart/render/TitlePassKit.h"

#include "../support/TitleGenTestSupport.h"

#include "GpuEnvironment.h"
#include "HeadlessContext.h"

#include <map>
#include <set>

using namespace jasperengine;
using namespace jasperengine::titleart;
using namespace jasperengine::titleart::test;
using namespace jasperengine::test;

namespace {

    constexpr float GEN_SCALE = 0.25f;

    struct PresetCase {
        std::string type;
        std::string id;
        std::string source;     // Pachislop's "<type>/<preset>" ("" for a new preset)
        Json preset;
    };

    // every preset of every S83 generator (the definitions with a preset file; S80's Fill and
    // Pattern Fill have their own goldens)
    std::vector<PresetCase> presetCases() {
        std::vector<PresetCase> cases;
        for (const GeneratorDef* d : TitleEffectRegistry::get().generators()) {
            if (!d->presetFile || std::string(d->type) == "pattern_fill") continue;
            for (const Json& p : loadGeneratorPresets(d->type)) cases.push_back({d->type, p.value("id", std::string()), p.value("source", std::string()), p});
        }
        return cases;
    }

    std::filesystem::path goldenFile(const std::string& relative) {
        return std::filesystem::path(getGoldenDirectory()) / relative;
    }

    // golden/titleart/gen/<name>.png with S80 §6.12's limits; JASPER_TEST_UPDATE_GOLDEN=1 writes it (glcore)
    void expectGenGolden(const std::string& name, const GenImage& image) {
        const std::filesystem::path path = goldenFile("titleart/gen/" + name + ".png");
        if (isGoldenUpdateMode()) {
            if (std::string(HeadlessContext::getBackendName()) == "glcore") {
                std::filesystem::create_directories(path.parent_path());
                ASSERT_TRUE(writePng(path.string(), image.rgba, image.width, image.height)) << path.string();
            }
            return;
        }
        GenImage golden;
        ASSERT_TRUE(readPng(path.string(), golden.rgba, golden.width, golden.height)) << "no golden " << path.string()
                                                                                         << " (JASPER_TEST_UPDATE_GOLDEN=1 writes it)";
        const GenPixelStats stats = compareGenImages(golden, image);
        const bool ok = stats.max <= 48 && stats.mean <= 0.25 && stats.fractionOver8 <= 0.001;
        if (!ok) {
            const std::filesystem::path actual = std::filesystem::path(getOutputDirectory()) / ("titleart-gen-" + name + ".actual.png");
            std::filesystem::create_directories(actual.parent_path());
            writePng(actual.string(), image.rgba, image.width, image.height);
        }
        EXPECT_TRUE(ok) << name << ": " << stats.text();
    }

    class TitleGenGpu : public ::testing::Test {
    protected:
        void SetUp() override { JASPER_REQUIRE_GPU(); }
    };

    class TitleGenParityGpu : public TitleGenGpu {};

    bool presetRender(const PresetCase& c, GenImage& out, std::string& error, uint32_t flags = 0, const Json& overrides = Json::object()) {
        std::vector<TitleProblem> problems;
        if (!renderGenDocument(generatorPresetDocument(c.type, c.preset, overrides), GEN_SCALE, out, error, flags, &problems)) return false;
        for (const TitleProblem& p : problems) {
            if (p.code == "glyph_missing" || p.code == "asset_missing" || p.code == "geometry_unavailable" || p.code == "generator_unavailable") {
                error = p.code + ": " + p.message;
                return false;
            }
        }
        return true;
    }

}

TEST_F(TitleGenGpu, PresetGoldens) {
    const std::vector<PresetCase> cases = presetCases();
    ASSERT_FALSE(cases.empty());
    std::set<std::string> types;
    for (const PresetCase& c : cases) {
        SCOPED_TRACE(c.type + "/" + c.id);
        types.insert(c.type);
        GenImage out;
        std::string error;
        ASSERT_TRUE(presetRender(c, out, error)) << error;
        expectGenGolden(c.type + "_" + c.id, out);
    }
    // every S83 generator has presets (R7)
    for (const GeneratorDef* d : TitleEffectRegistry::get().generators()) {
        if (d->presetFile && std::string(d->type) != "pattern_fill") EXPECT_TRUE(types.count(d->type)) << d->type << " has no preset";
    }
}

// AC4's parity exceptions (at most 10, S83 §7), each with what makes it differ; the rest match
const std::map<std::string, const char*> PARITY_EXCEPTIONS = {
    {"speedLines-beta", "the Beta Flash preset's Halftone dissolve is a layer effect of the effects stream (S83 §6.6.9)"},
    {"glints-highlights", "the picks follow S80's base title, whose highlights differ from Pachislop's, read from the 8-bit grid (D8)"},
};

TEST_F(TitleGenParityGpu, PachislopPresets) {
    int compared = 0;
    for (const PresetCase& c : presetCases()) {
        if (c.source.empty()) continue;
        SCOPED_TRACE(c.type + "/" + c.id + " <- " + c.source);
        std::string refName = c.source;
        std::replace(refName.begin(), refName.end(), '/', '-');
        GenImage ref;
        const std::filesystem::path refPath = goldenFile("titleart/pachislop/generator/" + refName + ".png");
        ASSERT_TRUE(readPng(refPath.string(), ref.rgba, ref.width, ref.height)) << refPath.string();
        GenImage out;
        std::string error;
        ASSERT_TRUE(presetRender(c, out, error)) << error;
        // Pachislop's goldens are trimmed (alpha >= 8): ours the same, centred on theirs
        const GenImage ours = centredGen(trimGen(out, 8), ref.width, ref.height);
        const GenBlockStats stats = compareGenBlocks(ref, ours);
        const bool ok = stats.fractionOver <= 0.03 && stats.histogramDistance <= 6.0;
        if (!ok || isGoldenUpdateMode()) {
            const std::filesystem::path actual = std::filesystem::path(getOutputDirectory()) / ("titleart-gen-parity-" + refName + ".png");
            std::filesystem::create_directories(actual.parent_path());
            writePng(actual.string(), ours.rgba, ours.width, ours.height);
        }
        std::printf("[parity] %s: %s\n", refName.c_str(), stats.text().c_str());
        const auto exception = PARITY_EXCEPTIONS.find(refName);
        if (exception != PARITY_EXCEPTIONS.end()) {
            // an exception still has the reference's structure, within twice AC12's histogram limit
            EXPECT_LE(stats.histogramDistance, 12.0) << refName << " (" << exception->second << "): " << stats.text();
        } else {
            EXPECT_TRUE(ok) << refName << ": " << stats.text();
        }
        compared++;
    }
    EXPECT_GE(compared, 72) << "the ported presets of §6.8";
    EXPECT_LE(PARITY_EXCEPTIONS.size(), 10u);
    // red control: the metric tells particle fields apart (Gold Dust reseeded misses its reference)
    for (const PresetCase& c : presetCases()) {
        if (c.source != "particles/goldDust") continue;
        GenImage ref, out;
        std::string error;
        ASSERT_TRUE(readPng(goldenFile("titleart/pachislop/generator/particles-goldDust.png").string(), ref.rgba, ref.width, ref.height));
        ASSERT_TRUE(presetRender(c, out, error, 0, Json{{"seed", 8}})) << error;
        const GenBlockStats wrong = compareGenBlocks(ref, centredGen(trimGen(out, 8), ref.width, ref.height));
        EXPECT_TRUE(wrong.fractionOver > 0.03 || wrong.histogramDistance > 6.0) << "a reseeded field passed: " << wrong.text();
    }
}

TEST_F(TitleGenGpu, Deterministic) {
    for (const PresetCase& c : presetCases()) {
        SCOPED_TRACE(c.type + "/" + c.id);
        GenImage a, b, reseeded;
        std::string error;
        ASSERT_TRUE(presetRender(c, a, error)) << error;
        ASSERT_TRUE(presetRender(c, b, error)) << error;
        EXPECT_TRUE(a.rgba == b.rgba) << "two renders differ";
        // R15: the seed reshuffles what is random, where the preset draws something random: not
        // where the seed's `when` is off (Sunburst's plain rays), Magic Circle's presets without
        // arcs rows, or a Shockwave without fibres, breakup or smear
        const GeneratorDef* def = findGenerator(c.type);
        const ParamDef* seedDef = findParam(def->params, "seed");
        if (!seedDef || c.type == "magic_circle") continue;
        Json values = defaultParams(def->params);
        const Json presetParams = c.preset.value("params", Json::object());
        for (auto it = presetParams.begin(); it != presetParams.end(); ++it) values[it.key()] = it.value();
        if (seedDef->when.active()) {
            const std::string on = values.value(seedDef->when.param, Json()).is_string() ? values[seedDef->when.param].get<std::string>() : std::string();
            if (std::none_of(seedDef->when.values.begin(), seedDef->when.values.end(), [&](const char* v) { return on == v; })) continue;
        }
        if (c.type == "shockwave" && values.value("profile", std::string()) != "fibrous" && values.value("breakup", 0.0) <= 0.0 &&
            values.value("smear", 0.0) <= 0.0) {
            continue;
        }
        // Glints' peaks place stars by the analysis alone; the seed only phases a twinkle that is off
        if (c.type == "glints" && values.value("placement", std::string()) == "peaks" && values.value("twinkle", 0.0) <= 0.0) continue;
        const int seed = values.value("seed", 1);
        ASSERT_TRUE(presetRender(c, reseeded, error, 0, Json{{"seed", seed + 7}})) << error;
        EXPECT_FALSE(a.rgba == reseeded.rgba) << "another seed draws the same";
    }
}

TEST_F(TitleGenGpu, FallbackWithinClass) {
    for (uint32_t flags : {TITLE_ART_TEST_NO_FLOAT_TARGETS | TITLE_ART_TEST_NO_R16F | TITLE_ART_TEST_NO_RGBA32F, TITLE_ART_TEST_NO_FLOAT_BLEND}) {
        std::set<std::string> done;
        for (const PresetCase& c : presetCases()) {
            // each definition at its first preset (its default look)
            if (!done.insert(c.type).second) continue;
            SCOPED_TRACE(c.type + "/" + c.id + " flags " + std::to_string(flags));
            GenImage f, e;
            std::string error;
            ASSERT_TRUE(presetRender(c, f, error)) << error;
            ASSERT_TRUE(presetRender(c, e, error, flags)) << error;
            const GenPixelStats stats = compareGenImages(f, e);
            const FallbackClass cls = findGenerator(c.type)->fallback;
            const double maxMean = cls == FallbackClass::F0 ? 1.0 : cls == FallbackClass::F1 ? 1.5 : 3.0;
            const int maxDiff = cls == FallbackClass::F0 ? 48 : cls == FallbackClass::F1 ? 64 : 96;
            const double maxOver = cls == FallbackClass::F0 ? 0.005 : cls == FallbackClass::F1 ? 0.02 : 0.05;
            EXPECT_LE(stats.mean, maxMean) << fallbackClassName(cls) << ": " << stats.text();
            // the layouts read from the followed layer's 8-bit grid (Lightning's edge cells,
            // Backlight's shape centre) may flip a borderline cell between the paths: a few
            // pixels then differ by more than the class's max, the mean and the share hold
            const bool gridLayout = c.type == "lightning" || c.type == "backlight";
            if (!gridLayout) EXPECT_LE(stats.max, maxDiff) << fallbackClassName(cls) << ": " << stats.text();
            EXPECT_LE(stats.fractionOver8, maxOver) << fallbackClassName(cls) << ": " << stats.text();
            if (stats.mean > maxMean || (!gridLayout && stats.max > maxDiff) || stats.fractionOver8 > maxOver) {
                // both paths beside each other for the evidence
                const std::filesystem::path dir(getOutputDirectory());
                std::filesystem::create_directories(dir);
                writePng((dir / ("titleart-gen-fallback-" + c.type + "-float.png")).string(), f.rgba, f.width, f.height);
                writePng((dir / ("titleart-gen-fallback-" + c.type + "-rgba8-" + std::to_string(flags) + ".png")).string(), e.rgba, e.width, e.height);
            }
        }
    }
}

// ---- QuadBatch (AC13) ----

namespace {

    class QuadBatchGpu : public ::testing::Test {
    protected:
        void SetUp() override {
            JASPER_REQUIRE_GPU();
            kit = std::make_unique<TitlePassKit>(TitleArtCaps::select(SystemRender::getFeatures(), 0));
            const bool ran = GpuEnvironment::runFrames([this]() {
                return kit->prepare((TitleProgram)(uint8_t)gen::GenProgram::QuadsTest, gen::genProgramVariant(gen::GenProgram::QuadsTest, kit->caps())) ||
                       !kit->error().empty();
            });
            ASSERT_TRUE(ran && kit->error().empty()) << kit->error();
        }

        // quads drawn additively on a cleared w x h target whose drawing space is target px with +y up
        std::vector<float> drawQuads(const gen::QuadBatch& batch, int w, int h, float minPx = 1.5f) {
            TargetRef* target = kit->acquire(kit->accumulatorKey(w, h));
            EXPECT_TRUE(target);
            if (!target) return {};
            kit->clear(*target);
            GeneratorContext ctx(*kit);
            ctx.width = w;
            ctx.height = h;
            ctx.frame = {0, 0, w, h};
            ctx.canvasWidth = w;
            ctx.canvasHeight = h;
            TitleAffine space;
            space.d = -1.0;
            space.ty = h;
            ctx.documentToTarget = space;
            ctx.layerToTarget = space;
            EXPECT_TRUE(batch.draw(ctx, gen::GenProgram::QuadsTest, "quads", *target, space, gen::QuadBlend::Add, minPx)) << kit->error();
            std::vector<float> out = readFloats(*target, w, h);
            kit->release(target);
            return out;
        }

        std::vector<float> readFloats(TargetRef& target, int w, int h) {
            SystemRender::commit();
            std::vector<uint8_t> bytes;
            const ColorFormat format = target.key().format;
            if (!readTextureRaw(*target.get()->colorTexture(), w, h, format, bytes)) return {};
            std::vector<float> out((size_t)w * h * 4);
            for (size_t i = 0; i < out.size(); i++) {
                out[i] = format == ColorFormat::RGBA16F ? halfToFloat(reinterpret_cast<const uint16_t*>(bytes.data())[i])
                                                        : ((i % 4 == 3) ? bytes[i] / 255.0f : srgbToLinear(bytes[i] / 255.0f));
            }
            return out;
        }

        static gen::QuadInstance quad(float x, float y, float w, float h, float r, float g, float b) {
            gen::QuadInstance q;
            q.cx = x;
            q.cy = y;
            q.w = w;
            q.h = h;
            q.color[0] = r;
            q.color[1] = g;
            q.color[2] = b;
            q.color[3] = std::max(r, std::max(g, b));
            return q;
        }

        std::unique_ptr<TitlePassKit> kit;
    };

}

TEST_F(QuadBatchGpu, SubPixelEnergy) {
    if (!kit->caps().floatPath) GTEST_SKIP() << "the float path measures light sums";
    // 0.3 px quads grow to min_px with their colour scaled by the area ratio: over every sub-pixel
    // phase (16 x 16 of them, one quad per 4 x 4 cell) the summed light is area x colour, and no
    // quad vanishes between pixel centres
    auto drawPhases = [&](float minPx, double& total, int& vanished) {
        gen::QuadBatch batch;
        for (int j = 0; j < 16; j++) {
            for (int i = 0; i < 16; i++) batch.add(quad(4.0f * i + 1.5f + i / 16.0f, 4.0f * j + 1.5f + j / 16.0f, 0.3f, 0.3f, 1.0f, 0.5f, 0.25f));
        }
        const std::vector<float> px = drawQuads(batch, 64, 64, minPx);
        total = 0;
        vanished = 0;
        for (int cy = 0; cy < 16; cy++) {
            for (int cx = 0; cx < 16; cx++) {
                double cell = 0;
                for (int y = 0; y < 4; y++) {
                    for (int x = 0; x < 4; x++) cell += px[((size_t)(cy * 4 + y) * 64 + cx * 4 + x) * 4];
                }
                total += cell;
                if (cell <= 0.0) vanished++;
            }
        }
        return !px.empty();
    };
    double total = 0;
    int vanished = 0;
    ASSERT_TRUE(drawPhases(1.5f, total, vanished));
    EXPECT_NEAR(total, 256 * 0.09, 0.02 * 256 * 0.09);
    EXPECT_EQ(vanished, 0) << "a grown sub-pixel quad always reaches a pixel centre";
    // red control: without min_px the quads keep their size and most fall between pixel centres
    ASSERT_TRUE(drawPhases(0.0f, total, vanished));
    EXPECT_GT(vanished, 128) << "sub-pixel quads drawn at their own size all reached a pixel centre";
}

TEST_F(QuadBatchGpu, AnalyticCoverage) {
    if (!kit->caps().floatPath) GTEST_SKIP() << "the float path measures light sums";
    // an axis-aligned 8 x 4 quad on pixel edges covers exactly 32 pixels at its colour; a 2 x 2 one 4
    gen::QuadBatch batch;
    batch.add(quad(10.0f, 10.0f, 8.0f, 4.0f, 0.5f, 0.5f, 0.5f));
    batch.add(quad(40.0f, 40.0f, 2.0f, 2.0f, 1.0f, 0.0f, 0.0f));
    const std::vector<float> px = drawQuads(batch, 64, 64);
    ASSERT_FALSE(px.empty());
    double grey = 0, red = 0;
    int covered = 0;
    for (int y = 0; y < 64; y++) {
        for (int x = 0; x < 64; x++) {
            const float* p = &px[((size_t)y * 64 + x) * 4];
            if (x < 30 && y >= 34) grey += p[1];
            if (x >= 30 && y < 34) red += p[0];
            if (p[3] > 0.0f) covered++;
        }
    }
    EXPECT_NEAR(grey, 32 * 0.5, 0.05);
    EXPECT_NEAR(red, 4 * 1.0, 0.05);
    EXPECT_EQ(covered, 36);
}

TEST_F(QuadBatchGpu, SixtyFiveThousandInOneDraw) {
    gen::QuadBatch batch;
    for (int i = 0; i < gen::QUAD_BATCH_MAX; i++) batch.add(quad((float)(i % 256) + 0.5f, (float)(i / 256) + 0.5f, 1.0f, 1.0f, 0.25f, 0.25f, 0.25f));
    batch.add(quad(0.5f, 0.5f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f));    // past the cap: dropped
    EXPECT_EQ(batch.size(), (size_t)gen::QUAD_BATCH_MAX);
    const uint64_t before = kit->passes();
    // min_px 1 keeps the 1 px quads at their own size (each covers exactly its pixel)
    const std::vector<float> px = drawQuads(batch, 256, 256, 1.0f);
    ASSERT_FALSE(px.empty());
    EXPECT_EQ(kit->passes() - before, 1u) << "one draw";
    // every pixel got exactly its own quad (the last row of instances too)
    int lit = 0;
    for (size_t i = 0; i < px.size(); i += 4) {
        if (std::abs(px[i + 1] - 0.25f) < 0.01f) lit++;
    }
    EXPECT_EQ(lit, 256 * 256);
}
