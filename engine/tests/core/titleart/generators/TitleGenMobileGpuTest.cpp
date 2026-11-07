// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The generators' mobile tier (docs/specs/S83-title-effects-generators.md §6.7, D12, AC12): every
// generator with a mobile reduction, at its first preset as TitleGenGpu draws it (the standard
// title followed where the definition must follow, 1920 x 1080 at 0.25):
// - TitleGenMobileGpu.AgreesWithDesktop: the mobile render against the desktop one. Each
//   reduction is held to what it can keep by construction (MOBILE_CASES):
//   - Particles and Fractal Noise keep a subset of the desktop work: the mobile render equals
//     the desktop render of the reduced params (count 2,000; complexity 4) within F1, and
//     Fractal Noise's dropped fifth octave (3 % of the value range) stays within F1 of the
//     full desktop image. Particles' cap drops 600 of Gold Dust's 2,600 particles, which no
//     image tolerance would call the same picture, so the full image is not compared.
//   - Lens Flare's 8 quads leave out the starburst and the bloom's tail past 6 sizes (§6.7.9):
//     the starburst's spikes near the core exceed F1's share of pixels over 8/255, so it is
//     held to F2 (the definition's own fallback class, a light-above-1 look).
//   - Backlight (rays at quarter resolution), Light Streaks (halos at quarter resolution) and
//     Aura (half resolution, 3-octave fbm) change detail finer than their reduced pixels, so
//     they are held to the structure metric S80 AC12 and S83 AC4 use for approximations (4 x 4
//     block means over 24/255 in <= 3 % of blocks, histogram distance <= 6 %).
//   - Fire and Smoke take their noise from the tiling texture, a different random field from
//     the desktop value noise, so their tongues and billows sit elsewhere by construction:
//     they are held to AC4's histogram distance (the same colours in the same amounts) and the
//     baked field itself to F1 (FireMobileGpu.BakedFieldAgrees).
//   These tolerances were chosen from the reductions' construction, not from a measured run;
//   every case prints its F1 and block statistics so the first run records them.
//   Red control: every mobile render differs from the desktop one (the tier reaches the
//   definitions; before S83's tier plumbing both were the desktop path).
// - TitleGenMobile.EstimateWithinClass: estimate(first preset, 1024 x 512, Mobile) is within the
//   definition's declared class, and Lens Flare's within C0 (its desktop pass is not: control).
// - TitleGenMobileGpu.PassesWithinEstimate: run on a pass kit with the title as the followed
//   layer, the mobile path draws at most the passes its mobile estimate states (the shared
//   distance field and the analysis grids are their own rows of the model, §6.4).
// - FireMobileGpu.BakedFieldAgrees (AC12, mobile half): on the "L" of FireGpu, the upstream
//   field baked at half resolution, read bilinearly at full resolution, gives the flame's base
//   coverage (turbulence 0, before the noise) within F1 of the full-resolution march. Red
//   control: a field baked at another spread does not.

#include <gtest/gtest.h>

#include "render/SystemRender.h"
#include "render/TextureRender.h"
#include "texture/TexturePrepare.h"
#include "titleart/generators/TitleGen.h"
#include "titleart/generators/TitleGenMobile.h"
#include "titleart/render/TitleArtCaps.h"
#include "titleart/render/TitleEffectContext.h"
#include "titleart/render/TitlePassKit.h"

#include "../support/TitleGenTestSupport.h"

#include "GpuEnvironment.h"
#include "Readback.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <memory>

using namespace jasperengine;
using namespace jasperengine::titleart;
using namespace jasperengine::titleart::test;
using namespace jasperengine::test;

namespace {

    constexpr float GEN_SCALE = 0.25f;
    // the preset documents' canvas and the targets a 0.25 render of it makes
    constexpr int CANVAS_W = 1920, CANVAS_H = 1080;
    constexpr int TARGET_W = 480, TARGET_H = 270;

    enum class Agreement : uint8_t {
        Subset,     // equals the desktop render of `reduced` params within F1
        F1,         // S83 §6.5's F1 limits against the desktop render
        F2,         // F2's limits
        Structure,  // S80 AC12's block metric (S83 AC4)
        Palette     // AC4's histogram distance alone
    };

    struct MobileCase {
        const char* type;
        std::vector<Agreement> checks;
        Json reduced;       // Subset: the desktop params the mobile path is
    };

