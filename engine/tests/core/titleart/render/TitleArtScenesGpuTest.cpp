// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Scenes against goldens (docs/specs/S80-title-art-core.md R11, R12, R16, R27-R29, R38, R39,
// R43; AC11, AC13, AC17, AC23, AC24): the compositor scenes (clipping, adjustment layers in and
// outside a group, an effects-only adjustment, part effects, nested hidden groups, masks from a
// layer's luminance, a feathered shape mask, a group's radial mask, a flipped 2D title), the
// asymmetric fixture that catches a flip of the frame, each Material member on the analytic
// shapes, bounded against unbounded distance fields, and the forced RGBA8 path against the float
// path on every scene within §6.3's tolerance. Goldens are written on glcore
// (JASPER_TEST_UPDATE_GOLDEN=1) and Vulkan matches them. Each Material member drawn with its
// ELEMENT variant is the whole program's image (S80 Deviation 58).

#include "TitleRenderTestEffects.h"
#include "TitleRenderTestSupport.h"

#include "titleart/render/TitleElementVariant.h"

#include "ImageCompare.h"

#include <functional>

using namespace jasperengine;
using namespace jasperengine::titleart;
using namespace jasperengine::titleart::test;

namespace {

    Json gradientFill(const char* a, const char* b, double angle = 90) {
        return Json{{"kind", "linear"}, {"angle", angle},
                    {"gradient", {{"stops", Json::array({{{"pos", 0}, {"color", a}}, {{"pos", 1}, {"color", b}}})}}}};
    }

    Json textLayer(const char* id, const char* text, double size, Json face, Json extra = Json::object()) {
        Json layer{{"id", id}, {"type", "text"}, {"name", id}, {"text", text}, {"size", size}, {"style", {{"face", face}, {"rims", Json::array()}}}};
        for (auto it = extra.begin(); it != extra.end(); ++it) layer[it.key()] = it.value();
        return layer;
    }

    Json mask(const char* id, const char* type, Json extra) {
        Json m{{"id", id}, {"type", type}, {"enabled", true}};
        for (auto it = extra.begin(); it != extra.end(); ++it) m[it.key()] = it.value();
        return m;
    }

    using Scene = std::function<Json()>;

