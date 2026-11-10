// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Pachislop parity of the compositor scenes (docs/specs/S80-title-art-core.md AC12, AC11's list):
// the semantic scenes of Pachislop's golden harness (tests/harness/cases.ts `scenes()`, by
// SafEight, used with the author's permission) written as documents member for member, rendered
// whole by the renderer at 0.25, trimmed and centred like the effect cases (support/TitleParity.h),
// against its references (tests/golden/refs/scene, copied to golden/titleart/pachislop/scene/)
// with AC12's block and histogram metric.
//
// How a Pachislop project becomes a document (its src/model/types.ts, defaults.ts, masks.ts):
// - Its comp space is ours: canvas px from the centre, +y up. `transform.x / y` are `position`
//   unchanged, so title2D('BIG', -150) sits below the centre in both. `rotZ` turns clockwise on
//   screen, as `rotation` does, and `flipX` mirrors about the position before the turn, as
//   `flip_x` does. (Only the rim's `offsetY` is +y down: the standard title negates it.)
// - Its layer list is bottom first with members before their group (`parentId`, normalizeTree);
//   ours is bottom first with members in the group's `children`.
// - A shape mask's `x / y / rotation` (clockwise) are `position` and `rotation`; its source is
//   shapeSource(kind, size) with that kind's starting values. A gradient mask's `angle` runs
//   counter-clockwise in Pachislop and clockwise in ours, so it is negated. Masks are linked
//   unless the case says otherwise (cases.ts `maskBase`).
// - title2D(text, y) is the standard title (support/TitleStandardTitle.h) with `text` at (0, y);
//   effects and generators take their definitions' defaults plus the case's params.
//
// mask-from-layer-luminance's matte is S83's Sunburst (S83 §6.7.15) at its first preset, Red & Gold
// Rays with its Vignette Gradient Overlay, as Pachislop's createGeneratorLayer('sunburst') makes
// it (editor/resources/titleart/fx-presets/sunburst.yaml), hidden. The 3D scenes (flip-3d,
// part-effects-3d, mask-gradient-3d, lighting-per-layer, image-3d-plain), split-image and
// mask-adjustment are outside AC11's list.
//
// The fixture is TitleArtScenesParityGpu, not TitleArtParityGpu: gtest wants every test of a
// suite on one fixture class, and TitleArtParityGpu is a class of the effect test's anonymous
// namespace.

#include "../support/TitleGenTestSupport.h"
#include "../support/TitleParity.h"
#include "../support/TitleStandardTitle.h"

#include "GpuEnvironment.h"

#include <cstdio>
#include <map>
#include <string>
#include <utility>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::titleart;
using namespace jasperengine::titleart::test::parity;
namespace standard = jasperengine::titleart::test;

namespace {

    // ids share one space in a document: layer n is n, rim k of layer n 0x100 + 16 n + k,
    // effects from 0x200, masks from 0x300
    std::string idOf(int n) {
        char t[9];
        std::snprintf(t, sizeof(t), "%08x", n);
        return t;
    }

    std::string rimId(int layer, int k) {
        return idOf(0x100 + layer * 16 + k);
    }

    // Pachislop's title2D(text, y): the standard title's layer with `text` at (0, y)
    Json title(int n, const char* text, double y = 0) {
        Json layer = standard::standardTitleLayer(idOf(n).c_str(), text);
        int k = 0;
        for (Json& rim : layer["style"]["rims"]) rim["id"] = rimId(n, k++);
        layer["transform"] = Json{{"position", Json::array({0, y})}};
        return layer;
    }

    Json group(int n, std::vector<Json> children, bool visible = true) {
        Json g{{"id", idOf(n)}, {"type", "group"}, {"name", "Group"}, {"visible", visible}, {"children", Json::array()}};
        for (Json& child : children) g["children"].push_back(std::move(child));
        return g;
    }

    // adjustmentScene's Hue/Saturation: hue 150, saturation 20
    Json hueShift(int n) {
        return Json{{"id", idOf(n)},
                    {"type", "adjustment"},
                    {"name", "Hue/Saturation"},
                    {"adjustment", {{"type", "hue_saturation"}, {"hue", 150}, {"saturation", 20}, {"lightness", 0}, {"colorize", false}}}};
    }

