// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Energy generators' GPU behaviour (docs/specs/S83-title-effects-generators.md §6.7.11,
// §6.7.14, §7), each pass run as the renderer runs it, around a TitlePassKit:
// - LightningGpu.MaxBlendNoDoubling (AC9, R21): where two bolt segments cross, the core is no
//   brighter than the brighter segment alone (+ 1/255), anywhere on the target; the red control
//   draws the same quads additively and exceeds it at the crossing.
// - FireGpu.FollowsSilhouette (AC12, R24): on an "L" the flame above the top edge reaches at
//   least 0.9 x `height` (and no further than `height`, turbulence off so every tongue is that
//   long); at `spread` 0 the flame beside the vertical edge stays within the side burn's floor,
//   and at the default 35 % it reaches well past it (the control).
//   The side burn weighs sideways distance by 1 / max(0.08, spread) (Pachislop's fire.ts, ported
//   exactly), so at spread 0 the side flame reaches 8 % of the height before its alpha ramp:
//   AC12's "< 5 %" cannot hold for the exact port, and the check uses that 8 % floor.

#include <gtest/gtest.h>

#include "render/SystemRender.h"
#include "render/TextureRender.h"
#include "texture/TexturePrepare.h"
#include "titleart/TitleArtDoc.h"
#include "titleart/TitleArtIO.h"
#include "titleart/TitleEffectRegistry.h"
#include "titleart/analysis/BoltBuilder.h"
#include "titleart/generators/TitleGen.h"
#include "titleart/gpu/QuadBatch.h"
#include "titleart/render/TitleArtCaps.h"
#include "titleart/render/TitleEffectContext.h"
#include "titleart/render/TitlePassKit.h"

#include "GpuEnvironment.h"
#include "Readback.h"

#include <algorithm>
#include <cmath>
#include <memory>

using namespace jasperengine;
using namespace jasperengine::titleart;
using namespace jasperengine::test;

namespace {

    // a kit with the programs these checks run, and its targets as floats
    class EnergyGpu : public ::testing::Test {
    protected:
        void SetUp() override {
            JASPER_REQUIRE_GPU();
            kit = std::make_unique<TitlePassKit>(TitleArtCaps::select(SystemRender::getFeatures(), 0));
            const bool ran = GpuEnvironment::runFrames([this]() {
                if (!kit->error().empty()) return true;
                bool ready = true;
                for (TitleProgram program : {TitleProgram::Copy, TitleProgram::Seed, TitleProgram::Jfa, TitleProgram::Distance}) {
                    ready = kit->prepare(program, titleProgramVariant(program, kit->caps())) && ready;
                }
                for (gen::GenProgram program : {gen::GenProgram::Fire, gen::GenProgram::QuadsLightning}) {
                    ready = kit->prepare((TitleProgram)(uint8_t)program, gen::genProgramVariant(program, kit->caps())) && ready;
                }
                return ready;
            });
            ASSERT_TRUE(ran && kit->error().empty()) << kit->error();
        }

        void TearDown() override {
            kit.reset();
            uploads.clear();
        }

        // a context over a w x h target whose document space is `documentToTarget`, the
        // generator layer unmoved from the followed one
        GeneratorContext context(int w, int h, const TitleAffine& documentToTarget) {
            GeneratorContext ctx(*kit);
            ctx.width = w;
            ctx.height = h;
            ctx.pxScale = 1.0f;
            ctx.frame = {0, 0, w, h};
            ctx.canvasWidth = w;
            ctx.canvasHeight = h;
            ctx.documentToTarget = documentToTarget;
            ctx.layerToTarget = documentToTarget;
            ctx.layerId = "00000002";
            return ctx;
        }