    // the compositor scenes of AC11 (Pachislop's tests/golden/refs/scene names)
    std::vector<std::pair<std::string, Scene>> compositorScenes() {
        return {
            {"clipping", [] {
                 Json base = shapeLayer("00000001", "ellipse", 110, solidFace("#3060ff"));
                 Json clipped = shapeLayer("00000002", "rect", 70, Json{{"fill", gradientFill("#ffe000", "#ff2000")}},
                                           {{"clip", true}, {"transform", {{"position", Json::array({30, 20})}}}});
                 return Json::array({base, clipped});
             }},
            {"adjustment_outside_group", [] {
                 Json a = shapeLayer("00000001", "rect", 100, Json{{"fill", gradientFill("#ff4040", "#4040ff", 0)}});
                 Json adj{{"id", "00000002"}, {"type", "adjustment"}, {"adjustment", {{"type", "hue_saturation"}, {"hue", 90}, {"saturation", 20}}}};
                 return Json::array({a, adj});
             }},
            {"adjustment_in_group", [] {
                 Json below = shapeLayer("00000001", "rect", 120, solidFace("#30c030"), {{"transform", {{"position", Json::array({-40, 0})}}}});
                 Json inner = shapeLayer("00000003", "ellipse", 80, solidFace("#ff8000"));
                 Json adj{{"id", "00000004"}, {"type", "adjustment"}, {"adjustment", {{"type", "invert"}}}};
                 Json top = shapeLayer("00000005", "rect", 40, solidFace("#ffffff"), {{"transform", {{"position", Json::array({20, 20})}}}});
                 Json group{{"id", "00000002"}, {"type", "group"}, {"children", Json::array({inner, adj, top})}, {"transform", {{"position", Json::array({30, 0})}}}};
                 return Json::array({below, group});
             }},
            {"effects_only_blur", [] {
                 Json a = textLayer("00000001", "FF", 90, solidFace("#ffffff"));
                 Json adj{{"id", "00000002"}, {"type", "adjustment"}, {"adjustment", {{"type", "effects_only"}}},
                          {"effects", Json::array({effect("00000003", "test_blur", {{"radius", 6}})})}};
                 return Json::array({a, adj});
             }},
            {"part_effects_2d", [] {
                 Json layer = textLayer("00000001", "FF", 90, solidFace("#ffd040"));
                 layer["style"]["rims"] = Json::array({Json{{"id", "00000011"}, {"width", 8}, {"material", solidFace("#802000")}}});
                 layer["part_effects"] = {{"rim:00000011", Json::array({effect("00000021", "test_glow", {{"size", 12}})})}};
                 return Json::array({layer});
             }},
            {"nested_hidden_groups", [] {
                 Json shown = shapeLayer("00000003", "rect", 60, solidFace("#ff00ff"));
                 Json hidden = shapeLayer("00000004", "ellipse", 90, solidFace("#00ffff"), {{"visible", false}});
                 Json inner{{"id", "00000002"}, {"type", "group"}, {"children", Json::array({hidden, shown})}};
                 Json hiddenGroup{{"id", "00000005"}, {"type", "group"}, {"visible", false},
                                  {"children", Json::array({shapeLayer("00000006", "rect", 140, solidFace("#ffffff"))})}};
                 Json outer{{"id", "00000001"}, {"type", "group"}, {"children", Json::array({inner, hiddenGroup})}};
                 return Json::array({outer});
             }},
            {"mask_layer_luminance", [] {
                 Json matte = shapeLayer("00000002", "rect", 140, Json{{"fill", gradientFill("#000000", "#ffffff", 0)}}, {{"visible", false}});
                 Json masked = shapeLayer("00000001", "ellipse", 120, solidFace("#ff6020"),
                                          {{"masks", Json::array({mask("00000003", "layer", {{"layer", "00000002"}, {"channel", "luminance"}})})}});
                 return Json::array({matte, masked});
             }},
            {"mask_shape_feather", [] {
                 Json layer = shapeLayer("00000001", "rect", 130, Json{{"fill", gradientFill("#2080ff", "#80ff20")}},
                                         {{"masks", Json::array({mask("00000002", "shape", {{"shape", {{"shape", "ellipse"}, {"size", 90}}}, {"feather", 8}})})}});
                 return Json::array({layer});
             }},
            {"group_radial_mask", [] {
                 Json a = shapeLayer("00000002", "rect", 70, solidFace("#ff3030"), {{"transform", {{"position", Json::array({-30, 0})}}}});
                 Json b = shapeLayer("00000003", "rect", 70, solidFace("#3030ff"), {{"transform", {{"position", Json::array({30, 0})}}}});
                 Json group{{"id", "00000001"}, {"type", "group"}, {"children", Json::array({a, b})},
                            {"masks", Json::array({mask("00000004", "gradient", {{"shape", "radial"}, {"length", 80}, {"inner", 0.3}})})}};
                 return Json::array({group});
             }},
            {"flip_2d", [] {
                 Json layer = textLayer("00000001", "F", 120, Json{{"fill", gradientFill("#ffffff", "#ff8000")}, {"bevel", {{"enabled", true}, {"size", 6}}}},
                                        {{"transform", {{"flip_x", true}, {"rotation", 15}}}});
                 return Json::array({layer});
             }},
        };
    }

