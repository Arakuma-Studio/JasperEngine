// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S86 AC9, AC10 (partly), AC13 (CPU twin), AC15, AC27 (zero cost): a Dimension 3D layer through
// the S80 renderer on the headless device. A gold 3D plate draws lit pixels where its 2D twin
// does and nothing outside its region; the ramp's bend changes the image (red control: bend 0);
// the compressed-light path agrees with the float path; the normals and environment debug views;
// a 2D document makes no 3D renderer; the environment's level map and CPU scene evaluation.

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