        // premultiplied linear rgba (rows top first) in an accumulator target
        TargetRef* upload(const std::vector<float>& rgba, int w, int h) {
            std::vector<uint16_t> halves(rgba.size());
            for (size_t i = 0; i < halves.size(); i++) halves[i] = floatToHalf(rgba[i]);
            auto texture = std::make_unique<TextureRender>();
            void* data[1] = {halves.data()};
            size_t size[1] = {halves.size() * sizeof(uint16_t)};
            if (!texture->createTexture("EnergyGpu input", w, h, ColorFormat::RGBA16F, TextureType::TEXTURE_2D, 1, data, size, TextureFilter::NEAREST,
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
            if (!kit->run(pass)) return nullptr;
            uploads.push_back(std::move(texture));
            return target;
        }

        // the target's linear premultiplied rgba (rows top first)
        std::vector<float> readFloats(TargetRef& target, int w, int h) {
            SystemRender::commit();
            std::vector<uint8_t> bytes;
            const ColorFormat format = target.key().format;
            if (!readTextureRaw(*titleTargetTexture(target), w, h, format, bytes)) return {};
            std::vector<float> out((size_t)w * h * 4);
            for (size_t i = 0; i < out.size(); i++) {
                out[i] = format == ColorFormat::RGBA16F ? halfToFloat(reinterpret_cast<const uint16_t*>(bytes.data())[i])
                                                        : ((i % 4 == 3) ? bytes[i] / 255.0f : srgbToLinear(bytes[i] / 255.0f));
            }
            return out;
        }

        std::unique_ptr<TitlePassKit> kit;
        std::vector<std::unique_ptr<TextureRender>> uploads;
    };

    class LightningGpu : public EnergyGpu {};
    class FireGpu : public EnergyGpu {};

}

// ---- Lightning (AC9) ----

TEST_F(LightningGpu, MaxBlendNoDoubling) {
    constexpr int W = 64, H = 64;
    // the drawing space is target px with +y up (Pachislop's render px)
    TitleAffine space;
    space.d = -1.0;
    space.ty = H;
    GeneratorContext ctx = context(W, H, space);
    // two segments crossing at (32, 32), each at intensity 0.6 (so the additive control exceeds
    // it on the RGBA8 path too), at the default core width
    const double coreWidth = 3.5 * 0.5;
    analysis::BoltSegment across{12.0, 32.0, 52.0, 32.0, 1.0, 0.6}, down{32.0, 12.0, 32.0, 52.0, 1.0, 0.6};
    auto cores = [&](std::initializer_list<analysis::BoltSegment> segs, gen::QuadBlend blend) {
        gen::QuadBatch batch;
        for (const analysis::BoltSegment& s : segs) {
            analysis::BoltQuad q;
            EXPECT_TRUE(analysis::boltQuad(s, coreWidth, q));
            gen::QuadInstance quad;
            quad.cx = (float)q.cx;
            quad.cy = (float)q.cy;
            quad.w = (float)q.length;
            quad.h = (float)(2.0 * q.halfWidth);
            quad.rotation = (float)q.rotation;
            for (float& c : quad.color) c = (float)s.intensity;
            batch.add(quad);
        }
        TargetRef* target = kit->acquire(kit->accumulatorKey(W, H));
        EXPECT_TRUE(target) << kit->error();
        if (!target) return std::vector<float>();
        kit->clear(*target);
        EXPECT_TRUE(batch.draw(ctx, gen::GenProgram::QuadsLightning, "lightning cores", *target, space, blend, 0.0f)) << kit->error();
        std::vector<float> px = readFloats(*target, W, H);
        kit->release(target);
        return px;
    };
    const std::vector<float> both = cores({across, down}, gen::QuadBlend::Max);
    const std::vector<float> a = cores({across}, gen::QuadBlend::Max);
    const std::vector<float> b = cores({down}, gen::QuadBlend::Max);
    const std::vector<float> added = cores({across, down}, gen::QuadBlend::Add);
    ASSERT_EQ(both.size(), (size_t)W * H * 4);
    ASSERT_EQ(a.size(), both.size());
    ASSERT_EQ(b.size(), both.size());
    ASSERT_EQ(added.size(), both.size());

    // the crossing (32, 32) in the y-up space: the pixel around target (32, 32) where both
    // segments are brightest together
    size_t crossing = 0;
    float together = -1.0f;
    for (int y = 30; y <= 33; y++) {
        for (int x = 30; x <= 33; x++) {
            const size_t i = ((size_t)y * W + x) * 4;
            if (std::min(a[i], b[i]) > together) {
                together = std::min(a[i], b[i]);
                crossing = i;
            }
        }
    }
    ASSERT_GT(std::min(a[crossing], b[crossing]), 0.4f) << "the segments do not both reach the crossing";
    const float tolerance = 1.0f / 255.0f;
    for (size_t i = 0; i < both.size(); i += 4) {
        const float single = std::max(a[i], b[i]);
        ASSERT_LE(both[i], single + tolerance) << "the core doubles at pixel " << (i / 4) % W << ", " << (i / 4) / W;
    }
    // red control: additive cores exceed the brighter segment where they cross
    EXPECT_GT(added[crossing], std::max(a[crossing], b[crossing]) + tolerance) << "additive blending did not double the crossing";
}

// ---- Fire (AC12) ----

namespace {