    // one Material member on (or off its default) per case (AC24's member list)
    std::vector<std::pair<std::string, Json>> materialCases() {
        auto with = [](Json face) { return face; };
        return {
            {"fill_linear", with({{"fill", gradientFill("#ffffff", "#2040ff")}})},
            {"fill_radial", with({{"fill", {{"kind", "radial"}, {"gradient", gradientFill("#ffff00", "#ff0000")["gradient"]}}}})},
            {"fill_angle", with({{"fill", {{"kind", "angle"}, {"gradient", gradientFill("#00ffff", "#ff00ff")["gradient"]}}}})},
            {"fill_reflected", with({{"fill", {{"kind", "reflected"}, {"gradient", gradientFill("#ffffff", "#000000")["gradient"]}}}})},
            {"fill_diamond", with({{"fill", {{"kind", "diamond"}, {"gradient", gradientFill("#ffe0a0", "#a04000")["gradient"]}}}})},
            {"fill_shape_burst", with({{"fill", {{"kind", "shape_burst"}, {"gradient", gradientFill("#000000", "#ffffff")["gradient"]}}}})},
            {"fill_image", with({{"fill", {{"kind", "image"}, {"image", {{"image", {{"path", "tile.png"}, {"guid", ""}}}, {"scale", 0.5}, {"tile", true}}}}}})},
            {"fill_span_char", with({{"fill", {{"kind", "linear"}, {"span", "char"}, {"gradient", gradientFill("#ffffff", "#ff2020")["gradient"]}}}})},
            {"opacity", with({{"fill", {{"kind", "solid"}, {"color", "#ff8000"}}}, {"opacity", 0.5}})},
            {"bevel_smooth", with({{"fill", {{"kind", "solid"}, {"color", "#c0c0c0"}}}, {"bevel", {{"enabled", true}, {"size", 8}}}})},
            {"bevel_fit_down", with({{"fill", {{"kind", "solid"}, {"color", "#c0c0c0"}}}, {"bevel", {{"enabled", true}, {"size_mode", "fit"}, {"direction", "down"}, {"profile", "chamfer"}}}})},
            {"bevel_gloss_custom", with({{"fill", {{"kind", "solid"}, {"color", "#a08040"}}},
                                        {"bevel", {{"enabled", true}, {"size", 10}, {"gloss", "custom"},
                                                   {"gloss_curve", {{"points", Json::array({{{"x", 0}, {"y", 0}}, {{"x", 0.5}, {"y", 1}}, {{"x", 1}, {"y", 0}}})}}}}}})},
            {"reflection", with({{"bevel", {{"enabled", true}, {"size", 8}}}, {"reflection", {{"enabled", true}, {"map", {{"path", "sky.png"}, {"guid", ""}}}, {"blur", 20}}}})},
            {"finish", with({{"bevel", {{"enabled", true}, {"size", 8}}},
                             {"finish", {{"enabled", true}, {"gradient", gradientFill("#301000", "#fff0c0")["gradient"]}}}})},
            {"pattern_zebra", with({{"pattern", {{"kind", "zebra"}, {"scale", 20}}}})},
            {"pattern_seigaiha", with({{"pattern", {{"kind", "seigaiha"}, {"scale", 25}, {"relief", 0.5}}}, {"bevel", {{"enabled", true}, {"size", 4}}}})},
            {"pattern_custom", with({{"pattern", {{"kind", "custom"}, {"image", {{"path", "tile.png"}, {"guid", ""}}}, {"scale", 30}}}})},
            {"satin", with({{"fill", {{"kind", "solid"}, {"color", "#ffc000"}}}, {"satin", {{"enabled", true}, {"distance", 6}, {"size", 8}}}})},
            {"texture_brushed", with({{"fill", {{"kind", "solid"}, {"color", "#b0b0b0"}}}, {"texture", {{"kind", "brushed"}, {"amount", 0.6}}}})},
            {"texture_hammered_surface", with({{"fill", {{"kind", "solid"}, {"color", "#b08050"}}}, {"bevel", {{"enabled", true}, {"size", 5}}},
                                              {"texture", {{"kind", "hammered"}, {"target", "surface"}, {"amount", 0.5}}}})},
            {"inner_glow", with({{"fill", {{"kind", "solid"}, {"color", "#ff3000"}}}, {"inner_glow", {{"enabled", true}, {"size", 6}, {"opacity", 0.9}}}})},
            {"gloss_cap", with({{"fill", {{"kind", "solid"}, {"color", "#2060ff"}}}, {"gloss", {{"enabled", true}}}})},
            {"refraction", with({{"fill", gradientFill("#ffffff", "#000000")}, {"bevel", {{"enabled", true}, {"size", 8}}}, {"refraction", 6}})},
            {"glass", with({{"bevel", {{"enabled", true}, {"size", 8}}}, {"glass", {{"enabled", true}}}})},
            {"sheen", with({{"fill", {{"kind", "solid"}, {"color", "#606060"}}}, {"texture", {{"kind", "hairline"}}}, {"sheen", {{"enabled", true}}}})},
        };
    }

    std::shared_ptr<const TitleArtDoc> sceneDoc(const Json& layers, int w = 256, int h = 160) {
        ensureTestEffects();
        return makeDoc({{"canvas", canvas(w, h)}, {"layers", layers}});
    }

    std::shared_ptr<const TitleArtDoc> materialDoc(const Json& face) {
        Json layer = textLayer("00000001", "FF", 110, face);
        layer["style"]["rims"] = Json::array({Json{{"id", "00000011"}, {"width", 5}, {"material", solidFace("#202020")}}});
        return sceneDoc(Json::array({layer}), 224, 144);
    }

    TestAssets& assets() {
        static TestAssets a;
        return a;
    }

    Pixels renderScene(const std::shared_ptr<const TitleArtDoc>& doc, uint32_t flags = 0) {
        auto renderer = makeRenderer();
        RenderRequest request;
        request.doc = doc;
        request.testFlags = flags;
        request.assets = &assets();
        return render(*renderer, request);
    }