    const std::vector<MobileCase>& mobileCases() {
        static const std::vector<MobileCase> cases = {
            {"particles", {Agreement::Subset}, Json{{"count", 2000}}},
            {"fractal_noise", {Agreement::Subset, Agreement::F1}, Json{{"complexity", 4}}},
            {"lens_flare", {Agreement::F2}, Json::object()},
            {"backlight", {Agreement::Structure}, Json::object()},
            {"light_streaks", {Agreement::Structure}, Json::object()},
            {"aura", {Agreement::Structure}, Json::object()},
            {"fire", {Agreement::Palette}, Json::object()},
            {"smoke", {Agreement::Palette}, Json::object()},
        };
        return cases;
    }

    Json firstPreset(const std::string& type) {
        const Json presets = loadGeneratorPresets(type);
        return presets.empty() ? Json::object() : presets[0];
    }

    // the definition's defaults with the preset's params over them, normalized
    Json presetValues(const GeneratorDef& def, const Json& preset) {
        Json values = defaultParams(def.params);
        const Json params = preset.value("params", Json::object());
        for (auto it = params.begin(); it != params.end(); ++it) values[it.key()] = it.value();
        NormalizeContext normalize;
        normalizeParams(def.params, values, "params", normalize);
        return values;
    }

    bool renderPreset(const std::string& type, const Json& preset, const Json& overrides, FxTier tier, GenImage& out, std::string& error) {
        std::vector<TitleProblem> problems;
        if (!renderGenDocument(generatorPresetDocument(type, preset, overrides), GEN_SCALE, out, error, 0, &problems, tier)) return false;
        for (const TitleProblem& p : problems) {
            if (p.code == "glyph_missing" || p.code == "asset_missing" || p.code == "generator_unavailable") {
                error = p.code + ": " + p.message;
                return false;
            }
        }
        return true;
    }

    struct Limits {
        double mean;
        int max;
        double over8;
    };
    constexpr Limits F1_LIMITS{1.5, 64, 0.02};
    constexpr Limits F2_LIMITS{3.0, 96, 0.05};

    bool within(const GenPixelStats& s, const Limits& l) {
        return s.mean <= l.mean && s.max <= l.max && s.fractionOver8 <= l.over8;
    }

    // §6.4's static limits per class (C3 is not bounded by taps, C4 by nothing)
    bool withinClass(const FxCost& c, CostClass cls) {
        switch (cls) {
            case CostClass::C0: return c.fre <= 1.0 && c.maxTapsPerPixel <= 16;
            case CostClass::C1: return c.fre <= 3.0 && c.maxTapsPerPixel <= 64;
            case CostClass::C2: return c.fre <= 10.0 && c.maxTapsPerPixel <= 200;
            case CostClass::C3: return c.fre <= 40.0;
            case CostClass::C4: return true;
        }
        return false;
    }

    void writeEvidence(const std::string& name, const GenImage& image) {
        const std::filesystem::path dir(getOutputDirectory());
        std::filesystem::create_directories(dir);
        writePng((dir / ("titleart-gen-mobile-" + name + ".png")).string(), image.rgba, image.width, image.height);
    }

    // Runs passes on a kit and counts those a definition's estimate counts: not the shared
    // distance field's (seeds, flood, resolve, bounds) nor the analysis grids (§6.4's own rows)
    class CountingRunner : public TitlePassRunner {
    public:
        explicit CountingRunner(TitlePassKit& passes) : kit(passes) {}
        const TitleArtCaps& caps() const override { return kit.caps(); }
        TargetRef* acquire(const TargetKey& key) override { return kit.acquire(key); }
        void release(TargetRef* target) override { kit.release(target); }
        const TitleLut* lut(const TitleLutData& data) override { return kit.lut(data); }
        bool run(const TitlePass& pass) override {
            const bool ran = kit.run(pass);
            if (ran && counts(pass)) counted++;
            return ran;
        }
        bool clear(TargetRef& target) override { return kit.clear(target); }
        bool skippedPasses() const override { return kit.skippedPasses(); }
        const std::string& error() const override { return kit.error(); }
        void clearError() override { kit.clearError(); }

        int counted = 0;

    private:
        static bool counts(const TitlePass& pass) {
            if (pass.variant & TITLE_VARIANT_GENERATOR) return pass.program != (TitleProgram)(uint8_t)gen::GenProgram::Grid;
            return pass.program != TitleProgram::Seed && pass.program != TitleProgram::Jfa && pass.program != TitleProgram::Distance &&
                   pass.program != TitleProgram::Bounds;
        }

