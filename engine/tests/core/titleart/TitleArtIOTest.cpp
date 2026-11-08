// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include "TestPaths.h"
#include "support/AllocationCounter.h"
#include "titleart/TitleArtIO.h"
#include "titleart/TitleArtSchema.h"
#include "titleart/TitleEffectRegistry.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>

using namespace jasperengine;
using namespace jasperengine::titleart;
namespace fs = std::filesystem;

namespace {

    std::string readFixture(const std::string& name) {
        const fs::path path = fs::path(test::testPaths().sourceDir) / "titleart" / "fixtures" / "v1" / name;
        std::ifstream file(path, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    }

    bool hasWarning(const std::vector<TitleWarning>& warnings, const std::string& path, const std::string& part) {
        for (const TitleWarning& w : warnings) {
            if (w.path == path && w.message.find(part) != std::string::npos) return true;
        }
        return false;
    }

    class TitleArtIOTest : public ::testing::Test {};

}

TEST_F(TitleArtIOTest, CanonicalRoundTrip) {
    for (const char* name : {"minimal.titleart", "every-layer-type.titleart", "unknown-data.titleart"}) {
        const TitleArtParseResult first = parseTitleArt(readFixture(name));
        ASSERT_TRUE(first.ok) << name << ": " << first.error.toString();
        const std::string text = writeTitleArt(first.doc);
        const TitleArtParseResult second = parseTitleArt(text);
        ASSERT_TRUE(second.ok) << name;
        EXPECT_TRUE(second.warnings.empty()) << name << ": a canonical file reads without warnings";
        EXPECT_EQ(writeTitleArt(second.doc), text) << name << ": write(parse(write(d))) == write(d)";
    }
}

TEST_F(TitleArtIOTest, CanonicalTextFormatting) {
    TitleArtDoc doc = makeNewTitleArt();
    const std::string text = writeTitleArt(doc);
    // the header members first, in order, 2-space indent, one member per line
    EXPECT_EQ(text.rfind("{\n  \"format\": \"jasperengine.titleart\",\n  \"version\": 1,\n  \"guid\": \"", 0), 0u) << text.substr(0, 120);
    // arrays of numbers on one line, integers plain, numbers %.7g
    EXPECT_NE(text.find("\"position\": [0, 0]"), std::string::npos);
    EXPECT_NE(text.find("\"line_height\": 1.1,"), std::string::npos);
    EXPECT_NE(text.find("\"color\": \"#ffffff\""), std::string::npos);
    EXPECT_EQ(text.back(), '\n');
    EXPECT_EQ(text.find('\t'), std::string::npos);
}

TEST_F(TitleArtIOTest, EveryMemberIsWrittenWithItsDefault) {
    const TitleArtParseResult parsed = parseTitleArt(readFixture("minimal.titleart"));
    ASSERT_TRUE(parsed.ok);
    const Json& root = parsed.doc.root;
    EXPECT_EQ(root["canvas"]["width"], 1920);
    EXPECT_EQ(root["canvas"]["height"], 1080);
    EXPECT_EQ(root["canvas"]["background"]["transparent"], true);
    EXPECT_EQ(root["global_light"]["angle"], 120);
    EXPECT_EQ(root["global_light"]["altitude"], 30);
    EXPECT_EQ(root["seed"], 1);
    ASSERT_EQ(root["outputs"].size(), 1u);
    const Json& main = root["outputs"][0];
    EXPECT_EQ(main["id"], "main");
    EXPECT_EQ(main["content"], "all");
    EXPECT_EQ(main["round_to"], 4);
    EXPECT_EQ(main["bit_depth"], 8);
    EXPECT_EQ(main["max_size"], 4096);
    EXPECT_EQ(main["alpha_bleed"], true);
    std::vector<std::string> order;
    for (auto it = root.begin(); it != root.end(); ++it) order.push_back(it.key());
    std::vector<std::string> expected = {"format", "version", "guid", "name", "canvas", "global_light", "seed",
                                         "material_links", "layers", "exposed", "outputs", "runtime",
                                         // S87's members (TitleTranslations.cpp)
                                         "source_language", "languages", "string_namespace", "dpi", "import_notes"};
#if defined(JASPERENGINE_TITLE_ART_3D) && JASPERENGINE_TITLE_ART_3D
    expected.push_back("scene_3d");     // S86's member (null in a 2D document)
#endif
    EXPECT_EQ(order, expected);
}

TEST_F(TitleArtIOTest, EveryLayerTypeTakesItsDefaults) {
    const TitleArtParseResult parsed = parseTitleArt(readFixture("every-layer-type.titleart"));
    ASSERT_TRUE(parsed.ok) << parsed.error.toString();
    const Json& root = parsed.doc.root;
    const Json* text = findLayer(root, "00000003");
    ASSERT_TRUE(text);
    EXPECT_EQ((*text)["tracking"], 0);
    EXPECT_EQ((*text)["line_height"], 1.1);
    EXPECT_EQ((*text)["align"], "center");
    EXPECT_EQ((*text)["tate_chu_yoko_max"], 3);
    EXPECT_EQ((*text)["warp"]["style"], "none");
    EXPECT_EQ((*text)["distort"]["corners"].size(), 4u);
    EXPECT_EQ((*text)["distort"]["mesh"].size(), 16u);
    EXPECT_EQ((*text)["dimension"], "2d");
    EXPECT_EQ((*text)["transform"]["scale"], 1);
    // the material: Pachislop's defaults, the members set kept
    const Json& face = (*text)["style"]["face"];
    EXPECT_EQ(face["fill"]["color"], "#ffd060");
    EXPECT_EQ(face["bevel"]["size"], 4);
    EXPECT_EQ(face["bevel"]["profile_curve"]["points"].size(), 3u);
    EXPECT_EQ(face["bevel"]["highlight_opacity"], 0.85);
    EXPECT_EQ(face["satin"]["contour"]["points"][1]["y"], 1);
    EXPECT_EQ(face["glass"]["color"], "#3cc8ff");
    EXPECT_EQ(face["sheen"]["position"], -0.25);
    EXPECT_EQ(face["fill"]["gradient"]["stops"].size(), 2u);
    // rims, plates, char adjustments with their own face
    const Json& rims = (*text)["style"]["rims"];
    ASSERT_EQ(rims.size(), 2u);
    EXPECT_EQ(rims[0]["join"], "miter");
    EXPECT_EQ(rims[1]["plate"]["kind"], "box");
    EXPECT_EQ(rims[1]["plate"]["corner"], 4);
    EXPECT_EQ((*text)["chars"]["1"]["face"]["fill"]["kind"], "linear");
    EXPECT_EQ((*text)["chars"]["1"]["scale"], 1);
    // the other layer types
    const Json* shape = findLayer(root, "00000004");
    ASSERT_TRUE(shape);
    EXPECT_EQ((*shape)["shape"], "star");
    EXPECT_EQ((*shape)["tail"]["y"], -60);
    EXPECT_EQ((*shape)["name"], "Badge");
    const Json* image = findLayer(root, "00000005");
    ASSERT_TRUE(image);
    EXPECT_EQ((*image)["trace"]["threshold"], 128);
    EXPECT_EQ((*image)["style"]["face"]["fill"]["kind"], "source") << "an image's face shows its own pixels";
    EXPECT_EQ((*image)["masks"][0]["mid"], 0.5);
    const Json* vector = findLayer(root, "00000006");
    ASSERT_TRUE(vector);
    EXPECT_EQ((*vector)["style"]["face"]["fill"]["kind"], "source") << "artwork mode shows the SVG's own colours";
    EXPECT_EQ((*vector)["masks"][0]["channel"], "luminance");
    const Json* adjustment = findLayer(root, "00000007");
    ASSERT_TRUE(adjustment);
    EXPECT_EQ((*adjustment)["adjustment"]["channels"]["rgb"]["gamma"], 1.2);
    EXPECT_EQ((*adjustment)["adjustment"]["channels"]["r"]["in_white"], 255);
    const Json* group = findLayer(root, "00000002");
    ASSERT_TRUE(group);
    EXPECT_EQ((*group)["collapsed"], false);
    EXPECT_EQ(root["outputs"][1]["bit_depth"], 16);
}

TEST_F(TitleArtIOTest, UnknownDataSurvivesEdits) {
    TitleArtParseResult parsed = parseTitleArt(readFixture("unknown-data.titleart"));
    ASSERT_TRUE(parsed.ok);
    // an edit through the field API, then save and read again, twice
    for (int pass = 0; pass < 2; pass++) {
        Json* layer = findLayer(parsed.doc.root, "0000cafe");
        ASSERT_TRUE(layer);
        const FieldSetResult set = setTitleArtField(*layer, "layer", "opacity", 0.5 - pass * 0.1);
        ASSERT_TRUE(set.ok) << set.error;
        normalizeTitleArt(parsed.doc);
        parsed = parseTitleArt(writeTitleArt(parsed.doc));
        ASSERT_TRUE(parsed.ok);
    }
    const Json& root = parsed.doc.root;
    EXPECT_EQ(root["future_member"]["keep"], Json::array({1, 2, 3}));
    EXPECT_EQ(root["animation"]["clips"][0]["name"], "intro") << "a reserved member is kept until its owner registers it";
    const Json* hologram = findLayer(root, "0000beef");
    ASSERT_TRUE(hologram);
    EXPECT_EQ((*hologram)["beams"], 3) << "a layer of unknown type is kept verbatim";
    EXPECT_FALSE(hologram->contains("visible")) << "and gets no members it does not have";
    const Json* text = findLayer(root, "0000cafe");
    ASSERT_TRUE(text);
    EXPECT_EQ((*text)["sparkle"], true);
    EXPECT_EQ((*text)["masks"][0]["scale"], 2);
    EXPECT_EQ((*text)["effects"][0]["params"]["rays"], 9);
    EXPECT_EQ((*text)["transform"]["stretch"], Json::array({1.5, 1}));
    EXPECT_NEAR((*text)["opacity"].get<double>(), 0.4, 1e-9);
    // unknown members follow the known ones
    std::vector<std::string> order;
    for (auto it = text->begin(); it != text->end(); ++it) order.push_back(it.key());
    EXPECT_EQ(order.back(), "sparkle");
    const std::vector<TitleProblem> problems = titleArtDocumentProblems(parsed.doc);
    size_t unknownTypes = 0;
    for (const TitleProblem& p : problems) unknownTypes += p.code == "unknown_type";
    EXPECT_EQ(unknownTypes, 3u) << "the layer, the mask and the effect of unknown types";
}

TEST_F(TitleArtIOTest, LenientRead) {
    const std::string text = R"({
      "format": "jasperengine.titleart", "version": 1, "guid": "0123456789abcdef0123456789abcdef",
      "canvas": {"width": "wide", "height": 9000},
      "layers": [
        {"id": "00000001", "type": "text", "opacity": 2, "align": "middle", "kerning": 1,
         "style": {"rims": [{"id": "00000002", "width": 140}], "face": {"fill": {"color": "#ABC"}}},
         "chars": {"x": {}, "02": {}, "3": {"rotation": "a lot"}},
         "variations": {"wght": 800, "toolong": 1}}
      ],
      "outputs": [{"id": "main", "round_to": 3, "scale": 20}]
    })";
    const TitleArtParseResult parsed = parseTitleArt(text);
    ASSERT_TRUE(parsed.ok) << parsed.error.toString();
    const auto& w = parsed.warnings;
    EXPECT_TRUE(hasWarning(w, "canvas.width", "expected a whole number"));
    EXPECT_TRUE(hasWarning(w, "canvas.height", "9000 clamped to 8192"));
    EXPECT_TRUE(hasWarning(w, "layers[0].opacity", "2 clamped to 1"));
    EXPECT_TRUE(hasWarning(w, "layers[0].align", "expected one of start, center, end"));
    EXPECT_TRUE(hasWarning(w, "layers[0].kerning", "expected true or false"));
    EXPECT_TRUE(hasWarning(w, "layers[0].style.rims[0].width", "140 clamped to 100"));
    EXPECT_TRUE(hasWarning(w, "layers[0].chars.x", "not a valid key"));
    EXPECT_TRUE(hasWarning(w, "layers[0].chars.02", "not a valid key"));
    EXPECT_TRUE(hasWarning(w, "layers[0].chars.3.rotation", "expected a number"));
    EXPECT_TRUE(hasWarning(w, "layers[0].variations.toolong", "not a valid entry"));
    EXPECT_TRUE(hasWarning(w, "outputs[0].round_to", "not one of the allowed values"));
    EXPECT_TRUE(hasWarning(w, "outputs[0].scale", "20 clamped to 8"));
    const Json& layer = parsed.doc.root["layers"][0];
    EXPECT_EQ(parsed.doc.root["canvas"]["width"], 1920);
    EXPECT_EQ(layer["opacity"], 1);
    EXPECT_EQ(layer["align"], "center");
    EXPECT_EQ(layer["style"]["rims"][0]["width"], 100);
    EXPECT_EQ(layer["style"]["face"]["fill"]["color"], "#aabbcc");
    EXPECT_EQ(layer["chars"]["3"]["rotation"], 0);
    EXPECT_EQ(layer["variations"], Json({{"wght", 800}}));
    EXPECT_EQ(parsed.doc.root["outputs"][0]["round_to"], 2) << "the nearest allowed value (2 and 4 are as near: the first wins)";
}

