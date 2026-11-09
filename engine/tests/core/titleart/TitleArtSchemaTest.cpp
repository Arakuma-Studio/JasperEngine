// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include "titleart/TitleArtIO.h"
#include "titleart/TitleArtSchema.h"

#include <utility>

using namespace jasperengine;
using namespace jasperengine::titleart;

namespace {

    class TitleArtSchemaTest : public ::testing::Test {};

    // §6.1's members by kind, as the spec's tables list them (S80 v1)
    const std::vector<std::pair<const char*, std::vector<const char*>>> SPEC_MEMBERS = {
        {"document", {"format", "version", "guid", "name", "canvas.width", "canvas.height", "canvas.background.transparent",
                      "canvas.background.color", "global_light.angle", "global_light.altitude", "seed",
                      "material_links.*.name", "layers[*]", "exposed[*].name", "exposed[*].label", "exposed[*].type",
                      "exposed[*].targets[*].layer", "exposed[*].targets[*].field", "exposed[*].min", "exposed[*].max",
                      "exposed[*].options", "outputs[*].id", "outputs[*].content", "outputs[*].layers", "outputs[*].file",
                      "outputs[*].scale", "outputs[*].trim", "outputs[*].padding", "outputs[*].round_to",
                      "outputs[*].bit_depth", "outputs[*].alpha_bleed", "outputs[*].max_size"}},
        {"layer", {"id", "type", "name", "visible", "locked", "guide", "blend", "opacity", "clip", "transform.position",
                   "transform.rotation", "transform.scale", "transform.flip_x", "transform.flip_y", "masks[*]", "effects[*].id",
                   "effects[*].type", "effects[*].enabled", "effects[*].params", "effects[*].from_look", "look.id",
                   "look.color_scheme", "look.owner"}},
        {"layer.text", {"text", "font", "face_index", "fallback_fonts", "variations.*", "features.*", "language", "size",
                        "tracking", "line_height", "kerning", "align", "vertical", "tate_chu_yoko", "tate_chu_yoko_max",
                        "proportional", "h_scale", "v_scale", "shear", "chars.*.offset", "chars.*.scale", "chars.*.h_scale",
                        "chars.*.v_scale", "chars.*.rotation", "chars.*.face", "lines.*.scale", "lines.*.offset",
                        "lines.*.h_scale", "lines.*.v_scale", "warp.style", "warp.bend", "warp.horizontal", "warp.vertical",
                        "distort.mode", "distort.corners", "distort.mesh", "weight", "dimension", "style.knockout",
                        "style.erode.enabled", "style.erode.amount", "style.erode.edge", "style.erode.scale",
                        "style.erode.angle", "style.erode.seed", "part_effects.*[*]"}},
        {"layer.shape", {"shape", "size", "aspect", "corner", "slant", "points", "inner", "jitter", "seed", "tail.x", "tail.y",
                         "tail.width", "warp.style", "distort.mode", "weight", "dimension", "style.face", "part_effects.*[*]"}},
        {"layer.vector", {"svg", "size", "mode", "warp.style", "distort.mode", "weight", "dimension", "style.rims[*].width"}},
        {"layer.image", {"image", "size", "filter", "trace.threshold", "trace.smooth", "trace.despeckle", "dimension", "style.face"}},
        {"layer.group", {"children[*]", "collapsed"}},
        {"layer.generator", {"generator.type", "generator.params", "follow"}},
        {"layer.adjustment", {"adjustment"}},
        {"rim", {"id", "enabled", "width", "join", "fill_counters", "plate.kind", "plate.close", "plate.corner", "plate.slant",
                 "offset", "material.fill.kind"}},
        {"material", {"fill.kind", "fill.color", "fill.gradient.stops[*].pos", "fill.gradient.stops[*].color",
                      "fill.gradient.stops[*].alpha", "fill.gradient.stops[*].mid", "fill.gradient.interp", "fill.angle",
                      "fill.scale", "fill.offset", "fill.span", "fill.image.image", "fill.image.scale", "fill.image.offset",
                      "fill.image.rotation", "fill.image.tile", "opacity", "bevel.enabled", "bevel.profile",
                      "bevel.profile_curve.points[*].x", "bevel.profile_curve.points[*].y", "bevel.profile_curve.points[*].corner",
                      "bevel.size", "bevel.size_mode", "bevel.fit", "bevel.depth", "bevel.direction", "bevel.soften",
                      "bevel.light_angle", "bevel.light_altitude", "bevel.use_global_light", "bevel.gloss", "bevel.gloss_curve.points[*].x",
                      "bevel.highlight_color", "bevel.highlight_opacity", "bevel.shadow_color", "bevel.shadow_opacity",
                      "reflection.enabled", "reflection.map", "reflection.amount", "reflection.blur", "reflection.rotation",
                      "reflection.colorize", "finish.enabled", "finish.gradient.interp", "finish.amount", "pattern.kind",
                      "pattern.color", "pattern.color2", "pattern.opacity", "pattern.scale", "pattern.angle", "pattern.seed",
                      "pattern.image", "pattern.shade", "pattern.relief", "satin.enabled", "satin.color", "satin.blend",
                      "satin.opacity", "satin.angle", "satin.use_global_light", "satin.distance", "satin.size",
                      "satin.contour.points[*].y", "satin.invert", "texture.kind", "texture.amount", "texture.scale",
                      "texture.blend", "texture.seed", "texture.target", "texture.angle", "inner_glow.enabled",
                      "inner_glow.color", "inner_glow.size", "inner_glow.opacity", "inner_glow.noise", "gloss.enabled",
                      "gloss.opacity", "gloss.position", "gloss.softness", "gloss.curve", "gloss.angle", "refraction",
                      "glass.enabled", "glass.color", "glass.tint", "glass.opacity", "glass.edge", "glass.caustic",
                      "sheen.enabled", "sheen.color", "sheen.amount", "sheen.position", "sheen.width", "link"}},
        {"mask", {"id", "type", "enabled", "invert", "feather", "density", "linked"}},
        {"mask.shape", {"shape.shape", "shape.size", "shape.aspect", "shape.corner", "shape.slant", "shape.points",
                        "shape.inner", "shape.jitter", "shape.seed", "shape.tail.x", "position", "rotation"}},
        {"mask.gradient", {"shape", "position", "angle", "length", "inner", "mid"}},
        {"mask.layer", {"layer", "channel"}},
        {"mask.image", {"image", "channel", "position", "size", "rotation"}},
        {"adjustment.brightness_contrast", {"brightness", "contrast"}},
        {"adjustment.levels", {"channels.rgb.in_black", "channels.r.in_white", "channels.g.gamma", "channels.b.out_black",
                               "channels.rgb.out_white"}},
        {"adjustment.curves", {"channels.rgb.points[*].x", "channels.b.points[*].y"}},
        {"adjustment.exposure", {"exposure", "offset", "gamma"}},
        {"adjustment.vibrance", {"vibrance", "saturation"}},
        {"adjustment.hue_saturation", {"hue", "saturation", "lightness", "colorize"}},
        {"adjustment.color_balance", {"shadows.cyan_red", "midtones.magenta_green", "highlights.yellow_blue", "preserve_luminosity"}},
        {"adjustment.black_white", {"reds", "yellows", "greens", "cyans", "blues", "magentas", "tint", "tint_color"}},
        {"adjustment.photo_filter", {"color", "density", "preserve_luminosity"}},
        {"adjustment.gradient_map", {"gradient.stops[*].pos", "reverse"}},
        {"adjustment.posterize", {"levels"}},
        {"adjustment.threshold", {"level"}},
    };

}