        TitlePassKit& kit;
    };

    // a kit, premultiplied linear uploads into its accumulators and reads of its targets
    class MobileKitGpu : public ::testing::Test {
    protected:
        void SetUp() override {
            JASPER_REQUIRE_GPU();
            kit = std::make_unique<TitlePassKit>(TitleArtCaps::select(SystemRender::getFeatures(), 0));
        }

        void TearDown() override {
            kit.reset();
            uploads.clear();
        }

        // runs `draw` once the programs it uses are compiled (a pass whose program is still
        // compiling fails with "being compiled"; the next frame tries again)
        bool whenCompiled(const std::function<void()>& draw) {
            bool failed = false;
            const bool ran = GpuEnvironment::runFrames([&]() {
                kit->clearError();
                draw();
                const std::string& e = kit->error();
                if (e.find("being compiled") != std::string::npos) return false;
                failed = !e.empty();
                return true;
            });
            return ran && !failed;
        }

        // premultiplied linear rgba (rows top first) in an accumulator target
        TargetRef* upload(const std::vector<float>& rgba, int w, int h) {
            std::vector<uint16_t> halves(rgba.size());
            for (size_t i = 0; i < halves.size(); i++) halves[i] = floatToHalf(rgba[i]);
            auto texture = std::make_unique<TextureRender>();
            void* data[1] = {halves.data()};
            size_t size[1] = {halves.size() * sizeof(uint16_t)};
            if (!texture->createTexture("TitleGenMobile input", w, h, ColorFormat::RGBA16F, TextureType::TEXTURE_2D, 1, data, size, TextureFilter::NEAREST,
                                        TextureFilter::NEAREST, TextureWrap::CLAMP_TO_EDGE, TextureWrap::CLAMP_TO_EDGE)) {
                return nullptr;
            }
            TargetRef* target = kit->acquire(kit->accumulatorKey(w, h));
            if (!target) return nullptr;
            TitlePass pass;
            pass.program = TitleProgram::Copy;
            pass.label = "upload";
            pass.textures[0].texture = texture.get();
            pass.output = target;
            pass.set(2, 1.0f);
            if (!whenCompiled([&]() { kit->run(pass); })) return nullptr;
            uploads.push_back(std::move(texture));
            return target;
        }

        // a target of the Distance format holding values 0..1 (TitleGenMobile.h's upstream
        // field: R16F, or 16-bit fixed point in RG), rows top first
        std::vector<float> readUnitField(TargetRef& target, int w, int h) {
            SystemRender::commit();
            std::vector<uint8_t> bytes;
            const ColorFormat format = target.key().format;
            if (!readTextureRaw(*titleTargetTexture(target), w, h, format, bytes)) return {};
            std::vector<float> out((size_t)w * h);
            for (size_t i = 0; i < out.size(); i++) {
                if (format == ColorFormat::R16F) out[i] = halfToFloat(reinterpret_cast<const uint16_t*>(bytes.data())[i]);
                else out[i] = (bytes[i * 4] * 256.0f + bytes[i * 4 + 1]) / 65535.0f;
            }
            return out;
        }

        GeneratorContext context(TitlePassRunner& runner, int w, int h, float pxScale, const TitleAffine& documentToTarget) {
            GeneratorContext ctx(runner);
            ctx.width = w;
            ctx.height = h;
            ctx.pxScale = pxScale;
            ctx.frame = {0, 0, w, h};
            ctx.documentToTarget = documentToTarget;
            ctx.layerToTarget = documentToTarget;
            ctx.layerId = "00000301";
            return ctx;
        }

        std::unique_ptr<TitlePassKit> kit;
        std::vector<std::unique_ptr<TextureRender>> uploads;
    };

    class TitleGenMobileGpu : public MobileKitGpu {};
    class FireMobileGpu : public MobileKitGpu {};

}