TEST_F(TitleArtIOTest, Failures) {
    TitleArtParseResult notJson = parseTitleArt("{\n  \"format\": \"jasperengine.titleart\",\n  \"version\": 1,,\n}");
    EXPECT_FALSE(notJson.ok);
    EXPECT_EQ(notJson.error.code, "not_json");
    EXPECT_EQ(notJson.error.line, 3) << notJson.error.toString();
    EXPECT_GT(notJson.error.column, 0);

    EXPECT_EQ(parseTitleArt("[1, 2]").error.code, "not_object");
    EXPECT_EQ(parseTitleArt(R"({"format": "jasperengine.controller", "version": 1})").error.code, "bad_format");
    EXPECT_EQ(parseTitleArt(R"({"version": 1})").error.code, "bad_format");
    EXPECT_EQ(parseTitleArt(R"({"format": "jasperengine.titleart", "version": "one"})").error.code, "bad_version");
    const TitleArtParseResult newer = parseTitleArt(R"({"format": "jasperengine.titleart", "version": 99})");
    EXPECT_FALSE(newer.ok);
    EXPECT_EQ(newer.error.code, "version_newer");
    EXPECT_EQ(newer.versionRead, 99);
    EXPECT_NE(newer.error.message.find("version 99"), std::string::npos);
}