TEST_F(TitleArtSchemaTest, CoversEveryMember) {
    const TitleArtSchema& schema = TitleArtSchema::get();
    for (const auto& [kind, patterns] : SPEC_MEMBERS) {
        ASSERT_TRUE(schema.kind(kind)) << kind;
        for (const char* pattern : patterns) {
            const SchemaMember* m = schema.resolvePattern(kind, pattern);
            ASSERT_TRUE(m) << kind << ": " << pattern << " is not in the schema";
            EXPECT_FALSE(m->label.empty()) << kind << ": " << pattern << " has no label";
            if (m->type == ValueType::Number || m->type == ValueType::Integer) {
                EXPECT_TRUE(m->nullable || m->defaultValue.is_number()) << kind << ": " << pattern << " has no default";
                EXPECT_LE(m->min, m->max) << kind << ": " << pattern;
            }
            if (m->type == ValueType::Enum) {
                ASSERT_TRUE(m->defaultValue.is_string()) << pattern;
                EXPECT_NE(std::find(m->options.begin(), m->options.end(), m->defaultValue.get<std::string>()), m->options.end())
                    << kind << ": " << pattern << "'s default is one of its values";
            }
        }
    }
    // the adjustment kinds S80 has: the fourteen operations
    EXPECT_EQ(schema.kind("adjustment")->variants.size(), 14u);
    EXPECT_EQ(schema.kind("layer")->variants.size(), 7u);
    EXPECT_EQ(schema.member(*schema.kind("layer"), "blend")->options.size(), 18u);
}

