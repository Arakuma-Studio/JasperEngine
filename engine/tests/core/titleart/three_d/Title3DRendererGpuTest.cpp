// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S86 AC9, AC10 (partly), AC13 (CPU twin), AC15, AC27 (zero cost): a Dimension 3D layer through
// the S80 renderer on the headless device. A gold 3D plate draws lit pixels where its 2D twin
// does and nothing outside its region; the ramp's bend changes the image (red control: bend 0);
// the compressed-light path agrees with the float path, and Gold 3D "BIG" with its glint, and "BIG"
// with a gem face and AO, render without a float target on every tier within AC9
// (Title3DFallbackGpu; red control: the float targets the forced path took before); the debug views; AO (AC16) darkens the
// crevices and keeps open faces; gems refract the layer's own opaque parts (AC11); a part's own
// environment lights that part only; the glint (AC17) adds light past the letters on every tier
// (red control: a threshold nothing passes); an environment build is spread over frames (the 21 s
// software-rasteriser stall of one-shot builds; red control: one shot); a 2D document makes no
// 3D renderer; five environments in one layer build once each (red control: the cache evicted
// them and the builds went round); the environment's level map and CPU scene evaluation.

#include "titleart/render/TitleRenderTestSupport.h"
#include "titleart/support/TitleTestAssets.h"

#include "titleart/render/TitleRenderServices.h"
#include "titleart/three_d/Title3DMath.h"
#include "titleart/three_d/Title3DRenderer.h"
#include "titleart/three_d/TitleEnvironment.h"
#include "titleart/three_d/TitleMeshFile.h"
#include "titleart/three_d/Title3DSource.h"
#include "titleart/three_d/mesher/TitleMesher.h"

#include <cmath>
#include <cstdio>

using namespace jasperengine;
using namespace jasperengine::titleart;
using namespace jasperengine::titleart::test;

namespace {

    // a 3D rect plate: gold face with a round bevel, a dark band behind
    Json plate3D(double bend = 0.6, const char* preset = "gold") {
        Json layer = shapeLayer("00000001", "rect", 120, solidFace("#ff0000"), Json{{"aspect", 2.5}, {"dimension", "3d"}});
        Json face = materialPreset(preset);
        face["gradient_bend"] = bend;
        layer["style_3d"] = Json{{"face", face}, {"bevel", {{"width", 12}, {"depth", 6}, {"profile", "round"}}},
                                 {"bands", Json::array({{{"grow", 3}, {"material", materialPreset("black_gloss")}}})}};
        return layer;
    }

    std::shared_ptr<const TitleArtDoc> doc3D(const Json& layer) {
        return makeDoc({{"canvas", canvas(512, 256)}, {"layers", Json::array({layer})}, {"scene_3d", {{"supersample", 1}}}});
    }

    std::unique_ptr<TitleArtRenderer> geometryRenderer() {
        auto renderer = std::make_unique<TitleArtRenderer>();
        renderer->setContentProvider(makeGeometryTitleContentProvider());
        return renderer;
    }

    RenderRequest requestOf(std::shared_ptr<const TitleArtDoc> doc, uint32_t flags = 0) {
        RenderRequest r;
        r.doc = std::move(doc);
        r.pxScale = 1.0f;
        r.testFlags = flags;
        return r;
    }

    // the 3D renderer of a TitleArtRenderer (its counters)
    const Title3DStats* stats3D(const TitleArtRenderer& renderer) {
        const std::shared_ptr<void> r = renderer.title3DRendererForTesting();
        return r ? &static_cast<const Title3DRenderer*>(r.get())->stats() : nullptr;
    }

    // the linear luminance of an sRGB-encoded pixel (AO multiplies linear light)
    double luminance(const float* p) {
        return 0.2126 * srgbToLinear(p[0]) + 0.7152 * srgbToLinear(p[1]) + 0.0722 * srgbToLinear(p[2]);
    }

    class Title3DRendererGpu : public ::testing::Test {
    protected:
        void SetUp() override { JASPER_REQUIRE_GPU(); }
    };

    class Title3DFallbackGpu : public Title3DRendererGpu {};

    // AC9's fixture: Gold 3D "BIG" (S84's metal/gold-3d structure, as the bake bench builds it),
    // with S83's stock star glint or none
    std::shared_ptr<const TitleArtDoc> goldBig(bool glint) {
        Json gold = materialPreset("gold");
        Json bands = Json::array();
        bands.push_back(Json{{"grow", 1.5}, {"depth", 10}, {"bevel", {{"width", 1.2}, {"depth", 1.2}, {"profile", "chamfer"}}}, {"material", gold}});
        bands.push_back(Json{{"grow", 0.6}, {"depth", 8}, {"bevel", {{"width", 0.3}, {"depth", 0.6}, {"profile", "step"}}},
                             {"material", materialPreset("black_gloss")}});
        bands.push_back(Json{{"grow", 2.2}, {"depth", 6}, {"plate", {{"kind", "hull"}}}, {"material", materialPreset("black_gloss")}});
        Json style = Json{{"depth", 12}, {"curve_segments", 10}, {"bevel", {{"width", 2.5}, {"depth", 2.5}, {"profile", "round"}}},
                          {"face", gold}, {"bands", bands}, {"glint", {{"enabled", glint}}}};
        Json layer = Json{{"id", "00000001"}, {"type", "text"}, {"name", "BIG"}, {"text", "BIG"}, {"font", fontRef(DELA_GOTHIC)},
                          {"size", 200}, {"dimension", "3d"}, {"style_3d", style}};
        return makeDoc({{"canvas", canvas(768, 384)}, {"layers", Json::array({layer})}});
    }