TEST_F(TitleGenMobileGpu, AgreesWithDesktop) {
    for (const MobileCase& c : mobileCases()) {
        const Json preset = firstPreset(c.type);
        const std::string name = std::string(c.type) + "/" + preset.value("id", std::string());
        SCOPED_TRACE(name);
        GenImage desktop, mobile;
        std::string error;
        ASSERT_TRUE(renderPreset(c.type, preset, Json::object(), FxTier::Desktop, desktop, error)) << error;
        ASSERT_TRUE(renderPreset(c.type, preset, Json::object(), FxTier::Mobile, mobile, error)) << error;
        const GenPixelStats stats = compareGenImages(desktop, mobile);
        const GenBlockStats blocks = compareGenBlocks(desktop, mobile);
        std::printf("[mobile] %s: %s; %s\n", name.c_str(), stats.text().c_str(), blocks.text().c_str());
        // red control: the mobile tier reaches the definition
        EXPECT_FALSE(desktop.rgba == mobile.rgba) << "the mobile render is the desktop one";
        bool ok = true;
        for (Agreement a : c.checks) {
            switch (a) {
                case Agreement::Subset: {
                    GenImage subset;
                    ASSERT_TRUE(renderPreset(c.type, preset, c.reduced, FxTier::Desktop, subset, error)) << error;
                    const GenPixelStats s = compareGenImages(subset, mobile);
                    EXPECT_TRUE(within(s, F1_LIMITS)) << "against the desktop render of " << c.reduced.dump() << ": " << s.text();
                    ok = ok && within(s, F1_LIMITS);
                    break;
                }
                case Agreement::F1:
                    EXPECT_TRUE(within(stats, F1_LIMITS)) << "F1: " << stats.text();
                    ok = ok && within(stats, F1_LIMITS);
                    break;
                case Agreement::F2:
                    EXPECT_TRUE(within(stats, F2_LIMITS)) << "F2: " << stats.text();
                    ok = ok && within(stats, F2_LIMITS);
                    break;
                case Agreement::Structure:
                    EXPECT_LE(blocks.fractionOver, 0.03) << blocks.text();
                    EXPECT_LE(blocks.histogramDistance, 6.0) << blocks.text();
                    ok = ok && blocks.fractionOver <= 0.03 && blocks.histogramDistance <= 6.0;
                    break;
                case Agreement::Palette:
                    EXPECT_LE(blocks.histogramDistance, 6.0) << blocks.text();
                    ok = ok && blocks.histogramDistance <= 6.0;
                    break;
            }
        }
        if (!ok) {
            writeEvidence(std::string(c.type) + "-desktop", desktop);
            writeEvidence(std::string(c.type) + "-mobile", mobile);
        }
    }
}

TEST(TitleGenMobile, EstimateWithinClass) {
    const FxSize mobileSize{1024, 512, 1024.0f / 1920.0f};
    for (const MobileCase& c : mobileCases()) {
        SCOPED_TRACE(c.type);
        const GeneratorDef* def = findGenerator(c.type);
        ASSERT_TRUE(def && def->estimate);
        const Json values = presetValues(*def, firstPreset(c.type));
        const ParamValues p(values);
        const FxCost m = def->estimate(p, mobileSize, FxTier::Mobile);
        EXPECT_TRUE(withinClass(m, def->costClass)) << m.fre << " FRE, " << m.maxTapsPerPixel << " taps over " << costClassName(def->costClass);
        EXPECT_GE(m.passes, 1);
        if (std::string(c.type) == "lens_flare") {
            EXPECT_TRUE(withinClass(m, CostClass::C0)) << "the mobile flare: " << m.fre << " FRE, " << m.maxTapsPerPixel << " taps";
            // control: the desktop flare's full-canvas pass is not C0
            EXPECT_FALSE(withinClass(def->estimate(p, mobileSize, FxTier::Desktop), CostClass::C0));
        }
    }
}