    // procedural noise and hashes (patterns, textures) compile to other float maths through
    // SPIR-V than through GLSL: a few pixels differ between the glcore goldens and Vulkan
    bool hashed(const std::string& name) {
        return name.find("pattern") != std::string::npos || name.find("texture") != std::string::npos || name.find("sheen") != std::string::npos;
    }

    void expectGolden(const std::string& name, const Pixels& p) {
        jasperengine::test::CompareOptions options;
        options.pixelThreshold = hashed(name) ? 24 : 2;
        options.maxBadFraction = hashed(name) ? 0.005 : 0.0;
        options.maxMeanDiff = hashed(name) ? 0.25 : 0.05;
        const jasperengine::test::CompareResult r = jasperengine::test::compareWithGolden(
            "titleart_" + name, p.bytes, p.width, p.height,
            jasperengine::test::hardwareTolerance({24, 0.01, 1.0, 4, 6.0}, options));
        EXPECT_TRUE(r.passed) << name << ": " << r.message;
    }

    void expectFallbackAgrees(const std::string& name, const std::shared_ptr<const TitleArtDoc>& doc) {
        Pixels floatPath = renderScene(doc);
        Pixels fallback = renderScene(doc, TITLE_ART_TEST_NO_FLOAT_TARGETS | TITLE_ART_TEST_NO_R16F | TITLE_ART_TEST_NO_RGBA32F);
        const Difference d = compare(floatPath, fallback);
        EXPECT_LE(d.mean, 1.0) << name << ": mean " << d.mean;
        EXPECT_LE(d.overFraction, 0.005) << name << ": " << d.overFraction * 100.0 << " % over 8/255";
        EXPECT_LE(d.max, 48.0) << name << ": max " << d.max;
        std::printf("[ fallback ] %-28s mean %.3f, over 8: %.3f %%, max %.0f\n", name.c_str(), d.mean, d.overFraction * 100.0, d.max);
    }

    class TitleArtScenesGpu : public ::testing::Test {
    protected:
        void SetUp() override {
            JASPER_REQUIRE_GPU();
            ensureTestEffects();
        }
    };

}

TEST_F(TitleArtScenesGpu, CompositorScenes) {
    for (const auto& [name, scene] : compositorScenes()) {
        SCOPED_TRACE(name);
        expectGolden("compositor_" + name, renderScene(sceneDoc(scene())));
    }
}

TEST_F(TitleArtScenesGpu, MaterialMembers) {
    for (const auto& [name, face] : materialCases()) {
        SCOPED_TRACE(name);
        expectGolden("material_" + name, renderScene(materialDoc(face)));
    }
}

TEST_F(TitleArtScenesGpu, OriginAsymmetric) {
    // an "F", a top-to-bottom gradient and a shadow falling down-right: any flip moves them
    Json f = textLayer("00000001", "F", 160, Json{{"fill", gradientFill("#ffffff", "#ff2000", 90)}},
                       {{"effects", Json::array({effect("00000002", "test_shadow", {{"angle", 60}, {"distance", 14}, {"size", 2}, {"opacity", 1}})})}});
    auto doc = sceneDoc(Json::array({f}), 256, 256);
    Pixels p = renderScene(doc);
    ASSERT_EQ(p.width, 256);
    auto alphaAt = [&p](int x, int y) { return p.bytes[((size_t)y * p.width + x) * 4 + 3]; };
    auto redAt = [&p](int x, int y) { return p.bytes[((size_t)y * p.width + x) * 4 + 0]; };
    auto greenAt = [&p](int x, int y) { return p.bytes[((size_t)y * p.width + x) * 4 + 1]; };
    // the F's top bar reaches right at the top (rows down from y = 128 - 0.4 x 160 = 64), its
    // bottom right is empty; its bar top is white, its stem's foot red
    EXPECT_EQ(alphaAt(128 + 40, 128 - 52), 255);
    EXPECT_EQ(alphaAt(128 + 40, 128 + 52), 0);
    EXPECT_GT(greenAt(128 - 34, 128 - 58), 200);
    EXPECT_LT(greenAt(128 - 34, 128 + 58), 80);
    EXPECT_GT(redAt(128 - 34, 128 + 58), 200);
    // the shadow falls down-right of the stem's foot: dark and opaque, not up-left
    const int sx = 128 - 34 + 7, sy = 128 + 64 + 9;
    EXPECT_EQ(alphaAt(sx, sy), 255);
    EXPECT_LT(redAt(sx, sy), 10);
    EXPECT_EQ(alphaAt(128 - 48 - 7, 128 - 64 - 9), 0);
    expectGolden("origin_asymmetric", p);
    // the RGBA8 path on the same fixture
    Pixels fallback = renderScene(doc, TITLE_ART_TEST_NO_FLOAT_TARGETS | TITLE_ART_TEST_NO_R16F | TITLE_ART_TEST_NO_RGBA32F);
    expectGolden("origin_asymmetric_rgba8", fallback);
}