    // the "L": a stem x 100..140, rows 200..360 and a foot x 100..300, rows 320..360 (target px)
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

}

TEST_F(FireGpu, FollowsSilhouette) {
    const GeneratorDef* def = findGenerator("fire");
    ASSERT_TRUE(def && def->render);
    // canvas px (centre origin, +y up) -> target px, at px_scale 1
    TitleAffine documentToTarget;
    documentToTarget.d = -1.0;
    documentToTarget.tx = L_SIZE * 0.5;
    documentToTarget.ty = L_SIZE * 0.5;
    TargetRef* shape = upload(lShape(), L_SIZE, L_SIZE);
    ASSERT_TRUE(shape) << kit->error();
    constexpr double HEIGHT = 150.0;

    // the fire's alpha at `spread`, turbulence off (every tongue `height` long, no sway)
    auto flames = [&](double spread) {
        Json params{{"height", HEIGHT}, {"turbulence", 0}, {"spread", spread}};
        NormalizeContext normalize;
        normalizeParams(def->params, params, "params", normalize);
        GeneratorContext ctx = context(L_SIZE, L_SIZE, documentToTarget);
        ctx.follow = shape;
        ctx.followContent = shape;
        TargetRef* target = kit->acquire(kit->accumulatorKey(L_SIZE, L_SIZE));
        EXPECT_TRUE(target) << kit->error();
        if (!target) return std::vector<float>();
        kit->clearError();
        def->render(ctx, *target, ParamValues(params, &def->globalLight));
        EXPECT_TRUE(kit->error().empty()) << kit->error();
        std::vector<float> rgba = readFloats(*target, L_SIZE, L_SIZE);
        kit->release(target);
        std::vector<float> alpha(rgba.size() / 4);
        for (size_t i = 0; i < alpha.size(); i++) alpha[i] = rgba[i * 4 + 3];
        return alpha;
    };
    auto at = [](const std::vector<float>& alpha, int x, int y) { return alpha[(size_t)y * L_SIZE + x]; };
    // how far above the stem's top the flame shows, at its centre column
    auto reachAbove = [&](const std::vector<float>& alpha) {
        const int x = (STEM_X0 + STEM_X1) / 2;
        int top = STEM_TOP;
        for (int y = STEM_TOP - 1; y >= 0; y--) {
            if (at(alpha, x, y) > 0.01f) top = y;
        }
        return STEM_TOP - top;
    };
    // the farthest pixel centre left of the stem with alpha >= 0.5, over the middle of the stem
    // (far from its top and from the foot)
    auto reachBeside = [&](const std::vector<float>& alpha) {
        double reach = 0.0;
        for (int y = STEM_TOP + 40; y < FOOT_TOP - 40; y++) {
            for (int x = 0; x < STEM_X0; x++) {
                if (at(alpha, x, y) >= 0.5f) {
                    reach = std::max(reach, STEM_X0 - (x + 0.5));
                    break;
                }
            }
        }
        return reach;
    };

    const std::vector<float> side0 = flames(0.0);
    const std::vector<float> side35 = flames(35.0);
    ASSERT_EQ(side0.size(), (size_t)L_SIZE * L_SIZE);
    ASSERT_EQ(side35.size(), side0.size());

    // above the top edge: the flames rise to their height, and stop there
    for (const std::vector<float>* alpha : {&side0, &side35}) {
        const int above = reachAbove(*alpha);
        EXPECT_GE(above, 0.9 * HEIGHT) << "the flame above the top edge reaches " << above << " px of " << HEIGHT;
        EXPECT_LE(above, HEIGHT + 2.0) << "the flame rises past its height";
    }
    // beside the vertical edge: at spread 0 within the side weighting's floor (1 / 0.08), at
    // 35 % far past it (the control: the side burn follows `spread`)
    const double beside0 = reachBeside(side0), beside35 = reachBeside(side35);
    EXPECT_LT(beside0, 0.08 * HEIGHT) << "at spread 0 the side flame reaches " << beside0 << " px";
    EXPECT_GT(beside35, 0.2 * HEIGHT) << "at spread 35 the side flame reaches only " << beside35 << " px";
    kit->release(shape);
}