    // "BIG" with a sapphire face (a gem: the behind copy refracts the gold bands) and AO on or off
    std::shared_ptr<const TitleArtDoc> gemBig(bool ao = true) {
        Json gold = materialPreset("gold");
        Json bands = Json::array();
        bands.push_back(Json{{"grow", 2.5}, {"depth", 10}, {"bevel", {{"width", 1.2}, {"depth", 1.2}, {"profile", "chamfer"}}}, {"material", gold}});
        bands.push_back(Json{{"grow", 2.2}, {"depth", 6}, {"plate", {{"kind", "hull"}}}, {"material", materialPreset("black_gloss")}});
        Json style = Json{{"depth", 12}, {"curve_segments", 10}, {"bevel", {{"width", 2.5}, {"depth", 2.5}, {"profile", "round"}}},
                          {"face", materialPreset("sapphire")}, {"bands", bands}, {"ao", {{"enabled", ao}, {"radius", 6}, {"intensity", 1.5}}}};
        Json layer = Json{{"id", "00000001"}, {"type", "text"}, {"name", "BIG"}, {"text", "BIG"}, {"font", fontRef(DELA_GOTHIC)},
                          {"size", 200}, {"dimension", "3d"}, {"style_3d", style}};
        return makeDoc({{"canvas", canvas(768, 384)}, {"layers", Json::array({layer})}});
    }

    // AC9's forced path: every float format off
    constexpr uint32_t FORCED_RGBA8 = TITLE_ART_TEST_NO_FLOAT_TARGETS | TITLE_ART_TEST_NO_R16F | TITLE_ART_TEST_NO_RGBA32F;

    RenderRequest fallbackRequest(std::shared_ptr<const TitleArtDoc> doc, int tier, uint32_t flags) {
        static TestAssetResolver assets;
        RenderRequest r = requestOf(std::move(doc), flags);
        r.tier = tier;
        r.assets = &assets;
        return r;
    }

    // a renderer that only rendered forced: no float target from the pool, none made beside it,
    // its environments built on the CPU twin
    void expectNoFloatTargets(const TitleArtRenderer& renderer) {
        const Title3DStats* st = stats3D(renderer);
        ASSERT_TRUE(st);
        EXPECT_EQ(renderer.poolStats().floatAcquires, 0u) << "the forced render acquired a float target";
        EXPECT_EQ(st->floatRenderTargets, 0u) << "the forced render made a float render target";
        EXPECT_EQ(st->environmentBuilds, 0u) << "no environment built on the GPU";
        EXPECT_GT(st->environmentCpuBuilds, 0u) << "the environment built on the CPU twin";
    }

}

TEST_F(Title3DRendererGpu, AGold3DPlateDrawsLitPixels) {
    auto renderer = geometryRenderer();
    std::unique_ptr<TitleRenderJob> job;
    const Pixels p = render(*renderer, requestOf(doc3D(plate3D())), &job);
    ASSERT_EQ(p.width, 512);
    ASSERT_EQ(p.height, 256);
    for (const TitleProblem& problem : job->problems()) ADD_FAILURE() << problem.code << ": " << problem.message;
    // the plate's centre is opaque and lit (gold: red above blue); a corner stays transparent
    const float* centre = p.at(256, 128);
    EXPECT_GT(centre[3], 0.99f);
    EXPECT_GT(centre[0] + centre[1] + centre[2], 0.1f);
    EXPECT_GT(centre[0], centre[2]);
    EXPECT_LT(p.at(2, 2)[3], 1.0f / 255.0f);
}

TEST_F(Title3DRendererGpu, TheBendChangesTheRampRedControl) {
    auto renderer = geometryRenderer();
    const Pixels bent = render(*renderer, requestOf(doc3D(plate3D(0.6))));
    const Pixels flat = render(*renderer, requestOf(doc3D(plate3D(0.0))));
    // among the plate's pixels, the bevel's share changes by more than 8 / 255
    ASSERT_EQ(bent.rgba.size(), flat.rgba.size());
    size_t covered = 0, changed = 0;
    for (size_t i = 0; i + 3 < bent.rgba.size(); i += 4) {
        if (bent.rgba[i + 3] <= 0 && flat.rgba[i + 3] <= 0) continue;
        covered++;
        float worst = 0;
        for (int c = 0; c < 4; c++) worst = std::max(worst, std::abs(bent.rgba[i + c] - flat.rgba[i + c]));
        if (worst > 8.0f / 255.0f) changed++;
    }
    ASSERT_GT(covered, 0u);
    EXPECT_GT((double)changed / covered, 0.02) << "gradient_bend 0 changes the bevels' ramp";
}

TEST_F(Title3DRendererGpu, CompressedLightAgreesWithFloat) {
    if (!SystemRender::getFeatures().renderRGBA16F || !SystemRender::getFeatures().msaaRGBA16F) GTEST_SKIP() << "no float MSAA here";
    auto renderer = geometryRenderer();
    const Pixels floats = render(*renderer, requestOf(doc3D(plate3D())));
    const Pixels compressed = render(*renderer, requestOf(doc3D(plate3D()), TITLE_ART_TEST_NO_FLOAT_TARGETS));
    const Difference d = compare(floats, compressed, 24.0);
    EXPECT_LE(d.mean, 3.0);
    EXPECT_LE(d.overFraction, 0.02);
}