    // Pachislop's shapeSource(kind, size) for the kinds the cases use (ellipse: the defaults;
    // star: 5 points, inner 0.45)
    Json shapeSource(const char* kind, double size) {
        const bool star = std::string(kind) == "star";
        return Json{{"shape", kind},
                    {"size", size},
                    {"aspect", 1},
                    {"corner", 0},
                    {"slant", 0},
                    {"points", star ? 5 : 16},
                    {"inner", star ? 0.45 : 0.7},
                    {"jitter", 0},
                    {"seed", 1},
                    {"tail", {{"x", -30}, {"y", -70}, {"width", 25}}}};
    }

    // nested-hidden-groups: 激アツ in a visible group inside a hidden one, 確定 in a hidden group
    // inside a visible one, BIG in the visible one; only BIG draws. `hide` false shows every
    // group (the red control's scene).
    std::vector<Json> nestedGroups(bool hide) {
        Json shownInner = group(2, {title(3, "激アツ", 200)});
        Json hiddenOuter = group(1, {shownInner}, !hide);
        Json hiddenInner = group(5, {title(6, "確定", 0)}, !hide);
        Json outer = group(4, {hiddenInner, title(7, "BIG", -200)});
        return {hiddenOuter, outer};
    }

    // AC11's scenes by their Pachislop reference names (golden/titleart/pachislop/scene/<name>.png)
    std::vector<std::pair<std::string, std::vector<Json>>> pachislopScenes() {
        std::vector<std::pair<std::string, std::vector<Json>>> scenes;

        // a full-frame Pattern Fill (its first preset, Zebra: the defaults) clipped to BIG
        Json zebra{{"id", idOf(2)}, {"type", "generator"}, {"name", "Zebra"}, {"clip", true},
                   {"generator", {{"type", "pattern_fill"}, {"params", Json::object()}}}};
        scenes.push_back({"clipping", {title(1, "BIG"), zebra}});

        // inside the group the hue shift reaches only 確定; at the top level BIG too
        scenes.push_back({"adjustment-in-group", {title(1, "BIG", -150), group(2, {title(3, "確定", 120), hueShift(4)})}});
        scenes.push_back({"adjustment-outside-group", {title(1, "BIG", -150), group(2, {title(3, "確定", 120)}), hueShift(4)}});

        Json blur{{"id", idOf(3)}, {"type", "adjustment"}, {"name", "Effects only"}, {"adjustment", {{"type", "effects_only"}}}};
        blur["effects"] = Json::array({standard::standardEffect(idOf(0x201).c_str(), "gaussian_blur", Json{{"radius", 12}})});
        scenes.push_back({"effects-only-gaussian-blur", {title(1, "BIG", -150), title(2, "確定", 120), blur}});

        // Outer Glow on the outermost rim (rims.at(-1): the seated black one), Inner Shadow on the face
        Json parts = title(1, "確定");
        parts["part_effects"] = Json{{"rim:" + rimId(1, 3), Json::array({standard::standardEffect(idOf(0x201).c_str(), "outer_glow")})},
                                     {"face", Json::array({standard::standardEffect(idOf(0x202).c_str(), "inner_shadow")})}};
        scenes.push_back({"part-effects-2d", {parts}});

        scenes.push_back({"nested-hidden-groups", nestedGroups(true)});

        // a feathered ellipse (2.2 wide, turned 20 more) and an inverted half-density star 140 px
        // right, both linked: they turn with the layer's 12
        Json masked = title(1, "確定");
        masked["effects"] = Json::array({standard::standardEffect(idOf(0x201).c_str(), "outer_glow")});
        masked["transform"]["rotation"] = 12;
        Json ellipse = shapeSource("ellipse", 260);
        ellipse["aspect"] = 2.2;
        masked["masks"] = Json::array({Json{{"id", idOf(0x301)}, {"type", "shape"}, {"feather", 18}, {"position", Json::array({0, 0})},
                                            {"rotation", 20}, {"shape", ellipse}},
                                       Json{{"id", idOf(0x302)}, {"type", "shape"}, {"invert", true}, {"density", 0.5},
                                            {"position", Json::array({140, 0})}, {"rotation", 0}, {"shape", shapeSource("star", 120)}}});
        scenes.push_back({"mask-shape-feather", {masked}});

        // an unlinked radial mask on the group shapes both members as one; Pachislop's
        // gradientMask() angle -90 (counter-clockwise: down) is 90 here (unused by a radial one)
        Json radial = group(1, {title(2, "BIG", -150), title(3, "確定", 120)});
        radial["masks"] = Json::array({Json{{"id", idOf(0x301)}, {"type", "gradient"}, {"linked", false}, {"shape", "radial"},
                                            {"position", Json::array({0, 0})}, {"angle", 90}, {"length", 300}, {"inner", 0.4}, {"mid", 0.5}}});
        scenes.push_back({"mask-group-radial", {radial}});

        // mirrored and turned: the drop shadow and the bevel light keep their screen direction
        Json flipped = title(1, "激アツ");
        flipped["effects"] = Json::array({standard::standardEffect(idOf(0x201).c_str(), "drop_shadow")});
        flipped["transform"]["rotation"] = 15;
        flipped["transform"]["flip_x"] = true;
        scenes.push_back({"flip-2d", {flipped}});

        // a hidden Sunburst still drives the mask: the title through the rays' luminance
        // (Pachislop's maskBase: linked, density 1, no feather)
        const Json presets = standard::loadGeneratorPresets("sunburst");
        const Json preset = presets.empty() ? Json::object() : presets[0];
        const Json look = preset.value("layer", Json::object());
        Json rays{{"id", idOf(2)}, {"type", "generator"}, {"name", preset.value("name", std::string("Sunburst"))}, {"visible", false},
                  {"blend", look.value("blend", std::string("normal"))}, {"opacity", look.value("opacity", 1.0)},
                  {"generator", {{"type", "sunburst"}, {"params", preset.value("params", Json::object())}}}, {"effects", Json::array()}};
        int n = 0;
        for (const Json& e : look.value("effects", Json::array())) {
            Json fx = e;
            fx["id"] = idOf(0x201 + n++);
            fx["enabled"] = true;
            rays["effects"].push_back(fx);
        }
        Json lit = title(1, "確定");
        lit["masks"] = Json::array({Json{{"id", idOf(0x301)}, {"type", "layer"}, {"layer", idOf(2)}, {"channel", "luminance"}}});
        scenes.push_back({"mask-from-layer-luminance", {rays, lit}});

        return scenes;
    }

