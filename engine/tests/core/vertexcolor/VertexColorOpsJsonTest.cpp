// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// docs/specs/V1-vertex-colors.md R120, R125, Table 1, D10: the op JSON that edit_mesh_colors and Lua
// applyOps share, and the result it returns.

#include "VertexColorTestSupport.h"

#include "vertexcolor/VertexColorOpsJson.h"

using namespace jasperengine;

// (a named namespace, tests included: a unity build merges the test sources)
namespace vertex_color_ops_json_test {
    using namespace jasperengine::vertexcolor;
    using namespace jasperengine::vertexcolor::test;
    using Json = nlohmann::json;

    bool parseOne(const std::string& text, Op& op, std::string& error, OpJsonHooks hooks = OpJsonHooks()) {
        OpJsonParser parser(std::move(hooks));
        const bool ok = parser.parse(Json::parse(text), 0, op);
        error = parser.error;
        return ok;
    }

    Op parsed(const std::string& text, OpJsonHooks hooks = OpJsonHooks()) {
        Op op;
        std::string error;
        EXPECT_TRUE(parseOne(text, op, error, std::move(hooks))) << text << "\n" << error;
        return op;
    }

    std::string errorOf(const std::string& text, OpJsonHooks hooks = OpJsonHooks()) {
        Op op;
        std::string error;
        EXPECT_FALSE(parseOne(text, op, error, std::move(hooks))) << text;
        return error;
    }