TEST_F(Title3DFallbackGpu, TheGlintRunsWithoutFloatTargetsOnEveryTier) {
    // AC9, R30, R32: Gold 3D "BIG" with the glint on, at Bake and High (the full chain), Medium (the
    // reduction) and Low (the sprites), with float targets and with TITLE_ART_TEST_NO_FLOAT_TARGETS
    // (each on its own renderer): the forced render acquires no float target and makes no float
    // render target (its environment built on the CPU twin and only sampled), runs the glint's
    // passes (as many as the float render), shows its glint (it differs from the forced render
    // without one) and agrees with the float render within mean 3/255. Red control: the glint's
    // RGBA16F targets before it had its 8-bit path (the forced render acquired them)
    const RenderFeatures& f = SystemRender::getFeatures();
    if (!f.renderRGBA16F || !f.filterRGBA16F || !f.blendRGBA16F || !f.msaaRGBA16F) GTEST_SKIP() << "no float passes here";
    for (int tier : {0, 1, 2, 3}) {
        SCOPED_TRACE("tier " + std::to_string(tier));
        auto floatRenderer = geometryRenderer();
        auto forcedRenderer = geometryRenderer();
        const Pixels floats = render(*floatRenderer, fallbackRequest(goldBig(true), tier, 0));
        const Pixels fallback = render(*forcedRenderer, fallbackRequest(goldBig(true), tier, FORCED_RGBA8));
        const Pixels fallbackOff = render(*forcedRenderer, fallbackRequest(goldBig(false), tier, FORCED_RGBA8));
        const Title3DStats* fs = stats3D(*floatRenderer);
        const Title3DStats* st = stats3D(*forcedRenderer);
        ASSERT_TRUE(fs && st);
        EXPECT_GT(fs->glintPasses, 0u) << "the glint ran";
        EXPECT_GT(floatRenderer->poolStats().floatAcquires, 0u) << "the float render takes float targets";
        expectNoFloatTargets(*forcedRenderer);
        EXPECT_EQ(st->glintPasses, fs->glintPasses) << "the forced glint runs the same passes";
        const Difference d = compare(floats, fallback, 24.0);
        const Difference shown = compare(fallback, fallbackOff, 24.0);
        std::printf("[title3d-fallback] tier %d: glint on, float vs forced mean %.3f/255, %.4f %% over 24/255, max %.0f; glint shown on %.3f %% of px\n",
                    tier, d.mean, d.overFraction * 100, d.max, shown.overFraction * 100);
        EXPECT_LE(d.mean, 3.0);
        EXPECT_GT(shown.overFraction, 0.0) << "the forced render shows the glint";
    }
}

TEST_F(Title3DFallbackGpu, AOAndGemsRenderWithoutFloatTargets) {
    // AC9, R21, R30, R31: "BIG" with a sapphire face over gold bands and AO on, glint off, at Bake and
    // High (where AO and the gem behind copy run) and Medium and Low: the forced render acquires no
    // float target and makes no float render target (the AO prepass packed into RGBA8, the behind
    // copy in the compressed-light encoding, the environment from the CPU twin) and agrees with the
    // float render within AC9's glint-off criteria (mean 2/255, at most 1 % of px over 24/255). Red
    // control: the AO targets, the behind copy and the environment cube in RGBA16F under the flag
    const RenderFeatures& f = SystemRender::getFeatures();
    if (!f.renderRGBA16F || !f.filterRGBA16F || !f.blendRGBA16F || !f.msaaRGBA16F) GTEST_SKIP() << "no float passes here";
    for (int tier : {0, 1, 2, 3}) {
        SCOPED_TRACE("tier " + std::to_string(tier));
        auto floatRenderer = geometryRenderer();
        auto forcedRenderer = geometryRenderer();
        const Pixels floats = render(*floatRenderer, fallbackRequest(gemBig(), tier, 0));
        const Pixels fallback = render(*forcedRenderer, fallbackRequest(gemBig(), tier, FORCED_RGBA8));
        const Title3DStats* fs = stats3D(*floatRenderer);
        const Title3DStats* st = stats3D(*forcedRenderer);
        ASSERT_TRUE(fs && st);
        if (tier <= 1) {
            EXPECT_GT(fs->aoPasses, 0u);
            EXPECT_GT(fs->behindCopies, 0u);
            EXPECT_EQ(st->aoPasses, fs->aoPasses) << "the forced render runs the AO";
            EXPECT_EQ(st->behindCopies, fs->behindCopies) << "the forced render makes the behind copy";
        }
        expectNoFloatTargets(*forcedRenderer);
        if (tier <= 1) {
            // both show on the forced path: the gem without its behind copy, the title without AO, differ
            const Pixels noBehind = render(*forcedRenderer, fallbackRequest(gemBig(), tier, FORCED_RGBA8 | TITLE_ART_TEST_NO_BEHIND));
            const Pixels noAO = render(*forcedRenderer, fallbackRequest(gemBig(false), tier, FORCED_RGBA8));
            EXPECT_GT(compare(fallback, noBehind, 8.0).overFraction, 0.01) << "the forced gem refracts the bands";
            EXPECT_GT(compare(fallback, noAO, 8.0).overFraction, 0.001) << "the forced AO darkens the crevices";
        }
        const Difference d = compare(floats, fallback, 24.0);
        std::printf("[title3d-fallback] tier %d: AO and gems, float vs forced mean %.3f/255, %.4f %% over 24/255, max %.0f\n", tier, d.mean,
                    d.overFraction * 100, d.max);
        EXPECT_LE(d.mean, 2.0);
        EXPECT_LE(d.overFraction, 0.01);
    }
}