TEST_F(TitleArtSchemaTest, AnimationClassesFollowS85) {
    const TitleArtSchema& schema = TitleArtSchema::get();
    auto anim = [&](const char* kind, const char* pattern) {
        const SchemaMember* m = schema.resolvePattern(kind, pattern);
        EXPECT_TRUE(m) << pattern;
        return m ? m->anim : FxAnim::None;
    };
    EXPECT_EQ(anim("layer", "transform.position"), FxAnim::Live);
    EXPECT_EQ(anim("layer", "transform.flip_x"), FxAnim::Live);
    EXPECT_EQ(schema.resolvePattern("layer", "transform.flip_x")->interp, ParamInterp::Stepped);
    EXPECT_EQ(anim("layer", "opacity"), FxAnim::Live);
    EXPECT_EQ(schema.resolvePattern("layer", "opacity")->combine, ParamCombine::Multiply);
    EXPECT_EQ(anim("layer", "visible"), FxAnim::Live);
    EXPECT_EQ(anim("layer", "blend"), FxAnim::Step);
    EXPECT_EQ(anim("layer", "clip"), FxAnim::Step);
    EXPECT_EQ(anim("layer", "id"), FxAnim::None);
    EXPECT_EQ(anim("layer", "type"), FxAnim::None);
    EXPECT_EQ(anim("layer", "name"), FxAnim::None);
    EXPECT_EQ(anim("layer", "locked"), FxAnim::None);
    EXPECT_EQ(anim("layer", "look.id"), FxAnim::None);
    EXPECT_EQ(anim("layer.text", "font"), FxAnim::Step);
    for (const char* relayout : {"text", "size", "tracking", "line_height", "kerning", "align", "vertical", "proportional",
                                 "h_scale", "v_scale", "shear", "chars.*.rotation", "lines.*.scale", "warp.bend",
                                 "distort.corners", "weight", "variations.*"}) {
        EXPECT_EQ(anim("layer.text", relayout), FxAnim::Relayout) << relayout;
    }
    EXPECT_EQ(anim("layer.shape", "points"), FxAnim::Relayout) << "shape geometry";
    EXPECT_EQ(anim("layer.image", "size"), FxAnim::Relayout);
    EXPECT_EQ(anim("layer.vector", "svg"), FxAnim::Step);
    EXPECT_EQ(anim("rim", "width"), FxAnim::Rerender);
    EXPECT_EQ(anim("material", "fill.color"), FxAnim::Rerender);
    EXPECT_EQ(anim("material", "fill.kind"), FxAnim::Step);
    EXPECT_EQ(anim("material", "fill.gradient.stops[*].pos"), FxAnim::Rerender);
    EXPECT_EQ(anim("material", "bevel.enabled"), FxAnim::Step);
    EXPECT_EQ(anim("material", "link"), FxAnim::None);
    EXPECT_EQ(anim("mask", "feather"), FxAnim::Rerender);
    EXPECT_EQ(anim("mask.shape", "shape.size"), FxAnim::Rerender) << "mask members re-render";
    EXPECT_EQ(anim("adjustment.hue_saturation", "hue"), FxAnim::Rerender);
    EXPECT_EQ(anim("document", "seed"), FxAnim::Rerender);
    EXPECT_EQ(anim("document", "global_light.angle"), FxAnim::Rerender);
    EXPECT_EQ(anim("document", "canvas.width"), FxAnim::None);
    EXPECT_EQ(anim("document", "outputs[*].scale"), FxAnim::None);
    EXPECT_EQ(anim("document", "exposed[*].name"), FxAnim::None);
    EXPECT_EQ(anim("layer.group", "collapsed"), FxAnim::None);
}