    OpJsonHooks editorHooks() {
        OpJsonHooks hooks;
        hooks.loadImage = [](const Json& item, std::string& field, std::string& error) -> std::shared_ptr<const Image> {
            auto image = std::make_shared<Image>();
            image->width = image->height = 1;
            image->pixels = {Vector4(1, 0, 0, 1)};
            return image;
        };
        hooks.loadStencil = [](const std::string& path, std::string& error) -> std::shared_ptr<const Image> {
            if (path != "stencils/rock.png") {
                error = "no texture at " + path;
                return nullptr;
            }
            return std::make_shared<Image>();
        };
        hooks.transferSource = [](const Json& value, std::string& error) -> std::shared_ptr<const TransferSource> {
            return std::make_shared<TransferSource>();
        };
        hooks.selection = [](Target& target, std::string& error) {
            target.kind = TargetKind::Faces;
            target.ids = {4, 5};
            return true;
        };
        return hooks;
    }

TEST(VertexColorOpsJson, ParsesSetFillAndTheirChannels) {
    Op op = parsed(R"({"op": "set", "target": {"kind": "faces", "ids": [3, 1]}, "value": [1, 0, 0]})");
    EXPECT_EQ(op.type, OpType::Set);
    EXPECT_EQ(op.target.kind, TargetKind::Faces);
    EXPECT_EQ(op.target.ids, std::vector<uint32_t>({3, 1}));
    EXPECT_EQ(op.value, Vector4(1, 0, 0, 1));
    EXPECT_EQ(op.channels, CHANNELS_RGB) << "R21: an RGB value writes rgb";
    op = parsed(R"({"op": "fill", "value": [0, 1, 0, 0.5], "blend": "multiply", "strength": 0.25})");
    EXPECT_EQ(op.channels, CHANNELS_RGBA);
    EXPECT_EQ(op.blend, Blend::Multiply);
    EXPECT_EQ(op.strength, 0.25f);
    EXPECT_EQ(op.target.kind, TargetKind::All);
    op = parsed(R"({"op": "fill", "value": "#FF000080", "channels": "ga"})");
    EXPECT_EQ(op.value, Vector4(1, 0, 0, 128.0f / 255.0f));
    EXPECT_EQ(op.channels, CHANNEL_G | CHANNEL_A);
    EXPECT_EQ(parsed(R"({"op": "fill", "value": "#00FF00"})").channels, CHANNELS_RGB);
}

TEST(VertexColorOpsJson, ParsesEveryTargetKind) {
    Op op = parsed(R"({"op": "fill", "value": [1, 1, 1], "target": {"kind": "sphere", "center": [1, 2, 3], "radius": 0.5, "element": "face"}})");
    EXPECT_EQ(op.target.kind, TargetKind::Sphere);
    EXPECT_EQ(op.target.center, Vector3(1, 2, 3));
    EXPECT_EQ(op.target.element, Element::Face);
    op = parsed(R"({"op": "fill", "value": [1, 1, 1], "target": {"kind": "box", "center": [0, 0, 0], "half_extents": [1, 2, 3], "rotation": [0, 90, 0]}})");
    EXPECT_EQ(op.target.kind, TargetKind::Box);
    EXPECT_EQ(op.target.halfExtents, Vector3(1, 2, 3));
    EXPECT_NEAR(op.target.rotation.y, std::sqrt(0.5f), 1e-6f);
    op = parsed(R"({"op": "fill", "value": [1, 1, 1], "target": {"kind": "capsule", "from": [0, 0, 0], "to": [0, 1, 0], "radius": 0.2}})");
    EXPECT_EQ(op.target.kind, TargetKind::Capsule);
    EXPECT_EQ(op.target.to, Vector3(0, 1, 0));
    op = parsed(R"({"op": "fill", "value": [1, 1, 1], "target": {"kind": "color", "value": [1, 0, 0, 1], "tolerance": 0.1, "ignore_v": true, "coverage": 0.5}})");
    EXPECT_EQ(op.target.kind, TargetKind::Color);
    EXPECT_FALSE(op.target.color.ignoreAlpha) << "an RGBA match compares alpha";
    EXPECT_TRUE(op.target.color.ignoreV);
    EXPECT_EQ(op.target.color.coverage, 0.5f);
    EXPECT_EQ(parsed(R"({"op": "fill", "value": [1, 1, 1], "target": {"kind": "corners", "ids": [7]}})").target.kind, TargetKind::Corners);
    EXPECT_EQ(parsed(R"({"op": "fill", "value": [1, 1, 1], "target": {"kind": "vertices", "ids": []}})").target.kind, TargetKind::Vertices);
    op = parsed(R"({"op": "fill", "value": [1, 1, 1], "target": {"kind": "selection"}})", editorHooks());
    EXPECT_EQ(op.target.kind, TargetKind::Faces);
    EXPECT_EQ(op.target.ids, std::vector<uint32_t>({4, 5}));
}

TEST(VertexColorOpsJson, ParsesFiltersAsOneOrAList) {
    Op op = parsed(R"({"op": "fill", "value": [1, 1, 1], "filter": {"kind": "facing", "direction": [0, 0, 1], "angle": 30, "invert": true}})");
    ASSERT_EQ(op.filters.size(), 1u);
    EXPECT_EQ(op.filters[0].kind, FilterKind::Facing);
    EXPECT_TRUE(op.filters[0].invert);
    EXPECT_EQ(op.filters[0].angle, 30.0f);
    op = parsed(R"({"op": "fill", "value": [1, 1, 1], "filter": [{"kind": "slope", "max": 35}, {"kind": "height", "min": 2},
                    {"kind": "curvature", "min": 0.2}, {"kind": "channel", "channel": "a", "max": 0.5}, {"kind": "occluded"}]})");
    ASSERT_EQ(op.filters.size(), 5u);
    EXPECT_EQ(op.filters[0].min, 0.0f);
    EXPECT_EQ(op.filters[0].max, 35.0f);
    EXPECT_EQ(op.filters[1].min, 2.0f);
    EXPECT_GT(op.filters[1].max, 1e30f);
    EXPECT_EQ(op.filters[2].max, 1.0f);
    EXPECT_EQ(op.filters[3].channel, 3);
    EXPECT_EQ(op.filters[4].kind, FilterKind::Occluded);
}

TEST(VertexColorOpsJson, ParsesStrokeAndItsOptions) {
    const Op op = parsed(R"({"op": "stroke", "points": [{"p": [0, 1, 0], "pressure": 0.5}, [1, 1, 0]], "radius": 0.3, "value": [0, 0, 1],
                             "falloff": "sphere", "hardness": 0.4, "spacing": 0.1, "buildup": true, "pressure_size": true,
                             "stencil": "stencils/rock.png", "stencil_scale": 2, "stencil_rotation": 45, "element": "face"})",
                         editorHooks());
    ASSERT_EQ(op.points.size(), 2u);
    EXPECT_EQ(op.points[0].pressure, 0.5f);
    EXPECT_EQ(op.points[1].position, Vector3(1, 1, 0));
    EXPECT_EQ(op.points[1].pressure, 1.0f);
    EXPECT_EQ(op.radius, 0.3f);
    EXPECT_EQ(op.falloff, FalloffPreset::Sphere);
    EXPECT_TRUE(op.buildup);
    EXPECT_TRUE(op.pressureSize);
    EXPECT_NE(op.stencil, nullptr);
    EXPECT_EQ(op.stencilRotation, 45.0f);
    EXPECT_EQ(op.target.element, Element::Face);
    EXPECT_EQ(op.channels, CHANNELS_RGB);
    const Op erase = parsed(R"({"op": "stroke", "points": [[0, 0, 0]], "radius": 1, "mode": "erase", "falloff_curve": [[0, 1], [1, 0.5]]})");
    EXPECT_EQ(erase.mode, StrokeMode::Erase);
    EXPECT_EQ(erase.falloff, FalloffPreset::Custom);
    EXPECT_FLOAT_EQ(erase.falloffCurve.evaluate(0.5f), 0.75f);
    EXPECT_EQ(erase.channels, CHANNELS_RGBA);
}