TEST_F(Title3DRendererGpu, The3DRegionIsTheProjectedMesh) {
    // a small plate in a large canvas: the pixels outside its projected bounds are untouched
    auto renderer = geometryRenderer();
    Json layer = plate3D();
    layer["size"] = 40;
    layer["transform"] = {{"position", Json::array({-150, 60})}};
    const Pixels p = render(*renderer, requestOf(doc3D(layer)));
    // the layer sits left of centre and above it: the right half stays empty
    double rightAlpha = 0;
    for (int y = 0; y < p.height; y++) {
        for (int x = 300; x < p.width; x++) rightAlpha += p.at(x, y)[3];
    }
    EXPECT_EQ(rightAlpha, 0.0);
    EXPECT_GT(p.at(256 - 150, 128 - 60)[3], 0.99f);
}

TEST_F(Title3DRendererGpu, DebugViewsShowNormalsAndTheEnvironment) {
    // render_title options.debug_view: the face's normal is +z, so the normals view is blue with
    // equal red and green where the gold render is red over blue
    auto renderer = geometryRenderer();
    RenderRequest normals = requestOf(doc3D(plate3D()));
    normals.debugView3D = 1;
    const Pixels n = render(*renderer, normals);
    const float* centre = n.at(256, 128);
    EXPECT_GT(centre[3], 0.99f);
    EXPECT_GT(centre[2], centre[0] + 0.1f);
    EXPECT_NEAR(centre[0], centre[1], 0.02f);
    RenderRequest environment = requestOf(doc3D(plate3D()));
    environment.debugView3D = 2;
    const Pixels e = render(*renderer, environment);
    const Pixels lit = render(*renderer, requestOf(doc3D(plate3D())));
    EXPECT_GT(e.at(256, 128)[3], 0.99f);
    EXPECT_GT(compare(e, lit, 8.0).overFraction, 0.05) << "the environment view is not the render";
}

TEST_F(Title3DRendererGpu, AODarkensTheCrevicesAndKeepsOpenFaces) {
    // AC16: where the face meets the band behind it the AO darkens by 15 % or more; the flat
    // middle of the face (open) keeps its value; intensity 0 changes nothing
    auto renderer = geometryRenderer();
    // a bright band grown well past the letter: the crevice at the letter's foot is lit
    auto litPlate = []() {
        Json layer = plate3D();
        layer["style_3d"]["bands"][0]["grow"] = 20;
        layer["style_3d"]["bands"][0]["material"] = materialPreset("white_gloss");
        // linear output below white, so the occlusion shows as it is multiplied in
        layer["style_3d"]["lighting"] = Json{{"tone_mapping", "none"}, {"exposure", 0.3}};
        return layer;
    };
    auto withAO = [&](double intensity) {
        Json layer = litPlate();
        layer["style_3d"]["ao"] = Json{{"enabled", true}, {"radius", 8}, {"intensity", intensity}};
        return doc3D(layer);
    };
    const Pixels off = render(*renderer, requestOf(doc3D(litPlate())));
    const Pixels on = render(*renderer, requestOf(withAO(2)));
    const Pixels zero = render(*renderer, requestOf(withAO(0)));
    ASSERT_EQ(off.rgba.size(), on.rgba.size());
    size_t covered = 0, darkened = 0;
    for (size_t i = 0; i + 3 < off.rgba.size(); i += 4) {
        if (off.rgba[i + 3] < 0.99f) continue;
        covered++;
        const double a = luminance(&off.rgba[i]), b = luminance(&on.rgba[i]);
        if (a > 0.05 && b < a * 0.95) darkened++;
    }
    ASSERT_GT(covered, 0u);
    // (the occlusion itself, 15 % or more at the foot, is the AO debug view's check below)
    EXPECT_GT(darkened, covered / 200) << darkened << " of " << covered << " pixels darkened by 5 %";
    const float* c0 = off.at(256, 128);
    const float* c1 = on.at(256, 128);
    EXPECT_NEAR(luminance(c1), luminance(c0), 0.01 * std::max(0.05, luminance(c0))) << "the open face keeps its value";
    EXPECT_LE(compare(off, zero, 1.0).overFraction, 0.0) << "intensity 0 is no AO";
    const Title3DStats* st = stats3D(*renderer);
    ASSERT_TRUE(st);
    EXPECT_GE(st->aoPasses, 4u);
}

TEST_F(Title3DRendererGpu, GemsRefractTheLayersOwnOpaqueParts) {
    // AC11: a sapphire face over its dark band: with the behind copy the face shows the band
    // refracted, without it (red control) the environment only
    auto renderer = geometryRenderer();
    const Pixels seen = render(*renderer, requestOf(doc3D(plate3D(0.0, "sapphire"))));
    const Pixels envOnly = render(*renderer, requestOf(doc3D(plate3D(0.0, "sapphire")), TITLE_ART_TEST_NO_BEHIND));
    ASSERT_EQ(seen.rgba.size(), envOnly.rgba.size());
    size_t covered = 0, changed = 0;
    for (size_t i = 0; i + 3 < seen.rgba.size(); i += 4) {
        if (seen.rgba[i + 3] <= 0 && envOnly.rgba[i + 3] <= 0) continue;
        covered++;
        float worst = 0;
        for (int c = 0; c < 4; c++) worst = std::max(worst, std::abs(seen.rgba[i + c] - envOnly.rgba[i + c]));
        if (worst > 8.0f / 255.0f) changed++;
    }
    ASSERT_GT(covered, 0u);
    EXPECT_GT((double)changed / covered, 0.10) << "the band behind the gem shows through it";
    const Title3DStats* st = stats3D(*renderer);
    ASSERT_TRUE(st);
    EXPECT_GE(st->behindCopies, 1u);
}