TEST_F(TitleGenMobileGpu, PassesWithinEstimate) {
    // the followed layer: the standard title as the preset documents draw it (no effects)
    GenImage title;
    std::string error;
    ASSERT_TRUE(renderGenDocument(standardTitleDocument(), GEN_SCALE, title, error)) << error;
    ASSERT_EQ(title.width, TARGET_W);
    ASSERT_EQ(title.height, TARGET_H);
    std::vector<float> premul(title.rgba.size());
    for (size_t i = 0; i < premul.size(); i += 4) {
        const float a = title.rgba[i + 3] / 255.0f;
        for (int k = 0; k < 3; k++) premul[i + k] = srgbToLinear(title.rgba[i + k] / 255.0f) * a;
        premul[i + 3] = a;
    }
    TargetRef* follow = upload(premul, TARGET_W, TARGET_H);
    ASSERT_TRUE(follow) << kit->error();
    // canvas px (centre origin, +y up) -> the 0.25 render's target px
    TitleAffine documentToTarget;
    documentToTarget.a = GEN_SCALE;
    documentToTarget.d = -GEN_SCALE;
    documentToTarget.tx = TARGET_W * 0.5;
    documentToTarget.ty = TARGET_H * 0.5;
    CountingRunner runner(*kit);
    for (const MobileCase& c : mobileCases()) {
        SCOPED_TRACE(c.type);
        const GeneratorDef* def = findGenerator(c.type);
        ASSERT_TRUE(def && def->render && def->estimate);
        const Json values = presetValues(*def, firstPreset(c.type));
        const ParamValues p(values, &def->globalLight);
        GeneratorContext ctx = context(runner, TARGET_W, TARGET_H, GEN_SCALE, documentToTarget);
        ctx.canvasWidth = CANVAS_W;
        ctx.canvasHeight = CANVAS_H;
        ctx.tier = FxTier::Mobile;
        if (def->follow == FollowMode::Required) {
            ctx.follow = follow;
            ctx.followContent = follow;
            // the title's pixels for the analysis caches (any key no renderer makes)
            ctx.followKey = 0x6d6f62696c650001ull;
            ctx.followContentKey = 0x6d6f62696c650002ull;
        }
        TargetRef* target = kit->acquire(kit->accumulatorKey(TARGET_W, TARGET_H));
        ASSERT_TRUE(target) << kit->error();
        ASSERT_TRUE(whenCompiled([&]() {
            runner.counted = 0;
            def->render(ctx, *target, p);
        })) << kit->error();
        kit->release(target);
        const FxCost estimate = def->estimate(p, FxSize{TARGET_W, TARGET_H, GEN_SCALE}, FxTier::Mobile);
        std::printf("[mobile passes] %s: %d run, %d estimated\n", c.type, runner.counted, estimate.passes);
        EXPECT_GE(runner.counted, 1) << "the mobile path drew nothing";
        EXPECT_LE(runner.counted, estimate.passes);
    }
    kit->release(follow);
}

// ---- Fire's baked field (AC12) ----

namespace {

    // FireGpu's "L": a stem x 100..140, rows 200..360 and a foot x 100..300, rows 320..360
    // (target px); every edge on an even pixel, so the half-resolution shape is exact
    constexpr int L_SIZE = 400;
    constexpr int STEM_X0 = 100, STEM_X1 = 140, STEM_TOP = 200, FOOT_X1 = 300, FOOT_TOP = 320, L_BOTTOM = 360;

    std::vector<float> lShape() {
        std::vector<float> rgba((size_t)L_SIZE * L_SIZE * 4, 0.0f);
        for (int y = 0; y < L_SIZE; y++) {
            for (int x = 0; x < L_SIZE; x++) {
                const bool stem = x >= STEM_X0 && x < STEM_X1 && y >= STEM_TOP && y < L_BOTTOM;
                const bool foot = x >= STEM_X0 && x < FOOT_X1 && y >= FOOT_TOP && y < L_BOTTOM;
                if (stem || foot) std::fill_n(&rgba[((size_t)y * L_SIZE + x) * 4], 4, 1.0f);
            }
        }
        return rgba;
    }

    // a w x h field at (x, y) in its own px, bilinear between pixel centres and clamped at the
    // edges (gen/energy_mobile.glsl em_upstream)
    float bilinearAt(const std::vector<float>& field, int w, int h, double x, double y) {
        x = std::clamp(x - 0.5, 0.0, (double)(w - 1));
        y = std::clamp(y - 0.5, 0.0, (double)(h - 1));
        const int x0 = (int)std::floor(x), y0 = (int)std::floor(y);
        const int x1 = std::min(x0 + 1, w - 1), y1 = std::min(y0 + 1, h - 1);
        const double fx = x - x0, fy = y - y0;
        auto at = [&](int i, int j) { return (double)field[(size_t)j * w + i]; };
        const double top = at(x0, y0) + (at(x1, y0) - at(x0, y0)) * fx;
        const double bottom = at(x0, y1) + (at(x1, y1) - at(x0, y1)) * fx;
        return (float)(top + (bottom - top) * fy);
    }

    // the flame's base coverage at turbulence 0 before the flicker (fire.glsl): every tongue
    // `height` long, E = field x 1.6 x height
    double baseCoverage(double field) {
        const double f = std::clamp(1.0 - field * 1.6, 0.0, 1.0);
        const double t = std::clamp(f / 0.35, 0.0, 1.0);
        return t * t * (3.0 - 2.0 * t);
    }

}