TEST_F(TitleArtIOTest, IdsAndNamesAreMadeUnique) {
    const std::string text = R"({
      "format": "jasperengine.titleart", "version": 1, "guid": "not-a-guid",
      "layers": [
        {"id": "00000001", "type": "shape", "style": {"rims": [{"id": "00000001"}, {}]}},
        {"id": "XYZ", "type": "shape", "masks": [{"type": "shape"}]}
      ],
      "exposed": [{"name": "title", "type": "text"}, {"name": "title", "type": "text"}],
      "outputs": [{"id": "main"}, {"id": "main"}]
    })";
    std::vector<TitleWarning> warnings;
    TitleArtParseResult parsed = parseTitleArt(text);
    ASSERT_TRUE(parsed.ok);
    const Json& root = parsed.doc.root;
    EXPECT_TRUE(isTitleArtGuid(root["guid"].get<std::string>()));
    std::set<std::string> ids;
    forEachLayer(root, [&](const Json& layer, const Json*, int) {
        ids.insert(layer["id"].get<std::string>());
        std::vector<std::string> inner;
        collectLayerIds(layer, inner);
        for (const std::string& id : inner) {
            EXPECT_TRUE(isTitleArtId(id)) << id;
            ids.insert(id);
        }
        return true;
    });
    EXPECT_EQ(ids.size(), 5u) << "two layers, two rims and a mask: one id space, no repeats";
    EXPECT_EQ(root["layers"][0]["id"], "00000001") << "the first holder keeps its id";
    EXPECT_EQ(root["exposed"][1]["name"], "title_2");
    EXPECT_EQ(root["outputs"][1]["id"], "main_2");
    EXPECT_TRUE(hasWarning(parsed.warnings, "layers[0].style.rims[0].id", "used twice"));
    EXPECT_TRUE(hasWarning(parsed.warnings, "layers[1].id", "not an id"));
}