TEST_F(Title3DRendererGpu, APartsOwnEnvironmentLightsThatPartOnly) {
    // a face material tinting its environment red: the face reddens, the band keeps its colour
    auto renderer = geometryRenderer();
    // the band grown well past the letter's bevel, so its rim shows
    Json base = plate3D(0.0, "chrome");
    base["style_3d"]["bands"][0]["grow"] = 20;
    Json tinted = base;
    tinted["style_3d"]["face"]["environment"] = Json{{"tint", "#ff2020"}};
    const Pixels plain = render(*renderer, requestOf(doc3D(base)));
    const Pixels red = render(*renderer, requestOf(doc3D(tinted)));
    const float* a = plain.at(256, 128);
    const float* b = red.at(256, 128);
    EXPECT_GT(b[0] - b[2], a[0] - a[2] + 0.05f) << "the face lights with its own tinted environment";
    // the band's rim, left of the face (the plate is 300 x 120 px centred)
    int bandX = -1;
    for (int x = 40; x < 256; x++) {
        if (plain.at(x, 128)[3] > 0.99f) {
            bandX = x + 1;
            break;
        }
    }
    ASSERT_GE(bandX, 0);
    for (int c = 0; c < 3; c++) EXPECT_NEAR(red.at(bandX, 128)[c], plain.at(bandX, 128)[c], 2.0f / 255.0f) << "the band keeps the layer's environment";
}

TEST_F(Title3DRendererGpu, MoreEnvironmentsThanTheCacheKeepsBuildOnce) {
    // five cubes in one layer (the cache keeps three of 1024): each built once, none evicted
    // while the render needs it (red control: evicted, the builds went round forever)
    auto renderer = geometryRenderer();
    Json layer = plate3D(0.0, "chrome");
    layer["style_3d"]["face"]["environment"] = Json{{"preset", "neon"}};
    Json bands = Json::array();
    const char* const presets[] = {"strips", "backlit", "under"};
    for (int i = 0; i < 3; i++) {
        Json material = materialPreset("chrome");
        material["environment"] = Json{{"preset", presets[i]}};
        bands.push_back(Json{{"grow", 3 + 4 * i}, {"material", material}});
    }
    layer["style_3d"]["bands"] = bands;
    const Pixels image = render(*renderer, requestOf(doc3D(layer)));
    ASSERT_GT(image.width, 0);
    const Title3DStats* stats = stats3D(*renderer);
    ASSERT_NE(stats, nullptr);
    EXPECT_EQ(stats->environmentBuilds, 5u) << "one build per cube";
    EXPECT_GT(image.at(256, 128)[3], 0.99f) << "the plate drew";
}

TEST_F(Title3DRendererGpu, DebugViewsAoInkAndWireframe) {
    auto renderer = geometryRenderer();
    Json layer = plate3D();
    layer["style_3d"]["bands"][0]["grow"] = 20;
    layer["style_3d"]["ao"] = Json{{"enabled", true}, {"radius", 8}, {"intensity", 2}};
    RenderRequest ao = requestOf(doc3D(layer));
    ao.debugView3D = (int)Title3DDebugView::AO;
    const Pixels aoView = render(*renderer, ao);
    EXPECT_GT(aoView.at(256, 128)[0], 0.95f) << "the open face is unoccluded";
    // the view is the occlusion (linear), written sRGB-encoded: the foot darkens by 15 % or more
    float darkest = 1;
    for (size_t i = 0; i + 3 < aoView.rgba.size(); i += 4) {
        if (aoView.rgba[i + 3] > 0.99f) darkest = std::min(darkest, srgbToLinear(aoView.rgba[i]));
    }
    EXPECT_LT(darkest, 0.85f) << "the crevices show";
    // ink: the render without glint (no glint here: the render itself)
    RenderRequest ink = requestOf(doc3D(plate3D()));
    ink.debugView3D = (int)Title3DDebugView::Ink;
    EXPECT_LE(compare(render(*renderer, ink), render(*renderer, requestOf(doc3D(plate3D()))), 2.0).overFraction, 0.001);
    // wireframe: lines over nothing, fewer covered pixels than the solid render
    RenderRequest wire = requestOf(doc3D(plate3D()));
    wire.debugView3D = (int)Title3DDebugView::Wireframe;
    const Pixels w = render(*renderer, wire);
    const Pixels solid = render(*renderer, requestOf(doc3D(plate3D())));
    size_t wireCovered = 0, solidCovered = 0;
    for (size_t i = 0; i + 3 < w.rgba.size(); i += 4) {
        wireCovered += w.rgba[i + 3] > 0.5f;
        solidCovered += solid.rgba[i + 3] > 0.5f;
    }
    EXPECT_GT(wireCovered, 0u);
    EXPECT_LT(wireCovered, solidCovered / 2);
}