TEST_F(TitleArtScenesGpu, BoundedJfa) {
    // bevel-only elements: the field flooded as far as the bevel reads shades like the whole
    // frame's, within 1/255, in at most ceil(log2(R + 2)) + 1 flood passes
    Json face{{"fill", {{"kind", "solid"}, {"color", "#c0a060"}}}, {"bevel", {{"enabled", true}, {"size", 6}}}};
    Json layer = textLayer("00000001", "FF", 120, face);
    layer["style"]["rims"] = Json::array({Json{{"id", "00000011"}, {"width", 6}, {"material", Json{{"fill", {{"kind", "solid"}, {"color", "#603010"}}}, {"bevel", {{"enabled", true}, {"size", 3}}}}}}});
    auto doc = sceneDoc(Json::array({layer}), 320, 200);
    auto bounded = makeRenderer();
    RenderRequest request;
    request.doc = doc;
    const uint64_t floodsBefore = bounded->stats().floodPasses;
    Pixels a = render(*bounded, request);
    const uint64_t floods = bounded->stats().floodPasses - floodsBefore;
    const uint64_t unboundedFloods = bounded->stats().floodPassesUnbounded;
    // two elements: the face's reach 6 % of 120 = 7.2 px + 2, the rim's 3.6 px + 2 (k = 4 and 3)
    EXPECT_LE(floods, (uint64_t)((4 + 1) + (3 + 1)));
    EXPECT_LE(floods * 2, unboundedFloods);
    request.testFlags = TITLE_ART_TEST_UNBOUNDED_FIELDS;
    auto unbounded = makeRenderer();
    Pixels b = render(*unbounded, request);
    const Difference d = compare(a, b, 1.0);
    EXPECT_LE(d.max, 1.0) << "bounded and unbounded fields shade differently";
}

TEST_F(TitleArtScenesGpu, FallbackAgrees) {
    for (const auto& [name, scene] : compositorScenes()) expectFallbackAgrees("compositor_" + name, sceneDoc(scene()));
    for (const auto& [name, face] : materialCases()) expectFallbackAgrees("material_" + name, materialDoc(face));
}

TEST_F(TitleArtScenesGpu, Deterministic) {
    for (const auto& [name, scene] : compositorScenes()) {
        auto doc = sceneDoc(scene());
        EXPECT_TRUE(identical(renderScene(doc), renderScene(doc))) << name;
    }
}

// S80 Deviation 58: every Material member drawn with the ELEMENT variant of its material
// (TitleElementVariant.h) is the image the whole program draws, on both pixel paths. The variants
// make the members the material leaves off constants that equal the block's values; a variant
// that dropped a member the material uses (or kept a wrong kind) shades differently. Red control:
// with satin, glass and reflection made constants regardless of the block, those cases differ
// (run by hand, Deviation 58).
TEST_F(TitleArtScenesGpu, ElementVariantsDrawTheWholeProgram) {
    for (uint32_t path : {0u, TITLE_ART_TEST_NO_FLOAT_TARGETS | TITLE_ART_TEST_NO_R16F | TITLE_ART_TEST_NO_RGBA32F}) {
        for (const auto& [name, face] : materialCases()) {
            SCOPED_TRACE(name + (path ? " (rgba8)" : ""));
            auto doc = materialDoc(face);
            const Pixels variant = renderScene(doc, path);
            const Pixels whole = renderScene(doc, path | TITLE_ART_TEST_ELEMENT_WHOLE);
            const Difference d = compare(variant, whole, 1.0);
            std::printf("[ variant ] %-28s %s max %.2f, mean %.4f\n", name.c_str(), path ? "rgba8" : "float", d.max, d.mean);
            EXPECT_LE(d.max, 1.0) << "max " << d.max << ", mean " << d.mean;
            EXPECT_EQ(d.overFraction, 0.0);
        }
    }
}