TEST(VertexColorOpsJson, ParsesGradientNoiseAndSmooth) {
    Op op = parsed(R"({"op": "gradient", "type": "radial", "from": [0, 0, 0], "to": [0, 2, 0],
                       "stops": [{"t": 0, "value": "#000000"}, {"t": 1, "value": [1, 1, 1, 1], "strength": 0.5}],
                       "interpolation": "hsv", "hue_direction": "ccw", "extend": "off", "element": "face"})");
    EXPECT_EQ(op.gradientType, GradientType::Radial);
    ASSERT_EQ(op.stops.size(), 2u);
    EXPECT_EQ(op.stops[1].strength, 0.5f);
    EXPECT_EQ(op.interpolation, Interpolation::Hsv);
    EXPECT_EQ(op.hueDirection, HueDirection::Ccw);
    EXPECT_EQ(op.extend, Extend::Off);
    EXPECT_EQ(op.target.element, Element::Face);
    EXPECT_EQ(op.channels, CHANNELS_RGBA);
    op = parsed(R"({"op": "gradient", "type": "axis", "axis": [1, 0, 0], "stops": [{"t": 0, "value": [0, 0, 0]}, {"t": 1, "value": [1, 1, 1]}]})");
    EXPECT_EQ(op.axis, Vector3(1, 0, 0));
    EXPECT_EQ(op.channels, CHANNELS_RGB);
    op = parsed(R"({"op": "noise", "kind": "cellular", "scale": 2, "octaves": 5, "seed": 42, "space": "local", "high": [0, 1, 0]})");
    EXPECT_EQ(op.noiseKind, NoiseKind::Cellular);
    EXPECT_EQ(op.noiseScale, 2.0f);
    EXPECT_EQ(op.octaves, 5);
    EXPECT_EQ(op.seed, 42u);
    EXPECT_FALSE(op.noiseWorld);
    EXPECT_EQ(op.value, Vector4(0, 0, 0, 1));
    EXPECT_EQ(op.value2, Vector4(0, 1, 0, 1));
    op = parsed(R"({"op": "smooth", "iterations": 4, "strength": 0.5})");
    EXPECT_EQ(op.iterations, 4);
    EXPECT_EQ(parsed(R"({"op": "sharpen"})").type, OpType::Sharpen);
}

