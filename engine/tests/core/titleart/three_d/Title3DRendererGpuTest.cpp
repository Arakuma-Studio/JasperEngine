// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S86 AC9, AC10 (partly), AC13 (CPU twin), AC15, AC27 (zero cost): a Dimension 3D layer through
// the S80 renderer on the headless device. A gold 3D plate draws lit pixels where its 2D twin
// does and nothing outside its region; the ramp's bend changes the image (red control: bend 0);
// the compressed-light path agrees with the float path; the debug views; AO (AC16) darkens the
// crevices and keeps open faces; gems refract the layer's own opaque parts (AC11); a part's own
// environment lights that part only; the glint (AC17) adds light past the letters on every tier
// (red control: a threshold nothing passes); an environment build is spread over frames (the 21 s
// software-rasteriser stall of one-shot builds; red control: one shot); a 2D document makes no
// 3D renderer; the environment's level map and CPU scene evaluation.

#include "titleart/render/TitleRenderTestSupport.h"

#include "titleart/render/TitleRenderServices.h"
#include "titleart/three_d/Title3DMath.h"
#include "titleart/three_d/Title3DRenderer.h"
#include "titleart/three_d/TitleEnvironment.h"

#include <cmath>

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

    double luminance(const float* p) {
        return 0.2126 * p[0] + 0.7152 * p[1] + 0.0722 * p[2];
    }

    class Title3DRendererGpu : public ::testing::Test {
    protected:
        void SetUp() override { JASPER_REQUIRE_GPU(); }
    };

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
    auto withAO = [](double intensity) {
        Json layer = plate3D();
        layer["style_3d"]["ao"] = Json{{"enabled", true}, {"radius", 6}, {"intensity", intensity}};
        return doc3D(layer);
    };
    const Pixels off = render(*renderer, requestOf(doc3D(plate3D())));
    const Pixels on = render(*renderer, requestOf(withAO(1.5)));
    const Pixels zero = render(*renderer, requestOf(withAO(0)));
    ASSERT_EQ(off.rgba.size(), on.rgba.size());
    size_t covered = 0, darkened = 0;
    for (size_t i = 0; i + 3 < off.rgba.size(); i += 4) {
        if (off.rgba[i + 3] < 0.99f) continue;
        covered++;
        const double a = luminance(&off.rgba[i]), b = luminance(&on.rgba[i]);
        if (a > 0.05 && b < a * 0.85) darkened++;
    }
    ASSERT_GT(covered, 0u);
    EXPECT_GT(darkened, covered / 200) << darkened << " of " << covered << " pixels darkened by 15 %";
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
    if (!SystemRender::getFeatures().renderRGBA16F || !SystemRender::getFeatures().filterRGBA16F) GTEST_SKIP() << "no float passes here";
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
    Json tinted = plate3D(0.0, "chrome");
    tinted["style_3d"]["face"]["environment"] = Json{{"tint", "#ff2020"}};
    const Pixels plain = render(*renderer, requestOf(doc3D(plate3D(0.0, "chrome"))));
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

TEST_F(Title3DRendererGpu, DebugViewsAoInkAndWireframe) {
    auto renderer = geometryRenderer();
    Json layer = plate3D();
    layer["style_3d"]["ao"] = Json{{"enabled", true}, {"radius", 6}, {"intensity", 1.5}};
    RenderRequest ao = requestOf(doc3D(layer));
    ao.debugView3D = (int)Title3DDebugView::AO;
    const Pixels aoView = render(*renderer, ao);
    EXPECT_GT(aoView.at(256, 128)[0], 0.95f) << "the open face is unoccluded";
    size_t dark = 0;
    for (size_t i = 0; i + 3 < aoView.rgba.size(); i += 4) dark += aoView.rgba[i + 3] > 0.99f && aoView.rgba[i] < 0.85f;
    EXPECT_GT(dark, 0u) << "the crevices show";
    // ink: the render without glint (no glint here: the render itself)
    RenderRequest ink = requestOf(doc3D(plate3D()));
    ink.debugView3D = (int)Title3DDebugView::Ink;
    EXPECT_LE(compare(render(*renderer, ink), render(*renderer, requestOf(doc3D(plate3D()))), 2.0).overFraction, 0.0);
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
    const RenderFeatures& f = SystemRender::getFeatures();
    if (!f.renderRGBA16F || !f.filterRGBA16F || !f.blendRGBA16F) GTEST_SKIP() << "no float passes here";
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