    bool renderScene(const std::vector<Json>& layers, Rgba8Image& out, std::string& error) {
        return renderDocument(standard::standardDocument(layers), 0.25f, TitleOutputFormat::Srgb8, nullptr, &out, error);
    }

    // `image` framed as expectParity frames it, against the reference `name`
    BlockStats againstReference(const std::string& name, const Rgba8Image& image) {
        Rgba8Image ref;
        EXPECT_TRUE(readImage(goldenPath("titleart/pachislop/scene/" + name + ".png"), ref)) << name;
        return compareBlocks(ref, centred(trim(image, 8), ref.width, ref.height));
    }

    class TitleArtScenesParityGpu : public ::testing::Test {
    protected:
        void SetUp() override { JASPER_REQUIRE_GPU(); }
    };

}

TEST_F(TitleArtScenesParityGpu, Scenes) {
    std::map<std::string, Rgba8Image> rendered;
    for (const auto& [name, layers] : pachislopScenes()) {
        SCOPED_TRACE(name);
        Rgba8Image out;
        std::string error;
        ASSERT_TRUE(renderScene(layers, out, error)) << error;
        expectParity(("scene/" + name).c_str(), out);
        rendered[name] = std::move(out);
    }

    // red controls: the metric tells the scenes' tree rules apart
    // - the hue shift at the top level is not the one inside the group (BIG turns blue)
    const BlockStats scope = againstReference("adjustment-in-group", rendered["adjustment-outside-group"]);
    EXPECT_FALSE(withinParity(scope)) << "the parity metric passes an adjustment outside its group: " << scope.text();
    // - hidden groups drawn are not hidden groups
    Rgba8Image shown;
    std::string error;
    ASSERT_TRUE(renderScene(nestedGroups(false), shown, error)) << error;
    const BlockStats hidden = againstReference("nested-hidden-groups", shown);
    EXPECT_FALSE(withinParity(hidden)) << "the parity metric passes hidden groups drawn: " << hidden.text();
    // - the title without the Sunburst's luminance is not the masked title
    Rgba8Image unmasked;
    ASSERT_TRUE(renderScene({title(1, "確定")}, unmasked, error)) << error;
    const BlockStats lit = againstReference("mask-from-layer-luminance", unmasked);
    EXPECT_FALSE(withinParity(lit)) << "the parity metric passes the title without its luminance mask: " << lit.text();
}