TEST_F(TitleArtIOTest, LimitsAreClamped) {
    Json root = Json{{"format", TITLE_ART_FORMAT}, {"version", 1}, {"canvas", {{"width", 8192}, {"height", 8192}}}};
    Json rims = Json::array();
    for (int i = 0; i < 20; i++) rims.push_back(Json::object());
    std::string longText(1100, 'a');
    root["layers"] = Json::array({Json{{"type", "text"}, {"text", longText}, {"style", {{"rims", rims}}}}});
    // groups nested deeper than 16
    Json deepest = Json{{"type", "shape"}};
    for (int i = 0; i < 20; i++) deepest = Json{{"type", "group"}, {"children", Json::array({deepest})}};
    root["layers"].push_back(deepest);
    TitleArtParseResult parsed = parseTitleArtJson(root);
    ASSERT_TRUE(parsed.ok);
    const Json& layers = parsed.doc.root["layers"];
    EXPECT_EQ(layers[0]["style"]["rims"].size(), TITLE_MAX_RIMS);
    EXPECT_EQ(layers[0]["text"].get<std::string>().size(), TITLE_MAX_TEXT_CODEPOINTS);
    const int64_t pixels = parsed.doc.root["canvas"]["width"].get<int64_t>() * parsed.doc.root["canvas"]["height"].get<int64_t>();
    EXPECT_LE(pixels, TITLE_MAX_CANVAS_PIXELS);
    int depth = 0;
    forEachLayer(parsed.doc.root, [&](const Json&, const Json*, int d) {
        depth = std::max(depth, d);
        return true;
    });
    EXPECT_LT(depth, TITLE_MAX_GROUP_DEPTH);
}