TEST_F(Title3DRendererGpu, TheGlintAddsLightPastTheLetters) {
    // AC17: a gold plate's bevel highlights make a glint that reaches past the plate (alpha raised
    // where the plain render is empty), on the full chain (Bake), the reduction (Medium) and the
    // sprites (Low); a threshold nothing passes changes nothing (red control)
    auto withGlint = [](double threshold) {
        Json layer = plate3D(0.6, "chrome");
        layer["style_3d"]["glint"] = Json{{"enabled", true}, {"threshold", threshold}, {"intensity", 2}, {"radius", 5},
                                          {"streaks", 4}, {"length", 40}, {"angle", 15}};
        return doc3D(layer);
    };
    for (int tier : {0, 2, 3}) {
        auto renderer = geometryRenderer();
        RenderRequest plainRequest = requestOf(doc3D(plate3D(0.6, "chrome")));
        plainRequest.tier = tier;
        RenderRequest glintRequest = requestOf(withGlint(0.5));
        glintRequest.tier = tier;
        RenderRequest darkRequest = requestOf(withGlint(1000));
        darkRequest.tier = tier;
        const Pixels plain = render(*renderer, plainRequest);
        const Pixels glint = render(*renderer, glintRequest);
        const Pixels dark = render(*renderer, darkRequest);
        ASSERT_EQ(plain.rgba.size(), glint.rgba.size());
        size_t outside = 0, brighter = 0;
        for (size_t i = 0; i + 3 < plain.rgba.size(); i += 4) {
            if (plain.rgba[i + 3] <= 0.0f && glint.rgba[i + 3] > 2.0f / 255.0f) outside++;
            if (glint.rgba[i] + glint.rgba[i + 1] + glint.rgba[i + 2] > plain.rgba[i] + plain.rgba[i + 1] + plain.rgba[i + 2] + 6.0f / 255.0f) brighter++;
        }
        EXPECT_GT(brighter, 0u) << "tier " << tier << ": the glint adds light";
        EXPECT_GT(outside, 0u) << "tier " << tier << ": the glint reaches past the letters";
        EXPECT_LE(compare(plain, dark, 1.0).overFraction, 0.0) << "tier " << tier << ": nothing passes the threshold";
        const Title3DStats* st = stats3D(*renderer);
        ASSERT_TRUE(st);
        EXPECT_GT(st->glintPasses, 0u);
    }
    // the debug view shows the glint alone
    auto renderer = geometryRenderer();
    RenderRequest view = requestOf(withGlint(0.5));
    view.debugView3D = (int)Title3DDebugView::Glint;
    const Pixels g = render(*renderer, view);
    double light = 0;
    for (size_t i = 0; i + 3 < g.rgba.size(); i += 4) light += g.rgba[i + 3];
    EXPECT_GT(light, 0.0);
}

TEST_F(Title3DRendererGpu, PatternsPrintAndRaiseTheFace) {
    // R22: a checker printed on the face changes it (marks in the pattern's colours); a
    // relief-only pattern (no ink) still bends the face's normal and changes its light
    auto renderer = geometryRenderer();
    auto withPattern = [](double opacity, double relief) {
        Json layer = plate3D(0.0, "chrome");
        layer["style_3d"]["face"]["pattern"] = Json{{"kind", "checker"}, {"color", "#ff0000"}, {"color2", "#0000ff"}, {"opacity", opacity},
                                                    {"scale", 20}, {"shade", 0.5}, {"relief", relief}};
        return doc3D(layer);
    };
    const Pixels plain = render(*renderer, requestOf(doc3D(plate3D(0.0, "chrome"))));
    const Pixels printed = render(*renderer, requestOf(withPattern(1, 0)));
    const Pixels raised = render(*renderer, requestOf(withPattern(0, 1)));
    auto changedShare = [&](const Pixels& a, const Pixels& b) {
        size_t covered = 0, changed = 0;
        for (size_t i = 0; i + 3 < a.rgba.size(); i += 4) {
            if (a.rgba[i + 3] < 0.99f) continue;
            covered++;
            float worst = 0;
            for (int c = 0; c < 3; c++) worst = std::max(worst, std::abs(a.rgba[i + c] - b.rgba[i + c]));
            if (worst > 8.0f / 255.0f) changed++;
        }
        return covered ? (double)changed / covered : 0.0;
    };
    EXPECT_GT(changedShare(plain, printed), 0.2) << "the marks show";
    EXPECT_GT(changedShare(plain, raised), 0.01) << "the relief bends the light";
    // the marks are red and blue: the face holds both hues
    size_t red = 0, blue = 0;
    for (size_t i = 0; i + 3 < printed.rgba.size(); i += 4) {
        if (printed.rgba[i + 3] < 0.99f) continue;
        red += printed.rgba[i] > printed.rgba[i + 2] + 0.15f;
        blue += printed.rgba[i + 2] > printed.rgba[i] + 0.15f;
    }
    EXPECT_GT(red, 0u);
    EXPECT_GT(blue, 0u);
}