TEST(VertexColorOpsJson, ParsesAdjustAndChannelVariants) {
    Op op = parsed(R"({"op": "adjust", "levels": {"in": [0.1, 0.9], "gamma": 2, "out": [0, 0.5]}})");
    EXPECT_EQ(op.adjustKind, AdjustKind::Levels);
    EXPECT_EQ(op.inBlack, 0.1f);
    EXPECT_EQ(op.outWhite, 0.5f);
    EXPECT_EQ(op.gamma, 2.0f);
    EXPECT_EQ(op.channels, CHANNELS_RGB);
    op = parsed(R"({"op": "adjust", "brightness_contrast": {"brightness": 0.2, "contrast": -0.5}})");
    EXPECT_EQ(op.contrast, -0.5f);
    op = parsed(R"({"op": "adjust", "hsv": {"hue": 90, "saturation": -0.2}})");
    EXPECT_EQ(op.hueShift, 90.0f);
    EXPECT_EQ(parsed(R"({"op": "adjust", "invert": true})").adjustKind, AdjustKind::Invert);
    op = parsed(R"({"op": "adjust", "clamp": [0.2, 0.8]})");
    EXPECT_EQ(op.adjustKind, AdjustKind::Clamp);
    EXPECT_EQ(op.outBlack, 0.2f);

    op = parsed(R"({"op": "channel", "copy": {"from": "r", "to": "a"}})");
    EXPECT_EQ(op.channelKind, ChannelOpKind::Copy);
    EXPECT_EQ(op.sourceChannel, 0);
    EXPECT_EQ(op.destChannel, 3);
    op = parsed(R"({"op": "channel", "swap": ["g", "b"]})");
    EXPECT_EQ(op.channelKind, ChannelOpKind::Swap);
    EXPECT_EQ(op.sourceChannel, 1);
    EXPECT_EQ(op.destChannel, 2);
    op = parsed(R"({"op": "channel", "invert": "a"})");
    EXPECT_EQ(op.channelKind, ChannelOpKind::Invert);
    EXPECT_EQ(op.channels, CHANNEL_A);
    op = parsed(R"({"op": "channel", "pack": {"r": "g", "a": 0.5}})");
    EXPECT_EQ(op.channelKind, ChannelOpKind::Pack);
    EXPECT_EQ(op.packSource[0], 1);
    EXPECT_EQ(op.packSource[3], -1);
    EXPECT_EQ(op.packConstant[3], 0.5f);
    EXPECT_EQ(op.channels, CHANNEL_R | CHANNEL_A);
}