TEST_F(TitleArtIOTest, PartKeysAreStoredById) {
    const std::string text = R"({
      "format": "jasperengine.titleart", "version": 1,
      "layers": [{"id": "00000001", "type": "text",
        "style": {"rims": [{"id": "0000000a"}, {"id": "0000000b"}]},
        "part_effects": {"rim#1": [{"id": "00000020", "type": "outer_glow"}], "rim:dead0000": [{"type": "outer_glow"}],
                         "face": [{"id": "00000021", "type": "inner_glow"}], "wing": [{"type": "x"}]}}]
    })";
    const TitleArtParseResult parsed = parseTitleArt(text);
    ASSERT_TRUE(parsed.ok);
    const Json& parts = parsed.doc.root["layers"][0]["part_effects"];
    EXPECT_TRUE(parts.contains("rim:0000000b")) << parts.dump();
    EXPECT_TRUE(parts.contains("face"));
    EXPECT_FALSE(parts.contains("rim#1"));
    EXPECT_FALSE(parts.contains("rim:dead0000"));
    EXPECT_FALSE(parts.contains("wing"));
    EXPECT_TRUE(hasWarning(parsed.warnings, "layers[0].part_effects.rim:dead0000", "does not exist"));
}

TEST_F(TitleArtIOTest, NewDocumentIsCreateTitleArtDefault) {
    const TitleArtDoc doc = makeNewTitleArt();
    EXPECT_EQ(doc.canvasWidth(), 1920);
    EXPECT_EQ(doc.canvasHeight(), 1080);
    ASSERT_EQ(doc.layers().size(), 1u);
    const Json& layer = doc.layers()[0];
    EXPECT_EQ(layer["type"], "text");
    EXPECT_EQ(layer["name"], "Title");
    EXPECT_EQ(layer["text"], "Title");
    EXPECT_TRUE(layer["font"].is_null()) << "the built-in face";
    ASSERT_EQ(doc.root["exposed"].size(), 1u);
    EXPECT_EQ(doc.root["exposed"][0]["name"], "text");
    EXPECT_EQ(doc.root["exposed"][0]["targets"][0]["layer"], layer["id"]);
    EXPECT_EQ(doc.root["exposed"][0]["targets"][0]["field"], "text");
    EXPECT_EQ(doc.root["outputs"][0]["id"], "main");
    EXPECT_TRUE(titleArtDocumentProblems(doc).empty());
    EXPECT_EQ(readTitleArtGuid(writeTitleArt(doc)), doc.guid());
}