TEST_F(Title3DRendererGpu, ACookedMeshDrawsWithoutTheMesher) {
    // R38, AC26: the layer's mesh written to .titlemesh and read back draws the same image in a
    // render that has no mesher (a player built without it); red control: without the cooked
    // mesh that render has nothing to draw and says why
    auto renderer = geometryRenderer();
    const Json layer = plate3D();
    const std::shared_ptr<const TitleArtDoc> doc = doc3D(layer);
    const Pixels meshed = render(*renderer, requestOf(doc));
    three_d::TitleMeshRequest r;
    auto content = makeGeometryTitleContentProvider();
    r.source = three_d::collectTitleSource(doc->layers()[0], *doc, *content, nullptr, nullptr);
    r.style = decodeStyle3D(doc->layers()[0]);
    r.blocks = three_d::collectCharBlocks(doc->layers()[0], r.style.bands.size());
    r.tier = TitleTier::Bake;
    r.outputScale = 1.0;
    three_d::TitleMesher mesher;
    const std::vector<uint8_t> file = writeTitleMesh(*mesher.build(r));
    std::string error;
    std::shared_ptr<TitleMesh> cooked = readTitleMesh(file, &error);
    ASSERT_TRUE(cooked) << error;
    RenderRequest playerRequest = requestOf(doc, TITLE_ART_TEST_NO_MESHER);
    playerRequest.cookedMeshes3D[doc->layers()[0].value("id", std::string())] = cooked;
    const Pixels fromFile = render(*renderer, playerRequest);
    const Difference d = compare(meshed, fromFile, 2.0);
    EXPECT_LE(d.mean, 0.5);
    EXPECT_LE(d.overFraction, 0.0);
    // red control: no cooked mesh, no mesher
    std::unique_ptr<TitleRenderJob> job;
    const Pixels nothing = render(*renderer, requestOf(doc, TITLE_ART_TEST_NO_MESHER), &job);
    bool reported = false;
    for (const TitleProblem& p : job->problems()) reported = reported || p.code == "mesher_unavailable";
    EXPECT_TRUE(reported);
    EXPECT_LT(nothing.at(256, 128)[3], 1.0f / 255.0f);
}

TEST_F(Title3DRendererGpu, ACookedEnvironmentLightsLikeTheBuiltOne) {
    // R26, AC13: the builder's CPU twin written as a KTX2 cube and handed to the render
    // (RenderRequest::cookedEnvironments3D, a player's path) lights the plate as the GPU build does
    auto renderer = geometryRenderer();
    RenderRequest built = requestOf(doc3D(plate3D(0.6, "chrome")));
    built.tier = 3;
    const Pixels gpu = render(*renderer, built);
    const int face = titleEnvironmentFace(Environment3D{}, 1.0, TitleTier::Low);
    const TitleEnvCube twin = buildTitleEnvironmentCpu(*titleEnvironmentPreset("studio"), nullptr, 0, face);
    std::vector<uint8_t> file;
    std::string error;
    ASSERT_TRUE(writeTitleEnvironmentKtx2(twin, {{"JE.titleart.environment", "studio"}}, file, error)) << error;
    auto cookedRenderer = geometryRenderer();
    RenderRequest cooked = requestOf(doc3D(plate3D(0.6, "chrome")));
    cooked.tier = 3;
    cooked.cookedEnvironments3D["studio"] = std::make_shared<const std::vector<uint8_t>>(file);
    const Pixels fromFile = render(*cookedRenderer, cooked);
    const Title3DStats* st = stats3D(*cookedRenderer);
    ASSERT_TRUE(st);
    EXPECT_EQ(st->cookedEnvironments, 1u);
    EXPECT_EQ(st->environmentBuilds, 0u) << "nothing is built";
    const Difference d = compare(gpu, fromFile, 24.0);
    EXPECT_LE(d.mean, 3.0);
    EXPECT_LE(d.overFraction, 0.02);
}

TEST_F(Title3DRendererGpu, AnImageLayerProjectsItsPicture) {
    // R1a: an Image layer in 3D shows its picture on its face (a red left half and a blue right
    // half on a white plastic face); face_image 0 shows the face material only (red control)
    static TestAssetResolver assets;
    TitleImageData picture;
    picture.width = 64;
    picture.height = 32;
    picture.rgba.resize((size_t)64 * 32 * 4);
    for (int y = 0; y < 32; y++) {
        for (int x = 0; x < 64; x++) {
            uint8_t* p = &picture.rgba[((size_t)y * 64 + x) * 4];
            p[0] = x < 32 ? 230 : 20;
            p[1] = 20;
            p[2] = x < 32 ? 20 : 230;
            p[3] = 255;
        }
    }
    assets.addImage("textures/halves.png", picture);
    auto docWith = [](double faceImage) {
        Json layer = Json{{"id", "00000001"}, {"type", "image"}, {"name", "Picture"}, {"image", Json{{"path", "textures/halves.png"}}},
                          {"size", 200}, {"dimension", "3d"}};
        layer["style_3d"] = Json{{"face", materialPreset("white_gloss")}, {"bevel", {{"width", 4}, {"depth", 4}}}, {"face_image", faceImage},
                                 {"edge_image", 1}};
        return makeDoc({{"canvas", canvas(512, 256)}, {"layers", Json::array({layer})}, {"scene_3d", {{"supersample", 1}}}});
    };
    auto renderer = geometryRenderer();
    RenderRequest withPicture = requestOf(docWith(1));
    withPicture.assets = &assets;
    RenderRequest without = requestOf(docWith(0));
    without.assets = &assets;
    const Pixels p = render(*renderer, withPicture);
    const Pixels plain = render(*renderer, without);
    const float* left = p.at(256 - 50, 128);
    const float* right = p.at(256 + 50, 128);
    EXPECT_GT(left[0], left[2] + 0.2f) << "the red half";
    EXPECT_GT(right[2], right[0] + 0.2f) << "the blue half";
    const float* plainLeft = plain.at(256 - 50, 128);
    EXPECT_LT(std::abs(plainLeft[0] - plainLeft[2]), 0.1f) << "face_image 0: the white face";
}