TEST_F(TitleArtSchemaTest, HashScopesSeparateTheCascade) {
    const TitleArtSchema& schema = TitleArtSchema::get();
    EXPECT_EQ(schema.resolvePattern("layer", "transform.position")->hash, HashScope::Placement);
    EXPECT_EQ(schema.resolvePattern("layer", "transform.rotation")->hash, HashScope::Stack);
    EXPECT_EQ(schema.resolvePattern("layer", "transform.scale")->hash, HashScope::Geometry);
    EXPECT_EQ(schema.resolvePattern("rim", "width")->hash, HashScope::Mask);
    EXPECT_EQ(schema.resolvePattern("material", "bevel.size")->hash, HashScope::Field);
    EXPECT_EQ(schema.resolvePattern("material", "fill.color")->hash, HashScope::Stack);
    EXPECT_EQ(schema.resolvePattern("layer", "name")->hash, HashScope::Identity);
}

TEST_F(TitleArtSchemaTest, RegisteredMemberIsValidated) {
    TitleArtSchema& schema = TitleArtSchema::get();
    SchemaMember member;
    member.key = "glow_boost";
    member.type = ValueType::Number;
    member.defaultValue = 1.0;
    member.min = 0;
    member.max = 4;
    member.owner = "test";
    ASSERT_TRUE(schema.addMember("layer.text", member));
    EXPECT_FALSE(schema.addMember("layer.text", member)) << "a key registers once";
    TitleArtDoc doc = makeNewTitleArt();
    EXPECT_EQ(doc.layers()[0]["glow_boost"], 1.0) << "makeNewTitleArt normalized with the member: it is written";
    doc.layers()[0]["glow_boost"] = 9;
    std::vector<TitleWarning> warnings;
    normalizeTitleArt(doc, &warnings);
    EXPECT_EQ(doc.layers()[0]["glow_boost"], 4);
    ASSERT_FALSE(warnings.empty());
    EXPECT_EQ(warnings[0].path, "layers[0].glow_boost");
    EXPECT_EQ(warnings[0].message, "9 clamped to 4");
    const FieldSetResult typo = setTitleArtField(doc.layers()[0], "layer", "glow_bost", 2);
    EXPECT_FALSE(typo.ok);
    EXPECT_EQ(typo.nearest, "glow_boost") << "a registered member appears in did-you-mean";
    EXPECT_TRUE(schema.removeMember("layer.text", "glow_boost"));
}

TEST_F(TitleArtSchemaTest, SparseKind) {
    TitleArtSchema& schema = TitleArtSchema::get();
    SchemaKind kind;
    kind.name = "test_translation";
    kind.label = "Translation";
    kind.sparse = true;
    kind.alwaysWritten = {"text", "status"};
    SchemaMember text;
    text.key = "text";
    text.type = ValueType::String;
    text.defaultValue = "";
    SchemaMember status;
    status.key = "status";
    status.type = ValueType::Enum;
    status.options = {"translated", "needs_review"};
    status.defaultValue = "translated";
    SchemaMember size;
    size.key = "size_scale";
    size.type = ValueType::Number;
    size.defaultValue = 1.0;
    size.min = 0.1;
    size.max = 10;
    SchemaMember tracking = size;
    tracking.key = "tracking";
    tracking.min = -1000;
    tracking.max = 1000;
    tracking.defaultValue = 0;
    kind.members = {text, status, size, tracking};
    ASSERT_TRUE(schema.addKind(kind));
    SchemaMember map;
    map.key = "test_translations";
    map.type = ValueType::ObjectMap;
    map.kind = "test_translation";
    map.mapKeys = MapKeys::LanguageCode;
    map.defaultValue = Json::object();
    ASSERT_TRUE(schema.addMember("layer.text", map));

    TitleArtDoc doc = makeNewTitleArt();
    doc.layers()[0]["test_translations"] = Json{{"de", {{"tracking", 5000}, {"text", "Titel"}}}};
    normalizeTitleArt(doc);
    const Json& de = doc.layers()[0]["test_translations"]["de"];
    std::vector<std::string> keys;
    for (auto it = de.begin(); it != de.end(); ++it) keys.push_back(it.key());
    EXPECT_EQ(keys, (std::vector<std::string>{"text", "status", "tracking"})) << "only the members present, in table order";
    EXPECT_EQ(de["tracking"], 1000) << "and each normalized";
    EXPECT_FALSE(de.contains("size_scale")) << "a default is never frozen into an override";
    EXPECT_NE(writeTitleArt(doc).find("\"status\": \"translated\""), std::string::npos);

    EXPECT_TRUE(schema.removeMember("layer.text", "test_translations"));
    EXPECT_TRUE(schema.removeKind("test_translation"));
}