TEST(VertexColorOpsJson, ParsesRecolorBakesDirectionTransferAndClear) {
    Op op = parsed(R"({"op": "replace_color", "from": "#FF0000", "to": [0, 0, 1], "tolerance": 0.05, "ignore_s": true})");
    EXPECT_EQ(op.type, OpType::ReplaceColor);
    EXPECT_EQ(op.target.color.value, Vector4(1, 0, 0, 1));
    EXPECT_EQ(op.target.color.tolerance, 0.05f);
    EXPECT_TRUE(op.target.color.ignoreS);
    EXPECT_EQ(op.value, Vector4(0, 0, 1, 1));
    op = parsed(R"({"op": "from_texture", "material_slot": "albedo", "uv_set": 1, "samples": 32, "keep_seams": false, "seam_delta_e": 4})", editorHooks());
    EXPECT_NE(op.image, nullptr);
    EXPECT_EQ(op.uvSet, 1);
    EXPECT_EQ(op.samples, 32);
    EXPECT_FALSE(op.keepSeams);
    EXPECT_EQ(op.seamDeltaE, 4.0f);
    op = parsed(R"({"op": "bake_ao", "rays": 128, "distance": 2, "scope": "scene", "seed": 9})");
    EXPECT_EQ(op.rays, 128);
    EXPECT_TRUE(op.sceneScope);
    EXPECT_EQ(op.seed, 9u);
    EXPECT_EQ(op.channels, CHANNELS_RGB);
    EXPECT_EQ(parsed(R"({"op": "bake_thickness"})").type, OpType::BakeThickness);
    op = parsed(R"({"op": "bake_curvature", "radius_edges": 3, "contrast": 2})");
    EXPECT_EQ(op.iterations, 3);
    EXPECT_EQ(op.contrast, 2.0f);
    op = parsed(R"({"op": "by_direction", "direction": [0, -1, 0], "range": [0.2, 0.9], "value": [0.3, 0.2, 0.1]})");
    EXPECT_EQ(op.direction, Vector3(0, -1, 0));
    EXPECT_EQ(op.rangeMax, 0.9f);
    op = parsed(R"({"op": "transfer", "source_entity": 12, "max_distance": 0.5, "normal_angle": 30, "unmatched": "nearest"})", editorHooks());
    EXPECT_NE(op.source, nullptr);
    EXPECT_EQ(op.unmatched, Unmatched::Nearest);
    EXPECT_EQ(parsed(R"({"op": "clear", "to": "black", "target": {"kind": "faces", "ids": [0]}})").clearTo, ClearTo::Black);
}

TEST(VertexColorOpsJson, ErrorsNameTheOpTheFieldAndTheFix) {
    EXPECT_EQ(errorOf(R"({"op": "paint"})").rfind("ops[0].op: \"paint\" is not an operation. Operations: set, fill, stroke,", 0), 0u);
    EXPECT_EQ(errorOf(R"({"op": "set", "value": [1, 1, 1], "radius": 2})"),
              "ops[0].radius: is not a field of set. Fields: op, target, filter, channels, value (jasper://docs/meshes).");
    EXPECT_EQ(errorOf(R"({"op": "fill", "value": [1, 1, 1], "strength": 2})"), "ops[0].strength: must be from 0 to 1, got 2; 1 is the full change.");
    EXPECT_EQ(errorOf(R"({"op": "stroke", "points": [[0, 0, 0]], "radius": 0, "value": [1, 1, 1]})"),
              "ops[0].radius: must be > 0, got 0; give the brush radius in units.");
    EXPECT_EQ(errorOf(R"({"op": "fill", "value": [1, 1, 1], "target": {"kind": "sphere", "center": [0, 0, 0], "radius": -1}})"),
              "ops[0].target.radius: must be > 0, got -1; give the radius in units.");
    EXPECT_EQ(errorOf(R"({"op": "from_texture", "texture": "a.png", "samples": 0})", editorHooks()), "ops[0].samples: must be a whole number from 1 to 256.");
    EXPECT_EQ(errorOf(R"({"op": "bake_ao", "rays": 2000})"), "ops[0].rays: must be a whole number from 1 to 1024.");
    EXPECT_EQ(errorOf(R"({"op": "fill", "value": [1, 1, 1], "target": {"kind": "faces", "ids": [-1]}})"),
              "ops[0].target.ids: must be whole numbers from 0; read the ids with sample_mesh_colors elements.");
    EXPECT_EQ(errorOf(R"({"op": "fill", "value": [1, 1, 1], "target": {"kind": "faces", "ids": [1], "radius": 2}})").rfind(
                  "ops[0].target.radius: is not a field of a faces target. Fields: kind, ids", 0),
              0u);
    EXPECT_EQ(errorOf(R"({"op": "fill", "value": [1, 2]})"), "ops[0].value: must be [r, g, b] or [r, g, b, a] (linear 0..1), or \"#RRGGBB[AA]\" (sRGB).");
    EXPECT_NE(errorOf(R"({"op": "fill", "value": [1, 1, 1], "channels": "rgbx"})").find("ops[0].channels: \"rgbx\" is not a subset of \"rgba\""), std::string::npos);
    EXPECT_EQ(errorOf(R"({"op": "fill", "value": [1, 1, 1], "filter": {"kind": "wet"}})").rfind("ops[0].filter.kind: \"wet\" is not one of", 0), 0u);
    EXPECT_EQ(errorOf(R"({"op": "fill", "value": [1, 1, 1], "blend": "darken"})").rfind("ops[0].blend: \"darken\" is not one of \"replace\"", 0), 0u);
    EXPECT_EQ(errorOf(R"({"op": "gradient", "stops": [{"t": 0, "value": [0, 0, 0]}], "from": [0, 0, 0], "to": [1, 0, 0]})").rfind("ops[0].stops: needs at least 2 stops", 0), 0u);
    EXPECT_EQ(errorOf(R"({"op": "adjust", "invert": true, "clamp": [0, 1]})"), "ops[0]: adjust takes exactly one of levels, brightness_contrast, hsv, invert, clamp.");
    EXPECT_EQ(errorOf(R"({"op": "fill"})"), "ops[0].value: is required: [r, g, b(, a)] linear 0..1, or \"#RRGGBB[AA]\".");
    EXPECT_EQ(errorOf(R"({"op": "stroke", "points": [[0, 0, 0]], "radius": 1, "mode": "smooth", "value": [1, 1, 1]})"),
              "ops[0].value: is for mode \"paint\"; erase, smooth and sharpen take no value.");
}

TEST(VertexColorOpsJson, NullHooksRefuseNamingTheField) {
    EXPECT_EQ(errorOf(R"({"op": "from_texture", "texture": "rock.png"})"), "ops[0].texture: textures are read by the editor; this caller cannot load them.");
    EXPECT_EQ(errorOf(R"({"op": "from_texture", "material_slot": "albedo"})"), "ops[0].material_slot: textures are read by the editor; this caller cannot load them.");
    EXPECT_EQ(errorOf(R"({"op": "stroke", "points": [[0, 0, 0]], "radius": 1, "value": [1, 1, 1], "stencil": "s.png"})"),
              "ops[0].stencil: stencil textures are read by the editor; this caller cannot load them.");
    EXPECT_EQ(errorOf(R"({"op": "transfer", "source_entity": 3})"), "ops[0].source_entity: transfer reads another entity's colors; only the editor can.");
    EXPECT_EQ(errorOf(R"({"op": "fill", "value": [1, 1, 1], "target": {"kind": "selection"}})"),
              "ops[0].target.kind: \"selection\" needs the editor's element selection; name the ids with a vertices, faces or corners target.");
    // a hook's own refusal is passed on
    EXPECT_EQ(errorOf(R"({"op": "stroke", "points": [[0, 0, 0]], "radius": 1, "value": [1, 1, 1], "stencil": "missing.png"})", editorHooks()),
              "ops[0].stencil: no texture at missing.png");
}

TEST(VertexColorOpsJson, ListErrorsCountFromTheCallersFirstIndex) {
    const Json list = Json::parse(R"([{"op": "fill", "value": [1, 1, 1]}, {"op": "smooth", "iterations": 0}])");
    std::vector<Op> ops;
    OpJsonParser json{OpJsonHooks()};
    EXPECT_FALSE(json.parseList(list, ops));
    EXPECT_EQ(json.error, "ops[1].iterations: must be a whole number from 1 to 100.");
    OpJsonHooks lua;
    lua.firstIndex = 1;
    OpJsonParser luaParser(lua);
    EXPECT_FALSE(luaParser.parseList(list, ops));
    EXPECT_EQ(luaParser.error, "ops[2].iterations: must be a whole number from 1 to 100.");
    EXPECT_FALSE(json.parseList(Json::array(), ops));
    EXPECT_EQ(json.error.rfind("ops must be a non-empty list", 0), 0u);
    ASSERT_TRUE(json.parseList(Json::parse(R"([{"op": "fill", "value": [1, 1, 1]}, {"op": "clear"}])"), ops));
    EXPECT_EQ(ops.size(), 2u);
}

TEST(VertexColorOpsJson, ParsedOpsRunAndTheResultSerializes) {
    const MeshView mesh = sphere();
    std::vector<Op> ops;
    OpJsonParser parser{OpJsonHooks()};
    ASSERT_TRUE(parser.parseList(Json::parse(R"([{"op": "set", "target": {"kind": "faces", "ids": [0, 1]}, "value": "#FF0000"},
                                                  {"op": "fill", "value": [0, 0, 1], "strength": 0.5, "filter": {"kind": "height", "max": 0}}])"),
                                 ops))
        << parser.error;
    TargetData data;
    const OpsResult result = applyOps(data, mesh, ops, localOptions(Format::RGBA8));
    ASSERT_TRUE(result.ok) << result.error;
    const Json json = toJson(result);
    EXPECT_EQ(json["ok"], true);
    ASSERT_EQ(json["ops"].size(), 2u);
    EXPECT_EQ(json["ops"][0]["split_added"], result.ops[0].splitAdded);
    EXPECT_EQ(json["ops"][0]["changed_corners"], 6u);
    EXPECT_EQ(json["ops"][1]["in_target"], result.ops[1].inTarget);
    EXPECT_EQ(json["ops"][1]["filtered_out"], result.ops[1].filteredOut);
    for (const char* key : {"changed_vertices", "mean_delta_e", "p95_delta_e", "under_sampled_faces", "unmatched", "mean_distance"})
        EXPECT_TRUE(json["ops"][0].contains(key)) << key;
    EXPECT_EQ(json["hash"], hex(result.hash));
    EXPECT_EQ(json["hash"].get<std::string>().size(), 16u);
    EXPECT_EQ(json["stats"]["before"]["count"], 240u);
    EXPECT_EQ(json["stats"]["before"]["mean"][0], 1.0);
    EXPECT_EQ(json["stats"]["after"]["histogram"].size(), 4u);
    EXPECT_EQ(json["stats"]["after"]["histogram"][0].size(), 16u);
    ASSERT_EQ(json["warnings"].size(), 1u);
    EXPECT_EQ(json["warnings"][0]["code"], "split_vertices_added");
    EXPECT_FALSE(json.contains("error"));

    Op bad = ops[0];
    bad.target.ids = {999};
    const Json failed = toJson(applyOps(data, mesh, std::span<const Op>(&bad, 1), localOptions(Format::RGBA8)));
    EXPECT_EQ(failed["ok"], false);
    EXPECT_EQ(failed["failed_op"], 0u);
    EXPECT_EQ(failed["error"].get<std::string>().rfind("ops[0].target.ids: 999 is out of range", 0), 0u);
}

TEST(VertexColorOpsJson, ValuesAndChannelsHaveOneSpelling) {
    Vector4 v;
    std::string error;
    EXPECT_TRUE(parseValue(Json::parse("[0.5, 0.25, 0]"), v, error));
    EXPECT_EQ(v, Vector4(0.5f, 0.25f, 0, 1));
    EXPECT_FALSE(parseValue(Json::parse("\"red\""), v, error));
    EXPECT_NE(error.find("is not a color"), std::string::npos);
    ChannelMask mask = 0;
    EXPECT_TRUE(parseChannels(Json("ba"), mask, error));
    EXPECT_EQ(mask, CHANNEL_B | CHANNEL_A);
    EXPECT_FALSE(parseChannels(Json("rr"), mask, error));
    EXPECT_FALSE(parseChannels(Json(""), mask, error));
    EXPECT_EQ(channelsName(CHANNEL_R | CHANNEL_B), "rb");
    EXPECT_EQ(channelsName(CHANNELS_RGBA), "rgba");
}

} // namespace vertex_color_ops_json_test