TEST_F(TitleArtIOTest, FieldShorthandsExpand) {
    TitleArtDoc doc = parseTitleArt(readFixture("every-layer-type.titleart")).doc;
    const Json* text = findLayer(doc.root, "00000003");
    ASSERT_TRUE(text);
    auto expand = [&](const std::string& path) { return expandFieldShorthand(text, path); };
    EXPECT_EQ(expand("face.fill.color").path, "style.face.fill.color");
    EXPECT_EQ(expand("rim#1.width").path, "style.rims[1].width");
    EXPECT_EQ(expand("rim:00000010.width").path, "style.rims[0].width");
    EXPECT_EQ(expand("fx:drop_shadow.distance").path, "effects[0].params.distance");
    EXPECT_EQ(expand("fx:drop_shadow.enabled").path, "effects[0].enabled") << "the entry's own member";
    EXPECT_EQ(expand("rim#1/fx:outer_glow.size").path, "part_effects.rim:00000011[0].params.size");
    EXPECT_EQ(expand("adj.type").path, "adjustment.type");
    EXPECT_EQ(expand("gen.color").path, "generator.params.color");
    EXPECT_EQ(expand("style.knockout").path, "style.knockout");
    const ExpandedFieldPath noRim = expand("rim#5.width");
    EXPECT_FALSE(noRim.ok);
    EXPECT_NE(std::find(noRim.validHere.begin(), noRim.validHere.end(), "rim#1"), noRim.validHere.end());
#if defined(JASPERENGINE_TITLE_ART_3D) && JASPERENGINE_TITLE_ART_3D
    // S86 registers style_3d: a band that does not exist is a bad path, 3d. expands
    EXPECT_EQ(expand("band#0.depth").code, "bad_path");
    EXPECT_EQ(expand("3d.depth").path, "style_3d.depth");
#else
    EXPECT_EQ(expand("band#0.depth").code, "not_available");
    EXPECT_EQ(expand("3d.extrude").code, "not_available");
#endif
}

TEST_F(TitleArtIOTest, FieldWritesValidateAndClamp) {
    TitleArtDoc doc = makeNewTitleArt();
    Json& layer = doc.layers()[0];
    FieldSetResult width = setTitleArtField(layer, "layer", "size", 99999);
    ASSERT_TRUE(width.ok);
    EXPECT_TRUE(width.clamped);
    EXPECT_EQ(width.to, 8192);
    EXPECT_EQ(width.clampMessage, "99999 clamped to 8192");
    FieldSetResult color = setTitleArtField(layer, "layer", "face.fill.color", Json::array({1, 0, 0}));
    ASSERT_TRUE(color.ok);
    EXPECT_EQ(color.to, "#ff0000");
    EXPECT_EQ(color.longPath, "style.face.fill.color");
    EXPECT_EQ(color.pattern, "style.face.fill.color");
    EXPECT_EQ(color.ownerLabel, "Fill");
    FieldSetResult bad = setTitleArtField(layer, "layer", "opacity", "half");
    EXPECT_FALSE(bad.ok);
    EXPECT_EQ(bad.code, "bad_value");
    EXPECT_EQ(layer["opacity"], 1) << "a refused write leaves the object as it was";
    FieldSetResult typo = setTitleArtField(layer, "layer", "style.face.bevl.size", 4);
    EXPECT_FALSE(typo.ok);
    EXPECT_EQ(typo.code, "bad_path");
    EXPECT_EQ(typo.nearest, "bevel");
    FieldSetResult readOnly = setTitleArtField(layer, "layer", "id", "00000000");
    EXPECT_EQ(readOnly.code, "read_only");
    FieldSetResult reserved = setTitleArtField(layer, "layer", "transform.stretch", Json::array({2, 1}));
    EXPECT_EQ(reserved.code, "not_available");
    EXPECT_NE(reserved.error.find("S85"), std::string::npos);
    FieldSetResult created = setTitleArtField(layer, "layer", "chars.2.rotation", 15);
    ASSERT_TRUE(created.ok) << created.error;
    EXPECT_EQ(layer["chars"]["2"]["rotation"], 15);
    EXPECT_EQ(layer["chars"]["2"]["scale"], 1) << "a new map entry starts at its defaults";
    EXPECT_EQ(created.pattern, "chars.*.rotation");
    FieldSetResult index = setTitleArtField(layer, "layer", "style.rims[0].width", 2);
    EXPECT_EQ(index.code, "out_of_range");
    FieldSetResult doc1 = setTitleArtField(doc.root, "document", "canvas.width", 640);
    EXPECT_TRUE(doc1.ok);
    EXPECT_EQ(doc.canvasWidth(), 640);
}