TEST_F(FireMobileGpu, BakedFieldAgrees) {
    TargetRef* shape = upload(lShape(), L_SIZE, L_SIZE);
    ASSERT_TRUE(shape) << kit->error();
    // canvas px (centre origin, +y up) -> target px, at px_scale 1
    TitleAffine documentToTarget;
    documentToTarget.d = -1.0;
    documentToTarget.tx = L_SIZE * 0.5;
    documentToTarget.ty = L_SIZE * 0.5;
    GeneratorContext full = context(*kit, L_SIZE, L_SIZE, 1.0f, documentToTarget);
    full.canvasWidth = L_SIZE;
    full.canvasHeight = L_SIZE;
    constexpr double HEIGHT = 150.0;
    const TitleVec2 up{0.0, 1.0};   // Fire's default direction, -90 (y up)

    // the field marched at `ctx`'s resolution from the shape at that resolution, as the mobile
    // fire bakes it (side weight 1 / spread), read back
    auto bake = [&](GeneratorContext& ctx, double spread, int& w, int& h) {
        std::vector<float> out;
        EXPECT_TRUE(whenCompiled([&]() {
            out.clear();
            EffectContext e = gen::effectContext(ctx);
            fx::Scratch source;
            const TargetRef* content = shape;
            if (ctx.width != L_SIZE) {
                source = fx::scratch(e);
                if (!source || !fx::copy(e, *shape, *source)) return;
                content = source.get();
            }
            fx::Scratch field = fx::distanceField(e, *content, 0.5f, (float)(1.6 * HEIGHT * ctx.pxScale + 2.0));
            if (!field) return;
            fx::Scratch baked = gen::upstreamField(ctx, *field, up, 1.6 * HEIGHT * gen::spaceScale(ctx.documentToTarget), 1.0 / std::max(0.08, spread));
            if (!baked) return;
            out = readUnitField(*baked, ctx.width, ctx.height);
        })) << kit->error();
        w = ctx.width;
        h = ctx.height;
        return out;
    };
    int fw = 0, fh = 0, hw = 0, hh = 0;
    const std::vector<float> reference = bake(full, 0.35, fw, fh);
    GeneratorContext half = gen::reducedContext(full, 2);
    const std::vector<float> baked = bake(half, 0.35, hw, hh);
    const std::vector<float> otherSpread = bake(half, 1.0, hw, hh);
    ASSERT_EQ(reference.size(), (size_t)L_SIZE * L_SIZE);
    ASSERT_EQ(baked.size(), (size_t)hw * hh);
    ASSERT_EQ(otherSpread.size(), baked.size());

    // the base coverage of both as 8-bit alpha, the half-resolution field read at each full
    // pixel's centre (the reduced target's uv lines up with the full one's)
    auto compare = [&](const std::vector<float>& field) {
        GenPixelStats s;
        double sum = 0.0;
        size_t over = 0;
        const double sx = (double)hw / L_SIZE, sy = (double)hh / L_SIZE;
        for (int y = 0; y < L_SIZE; y++) {
            for (int x = 0; x < L_SIZE; x++) {
                const double a = baseCoverage(reference[(size_t)y * L_SIZE + x]) * 255.0;
                const double b = baseCoverage(bilinearAt(field, hw, hh, (x + 0.5) * sx, (y + 0.5) * sy)) * 255.0;
                const double d = std::fabs(std::round(a) - std::round(b));
                sum += d;
                s.max = std::max(s.max, (int)d);
                if (d > 8.0) over++;
            }
        }
        s.mean = sum / ((double)L_SIZE * L_SIZE);
        s.fractionOver8 = (double)over / ((double)L_SIZE * L_SIZE);
        return s;
    };
    const GenPixelStats agree = compare(baked);
    std::printf("[mobile] fire baked field: %s\n", agree.text().c_str());
    EXPECT_TRUE(within(agree, F1_LIMITS)) << "F1: " << agree.text();
    // red control: the comparison sees the field (another spread burns the sides elsewhere)
    const GenPixelStats wrong = compare(otherSpread);
    EXPECT_FALSE(within(wrong, F1_LIMITS)) << "a field of another spread passed: " << wrong.text();
    kit->release(shape);
}