TEST_F(TitleArtSchemaTest, ReservedNamesNameTheirOwner) {
    const TitleArtSchema& schema = TitleArtSchema::get();
    // S86's names are reserved until its 3D module registers them
#if defined(JASPERENGINE_TITLE_ART_3D) && JASPERENGINE_TITLE_ART_3D
    const std::string S86_OWNER;
#else
    const std::string S86_OWNER = "S86";
#endif
    // S85 registered `animation`, the layer's tint and flash, transform.stretch and glyph_mode
    EXPECT_EQ(schema.reservedOwner("document", "animation"), "");
    const SchemaMember* animation = schema.member(*schema.kind("document"), "animation");
    ASSERT_NE(animation, nullptr);
    EXPECT_EQ(animation->owner, "S85");
    EXPECT_TRUE(animation->omitDefault);
    // S81 registered `runtime`: the member names its owner, the reservation is gone
    EXPECT_EQ(schema.reservedOwner("document", "runtime"), "");
    const SchemaMember* runtime = schema.member(*schema.kind("document"), "runtime");
    ASSERT_NE(runtime, nullptr);
    EXPECT_EQ(runtime->owner, "S81");
    EXPECT_EQ(schema.reservedOwner("document", "scene_3d"), S86_OWNER);
    EXPECT_EQ(schema.reservedOwner("document", "color_schemes"), "S84");
    EXPECT_EQ(schema.reservedOwner("document", "languages"), "S87");
    EXPECT_EQ(schema.reservedOwner("transform", "z"), S86_OWNER);
    EXPECT_EQ(schema.reservedOwner("layer.text", "translations"), "S87");
    EXPECT_EQ(schema.reservedOwner("layer.text", "only_languages"), "S87") << "a variant has its base's reservations";
    EXPECT_EQ(schema.reservedOwner("layer.text", "tint"), "");
    EXPECT_EQ(schema.reservedOwner("char_adjust", "rig"), S86_OWNER);
    EXPECT_EQ(schema.reservedOwner("document", "variants"), "") << "S87 released `variants`";
}

TEST_F(TitleArtSchemaTest, FieldPathsParse) {
    std::vector<FieldSegment> path;
    ASSERT_TRUE(parseFieldPath("style.rims[1].width", path, nullptr));
    ASSERT_EQ(path.size(), 4u);
    EXPECT_EQ(path[0].key, "style");
    EXPECT_TRUE(path[2].isIndex);
    EXPECT_EQ(path[2].index, 1u);
    EXPECT_EQ(formatFieldPath(path), "style.rims[1].width");
    ASSERT_TRUE(parseFieldPath("part_effects.rim:0000000a[0].params.size", path, nullptr));
    EXPECT_EQ(path[1].key, "rim:0000000a");
    std::string error;
    EXPECT_FALSE(parseFieldPath("style..face", path, &error));
    EXPECT_FALSE(parseFieldPath("rims[x]", path, &error));
    EXPECT_FALSE(parseFieldPath("rims[1", path, &error));
    EXPECT_FALSE(parseFieldPath("", path, &error));
    EXPECT_EQ(nearestName("bevl", {"fill", "bevel", "gloss"}), "bevel");
    EXPECT_EQ(nearestName("zzzzzz", {"fill", "bevel"}), "");
}

TEST_F(TitleArtSchemaTest, PatternsListLeaves) {
    const std::vector<std::string> patterns = TitleArtSchema::get().patterns("rim");
    EXPECT_NE(std::find(patterns.begin(), patterns.end(), "width"), patterns.end());
    EXPECT_NE(std::find(patterns.begin(), patterns.end(), "material.bevel.size"), patterns.end());
    EXPECT_NE(std::find(patterns.begin(), patterns.end(), "material.fill.gradient.stops[*].color"), patterns.end());
    for (const std::string& p : patterns) {
        EXPECT_TRUE(TitleArtSchema::get().resolvePattern("rim", p)) << p << " resolves back";
    }
}