TEST_F(TitleArtIOTest, OverridesApplyToACopy) {
    const TitleArtDoc doc = makeNewTitleArt();
    const std::string id = doc.layers()[0]["id"].get<std::string>();
    TitleArtDoc copy = doc;
    std::vector<TitleProblem> problems;
    applyFieldOverrides(copy, {{id, "text", "Bonus"}, {id, "face.fill.color", "#00ff00"}, {"", "seed", 9},
                               {id, "nonsense.path", 1}, {"ffffffff", "text", "x"}},
                        &problems);
    EXPECT_EQ(copy.layers()[0]["text"], "Bonus");
    EXPECT_EQ(copy.layers()[0]["style"]["face"]["fill"]["color"], "#00ff00");
    EXPECT_EQ(copy.root["seed"], 9);
    EXPECT_EQ(doc.layers()[0]["text"], "Title") << "the snapshot is untouched";
    ASSERT_EQ(problems.size(), 2u);
    EXPECT_EQ(problems[0].code, "override_skipped");
}

TEST_F(TitleArtIOTest, Budget) {
    // R47: parse + normalize + write of a 256-layer, 1 MB document within 60 ms (Release; a build
    // without optimisation gets ten times that). A wall-clock budget is printed always and enforced
    // with perfSlack() (JASPER_TEST_PERF_SLACK, the benchmark run), never asserted on a shared
    // builder; the round trip itself is checked everywhere. Every member is written, so a styled text
    // layer is about 14 KB: the 256 layers are styled text layers and plain groups, as many text
    // layers as make the document 1 MB.
    auto layerBytes = [](const Json& layer) {
        Json one = Json{{"format", TITLE_ART_FORMAT}, {"version", 1}, {"layers", Json::array({layer})}};
        return writeTitleArtJson(parseTitleArtJson(one).doc.root["layers"][0]).size();
    };
    Json chars = Json::object();
    for (int c = 0; c < 4; c++) chars[std::to_string(c)] = Json{{"rotation", c}};
    const Json textLayer = Json{{"type", "text"}, {"text", "Layer"}, {"chars", chars},
                                {"style", {{"rims", Json::array({Json{{"width", 2}}})}}}};
    const Json groupLayer = Json{{"type", "group"}};
    const size_t textBytes = layerBytes(textLayer), groupBytes = layerBytes(groupLayer);
    const size_t texts = std::min<size_t>(256, (1000000 - 256 * groupBytes) / (textBytes - groupBytes) + 1);
    Json root = Json{{"format", TITLE_ART_FORMAT}, {"version", 1}};
    Json layers = Json::array();
    for (size_t i = 0; i < 256; i++) layers.push_back(i < texts ? textLayer : groupLayer);
    root["layers"] = layers;
    const std::string text = writeTitleArt(parseTitleArtJson(root).doc);
    EXPECT_GE(text.size(), 1000000u) << "the document is at least 1 MB";
    using Clock = std::chrono::steady_clock;
    const auto ms = [](Clock::time_point a, Clock::time_point b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
    const auto rawStart = Clock::now();
    const Json raw = Json::parse(text);
    const double rawMs = ms(rawStart, Clock::now());
    // the best of three runs (the build machines are shared)
    double best = 1e9, parseMs = 0, writeMs = 0;
    for (int run = 0; run < 3; run++) {
        const auto start = Clock::now();
        const TitleArtParseResult parsed = parseTitleArt(text);
        const auto parsedAt = Clock::now();
        const std::string again = writeTitleArt(parsed.doc);
        const auto writtenAt = Clock::now();
        ASSERT_TRUE(parsed.ok);
        EXPECT_EQ(again, text);
        if (ms(start, writtenAt) < best) {
            best = ms(start, writtenAt);
            parseMs = ms(start, parsedAt);
            writeMs = ms(parsedAt, writtenAt);
        }
    }
#ifdef NDEBUG
    const double limit = 60.0;
#else
    const double limit = 600.0;
#endif
    std::printf("[bench] TitleArtIO 1 MB parse + normalize + write: %.3f ms (budget %.3f; %zu bytes: parse + normalize %.3f ms, "
                "write %.3f ms, the JSON parse alone %.3f ms)\n", best, limit, text.size(), parseMs, writeMs, rawMs);
    const double slack = testsupport::perfSlack();
    if (slack > 0.0) {
        EXPECT_LE(best, limit * slack);
    }
}