TEST_F(Title3DRendererGpu, AnEnvironmentBuildIsSpreadOverFrames) {
    // a cube is built a share per ready() call (the job steps again next frame) instead of in one
    // call; red control: TITLE_ART_TEST_ENV_ONE_SHOT builds it in one
    auto runOnce = [](uint32_t flags, uint64_t& steps, uint64_t& maxUnits) {
        auto renderer = geometryRenderer();
        RenderRequest r = requestOf(doc3D(plate3D(0.6, "chrome")), flags);
        r.tier = 3;
        const Pixels p = render(*renderer, r);
        EXPECT_GT(p.at(256, 128)[3], 0.99f);
        const Title3DStats* st = stats3D(*renderer);
        ASSERT_TRUE(st);
        EXPECT_EQ(st->environmentBuilds, 1u);
        steps = st->environmentSteps;
        maxUnits = st->environmentMaxStepUnits;
    };
    uint64_t spreadSteps = 0, spreadMax = 0, oneSteps = 0, oneMax = 0;
    runOnce(0, spreadSteps, spreadMax);
    runOnce(TITLE_ART_TEST_ENV_ONE_SHOT, oneSteps, oneMax);
    EXPECT_GE(spreadSteps, 2u) << "the build took several calls";
    EXPECT_EQ(oneSteps, 1u);
    EXPECT_LT(spreadMax, oneMax) << "no call ran the whole build";
}

TEST(Title3DEnvironmentTest, LevelMapAndSceneTwin) {
    // level = 6 r + log2(face / 512) from r = 1/6 up; a 512 face reads level 6 at r = 1
    EXPECT_NEAR(titleEnvironmentLevel(0.5f, 512), 3.0f, 1e-6f);
    EXPECT_NEAR(titleEnvironmentLevel(0.5f, 1024), 4.0f, 1e-6f);
    EXPECT_NEAR(titleEnvironmentLevel(0.0f, 1024), 0.0f, 1e-6f);
    EXPECT_EQ(titleEnvironmentLevels(1024), 8);
    for (int level = 0; level < 8; level++) {
        EXPECT_NEAR(titleEnvironmentLevel(titleEnvironmentRoughness(level, 1024), 1024), (float)level, 1e-4f);
    }
    // the studio: a hot strip light straight ahead of its panel, radiance above 5 (not clipped)
    const TitleEnvScene* studio = titleEnvironmentPreset("studio");
    ASSERT_TRUE(studio);
    const float d = std::sqrt(9.0f * 9 + 12 * 12);
    const auto strip = evaluateTitleEnvironment(*studio, {9 / d, 0, 12 / d});
    EXPECT_GT(strip[0], 5.0f);
    // straight up the sky's last stop (0.7) unless a panel covers it
    const auto up = evaluateTitleEnvironment(*studio, {0, 1, 0});
    EXPECT_NEAR(up[0], 0.7f, 1e-4f);
    // every procedural preset has a sky and the key softbox
    for (const char* id : {"studio", "strips", "backlit", "under", "sunset", "neon", "gold", "night"}) {
        const TitleEnvScene* s = titleEnvironmentPreset(id);
        ASSERT_TRUE(s) << id;
        EXPECT_FALSE(s->sky.empty()) << id;
        EXPECT_EQ(s->elements.back().kind, TitleEnvElementKind::SoftPanel) << id;
        EXPECT_LE(s->elements.size(), (size_t)TITLE_ENV_MAX_ELEMENTS) << id;
    }
    EXPECT_EQ(titleEnvironmentPreset("night")->elements.size(), 1u + 60u + 4u + 1u);
    // the grade: saturation 0 is grey, intensity scales
    Environment3D env;
    env.saturation = 0;
    env.intensity = 2;
    const auto g = gradeTitleEnvironment({1, 0, 0}, env);
    EXPECT_NEAR(g[0], g[1], 1e-6f);
    EXPECT_NEAR(g[0], 2 * 0.2126f, 1e-5f);
}

TEST(Title3DMathTest, CameraMapsTheCanvasPlaneOneToOne) {
    Camera3D cam;
    const TitleCamera3D c = title3DCamera(cam, 1920, 1080, 1.0, {0, 0, 1920, 1080}, false);
    std::array<double, 2> px;
    ASSERT_TRUE(title3DProject(c, {100, 50, 0}, 1920, 1080, 1.0, px));
    EXPECT_NEAR(px[0], 960 + 100, 1e-3);
    EXPECT_NEAR(px[1], 540 - 50, 1e-3);
    // the projection agrees with the pinhole
    const Mat4 vp = c.projection * c.view;
    const auto clip = vp.apply({100, 50, 0, 1});
    EXPECT_NEAR((clip[0] / clip[3] * 0.5 + 0.5) * 1920, 1060, 1e-2);
    EXPECT_NEAR((0.5 - clip[1] / clip[3] * 0.5) * 1080, 490, 1e-2);
    // AE order: rotation_y then rotation_x differs from the reverse
    const Mat4 a = title3DLayerMatrix(Json{{"rotation_y", 90}, {"rotation_x", 30}}, geometry::Affine());
    const Mat4 b = Mat4::rotateX(30) * Mat4::rotateY(90);
    const auto pa = a.apply({1, 0, 0, 1}), pb = b.apply({1, 0, 0, 1});
    EXPECT_GT(std::abs(pa[1] - pb[1]) + std::abs(pa[2] - pb[2]), 0.1);
}
