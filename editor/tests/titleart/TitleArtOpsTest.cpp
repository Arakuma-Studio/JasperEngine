// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include <algorithm>

#include <filesystem>
#include <fstream>

#include "titleart/TitleArtOps.h"
#include "titleart/TitleArtIO.h"
#include "titleart/TitleArtSchema.h"
#include "titleart/TitleEffectRegistry.h"

using namespace jasperengine;
using namespace jasperengine::editor;
using titleart::Json;
using titleart::TitleArtDoc;

namespace {

    titleart::EffectDef testShadow() {
        titleart::EffectDef def;
        def.type = "test_shadow";
        def.label = "Test Shadow";
        titleart::ParamDef distance;
        distance.key = "distance";
        distance.label = "Distance";
        distance.kind = titleart::ParamKind::Number;
        distance.unit = titleart::ParamUnit::Px;
        distance.min = 0;
        distance.max = 500;
        distance.defaultValue = 10;
        titleart::ParamDef color;
        color.key = "color";
        color.label = "Color";
        color.kind = titleart::ParamKind::Color;
        color.defaultValue = "#000000";
        def.params = {distance, color};
        return def;
    }

    titleart::EffectDef layerOnly() {
        titleart::EffectDef def;
        def.type = "test_layer_only";
        def.label = "Test Layer Only";
        def.onParts = false;
        return def;
    }

    // Title (text, two rims, an effect), a group "Logo" holding "Spark" twice and a shape
    TitleArtDoc sample() {
        const std::string text = R"({"format": "jasperengine.titleart", "version": 1,
          "material_links": {},
          "layers": [
            {"id": "00000001", "type": "text", "name": "Title", "size": 200,
             "style": {"rims": [{"id": "00000011", "width": 3}, {"id": "00000012", "width": 5}]},
             "effects": [{"id": "00000021", "type": "test_shadow"}]},
            {"id": "00000002", "type": "group", "name": "Logo", "transform": {"position": [100, 0], "rotation": 90, "scale": 2},
             "children": [
               {"id": "00000003", "type": "shape", "name": "Spark", "transform": {"position": [10, 0]}},
               {"id": "00000004", "type": "shape", "name": "Spark"},
               {"id": "00000005", "type": "shape", "name": "Badge", "masks": [{"id": "00000031", "type": "layer", "layer": "00000001"}]}]}
          ],
          "exposed": [{"name": "title", "type": "text", "targets": [{"layer": "00000001", "field": "text"}]}]})";
        titleart::TitleArtParseResult parsed = titleart::parseTitleArt(text);
        EXPECT_TRUE(parsed.ok) << parsed.error.toString();
        return parsed.doc;
    }

    const TitleOpProblem* failure(const TitleOpsResult& r) {
        for (const TitleOpProblem& p : r.problems) {
            if (p.severity == "error") return &p;
        }
        return nullptr;
    }

    class TitleArtOpsTest : public ::testing::Test {
    protected:
        void SetUp() override {
            ASSERT_TRUE(titleart::TitleEffectRegistry::get().add(testShadow()));
            ASSERT_TRUE(titleart::TitleEffectRegistry::get().add(layerOnly()));
            doc = sample();
        }
        void TearDown() override {
            titleart::TitleEffectRegistry::get().removeEffect("test_shadow");
            titleart::TitleEffectRegistry::get().removeEffect("test_layer_only");
        }
        TitleOpsResult apply(const Json& ops) { return TitleArtOps::apply(doc, ops); }
        TitleArtDoc doc;
    };

}

TEST_F(TitleArtOpsTest, SetClampsAndEchoesPixels) {
    const TitleOpsResult r = apply(Json{{"op", "set"}, {"layer", "Title"}, {"values", {{"rim#1.width", 140}}}});
    ASSERT_TRUE(r.ok) << r.error();
    ASSERT_EQ(r.changes.size(), 1u);
    const TitleOpChange& c = r.changes[0];
    EXPECT_EQ(c.path, "style.rims[1].width");
    EXPECT_EQ(c.from, 5);
    EXPECT_EQ(c.to, 100);
    EXPECT_TRUE(c.clamped);
    EXPECT_EQ(c.clampMessage, "140 clamped to 100");
    ASSERT_TRUE(c.px);
    EXPECT_DOUBLE_EQ(*c.px, 200.0) << "100 % of a 200 px size";
    EXPECT_EQ(r.undoName, "Set Rim Width");
    EXPECT_EQ(r.doc.layers()[0]["style"]["rims"][1]["width"], 100);
    EXPECT_EQ(doc.layers()[0]["style"]["rims"][1]["width"], 5) << "apply never changes its input";
    EXPECT_EQ(apply(Json{{"op", "set"}, {"layer", "Title"}, {"values", {{"fx:test_shadow.distance", 20}}}}).undoName,
              "Set Test Shadow Distance");
    EXPECT_EQ(apply(Json{{"op", "set"}, {"values", {{"canvas.width", 640}}}}).undoName, "Set Canvas Width");
    const TitleOpsResult two = apply(Json{{"op", "set"}, {"layer", "Title"}, {"values", {{"opacity", 0.5}, {"text", "Bonus"}}}});
    EXPECT_EQ(two.undoName, "Edit Title Art (2 changes)");
}

TEST_F(TitleArtOpsTest, BadPathsSuggestTheNearestName) {
    const TitleOpsResult r = apply(Json{{"op", "set"}, {"layer", "Title"}, {"values", {{"face.bevl.size", 4}}}});
    EXPECT_FALSE(r.ok);
    const TitleOpProblem* p = failure(r);
    ASSERT_TRUE(p);
    EXPECT_EQ(p->code, "bad_path");
    EXPECT_EQ(p->nearest, "bevel");
    EXPECT_FALSE(p->validHere.empty());
    EXPECT_NE(r.error().find("did you mean 'bevel'"), std::string::npos) << r.error();
    EXPECT_NE(r.error().find("Nothing was applied."), std::string::npos);
    const TitleOpsResult layer = apply(Json{{"op", "set"}, {"layer", "Titel"}, {"values", {{"opacity", 1}}}});
    ASSERT_TRUE(failure(layer));
    EXPECT_EQ(failure(layer)->code, "bad_layer");
    EXPECT_EQ(failure(layer)->nearest, "Title");
    const TitleOpsResult op = apply(Json{{"op", "sett"}});
    ASSERT_TRUE(failure(op));
    EXPECT_EQ(failure(op)->nearest, "set");
}

TEST_F(TitleArtOpsTest, BatchesAreAllOrNothing) {
    const TitleOpsResult r = apply(Json::array({Json{{"op", "set"}, {"layer", "Title"}, {"values", {{"opacity", 0.25}}}},
                                                Json{{"op", "remove_layer"}, {"layer", "Logo/Spark#1"}},
                                                Json{{"op", "set"}, {"layer", "Title"}, {"values", {{"opacity", "half"}}}}}));
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.failedOp, 2);
    EXPECT_EQ(failure(r)->op, 2);
    EXPECT_EQ(failure(r)->code, "bad_value");
    EXPECT_TRUE(r.changes.empty());
    EXPECT_EQ(titleart::writeTitleArt(r.doc), titleart::writeTitleArt(doc)) << "nothing applied";
}

TEST_F(TitleArtOpsTest, LayersAreAddressedByPathDuplicatesAndIds) {
    auto opacity = [&](const std::string& ref) {
        const TitleOpsResult r = apply(Json{{"op", "set"}, {"layer", ref}, {"values", {{"opacity", 0.5}}}});
        return r.ok ? r.changes[0].layer : (failure(r) ? failure(r)->code : std::string("?"));
    };
    EXPECT_EQ(opacity("Logo/Spark"), "00000003");
    EXPECT_EQ(opacity("Logo/Spark#1"), "00000004") << "#N: the Nth same-named sibling after the first";
    EXPECT_EQ(opacity("id:00000004"), "00000004");
    EXPECT_EQ(opacity("Badge"), "00000005") << "a bare name found anywhere";
    EXPECT_EQ(opacity("Spark"), "ambiguous") << "a bare name of several layers is refused";
    const TitleOpsResult r = apply(Json{{"op", "set"}, {"layer", "Spark"}, {"values", {{"opacity", 0.5}}}});
    ASSERT_TRUE(failure(r));
    EXPECT_EQ(failure(r)->validHere, (std::vector<std::string>{"Logo/Spark", "Logo/Spark#1"}));
    EXPECT_EQ(opacity("Logo/Nope"), "bad_layer");
}

TEST_F(TitleArtOpsTest, AddLayerFrom) {
    // a layer from another document: a group whose child masks itself with its sibling
    const Json from = Json{{"id", "00000002"}, {"type", "group"}, {"name", "Pasted"},
                           {"children", Json::array({Json{{"id", "0000000a"}, {"type", "text"}, {"name", "A"}},
                                                     Json{{"id", "0000000b"}, {"type", "shape"}, {"name", "B"},
                                                          {"masks", Json::array({Json{{"id", "0000000c"}, {"type", "layer"}, {"layer", "0000000a"}},
                                                                                 Json{{"id", "0000000d"}, {"type", "layer"}, {"layer", "99999999"}}})}}})}};
    const TitleOpsResult r = apply(Json{{"op", "add_layer"}, {"from", from}});
    ASSERT_TRUE(r.ok) << r.error();
    const std::string id = r.newIds["0"]["layer"].get<std::string>();
    EXPECT_NE(id, "00000002") << "fresh ids: the source's would collide with Logo";
    const Json* pasted = titleart::findLayer(r.doc.root, id);
    ASSERT_TRUE(pasted);
    const Json& a = (*pasted)["children"][0];
    const Json& b = (*pasted)["children"][1];
    EXPECT_NE(a["id"], "0000000a");
    EXPECT_NE(b["masks"][0]["id"], "0000000c");
    EXPECT_EQ(b["masks"][0]["layer"], a["id"]) << "references inside the copy are remapped";
    EXPECT_EQ(b["masks"][1]["layer"], "") << "references outside it are dropped";
    bool noted = false;
    for (const TitleOpProblem& p : r.problems) noted |= p.code == "reference_dropped";
    EXPECT_TRUE(noted);
    EXPECT_EQ(r.doc.layers().back()["id"], id) << "the top of the document by default";
    std::set<std::string> ids;
    titleart::forEachLayer(r.doc.root, [&](const Json& l, const Json*, int) {
        EXPECT_TRUE(ids.insert(l["id"].get<std::string>()).second) << "no duplicate id";
        return true;
    });
}

TEST_F(TitleArtOpsTest, AddLayerPlacesItAndAppliesValues) {
    const TitleOpsResult r = apply(Json{{"op", "add_layer"}, {"type", "text"}, {"parent", "Logo"}, {"before", "Logo/Badge"},
                                        {"name", "Caption"}, {"values", {{"text", "Win"}, {"face.fill.color", "#ff0000"}}}});
    ASSERT_TRUE(r.ok) << r.error();
    const Json& children = r.doc.layers()[1]["children"];
    ASSERT_EQ(children.size(), 4u);
    EXPECT_EQ(children[2]["name"], "Caption") << "below Badge in draw order";
    EXPECT_EQ(children[2]["text"], "Win");
    EXPECT_EQ(children[2]["style"]["face"]["fill"]["color"], "#ff0000");
    EXPECT_EQ(r.undoName, "Add Text Layer");
    EXPECT_EQ(failure(apply(Json{{"op", "add_layer"}, {"type", "txt"}}))->nearest, "text");
    EXPECT_EQ(failure(apply(Json{{"op", "add_layer"}, {"type", "text"}, {"parent", "Title"}}))->code, "bad_layer");
    EXPECT_EQ(failure(apply(Json{{"op", "add_layer"}, {"type", "text"}, {"look", "gold"}}))->code, "not_available");
    const TitleOpsResult adjustment = apply(Json{{"op", "add_layer"}, {"type", "adjustment"}, {"adjustment", "levels"}});
    ASSERT_TRUE(adjustment.ok) << adjustment.error();
    EXPECT_EQ(adjustment.doc.layers().back()["adjustment"]["type"], "levels");
    EXPECT_EQ(failure(apply(Json{{"op", "add_layer"}, {"type", "generator"}, {"generator", "nope"}}))->code, "bad_value");
}

TEST_F(TitleArtOpsTest, RemoveLayerClearsWhatNamesIt) {
    const TitleOpsResult r = apply(Json{{"op", "remove_layer"}, {"layer", "Title"}});
    ASSERT_TRUE(r.ok) << r.error();
    EXPECT_EQ(titleart::findLayer(r.doc.root, "00000001"), nullptr);
    EXPECT_TRUE(titleart::findLayer(r.doc.root, "00000005")->value("masks", Json::array()).empty()) << "the layer mask that named it";
    EXPECT_TRUE(r.doc.root["exposed"][0]["targets"].empty()) << "the exposed target that named it";
    EXPECT_EQ(r.undoName, "Remove Layer Title");
    size_t notes = 0;
    for (const TitleOpChange& c : r.changes) notes += !c.note.empty();
    EXPECT_GE(notes, 3u) << "each listed in changes";
}

TEST_F(TitleArtOpsTest, DuplicateMoveGroupUngroup) {
    const TitleOpsResult dup = apply(Json{{"op", "duplicate_layer"}, {"layer", "Logo/Badge"}});
    ASSERT_TRUE(dup.ok) << dup.error();
    const Json& children = dup.doc.layers()[1]["children"];
    ASSERT_EQ(children.size(), 4u);
    EXPECT_NE(children[3]["id"], children[2]["id"]);
    EXPECT_NE(children[3]["masks"][0]["id"], children[2]["masks"][0]["id"]);
    EXPECT_EQ(children[3]["masks"][0]["layer"], "00000001") << "a duplicate keeps references outside it";

    const TitleOpsResult move = apply(Json{{"op", "move_layer"}, {"layer", "Title"}, {"parent", "Logo"}, {"after", "Logo/Spark"}});
    ASSERT_TRUE(move.ok) << move.error();
    EXPECT_EQ(move.doc.layers().size(), 1u);
    EXPECT_EQ(move.doc.layers()[0]["children"][1]["id"], "00000001");
    EXPECT_EQ(failure(apply(Json{{"op", "move_layer"}, {"layer", "Logo"}, {"parent", "Logo"}}))->code, "bad_layer")
        << "a layer cannot move into itself";

    const TitleOpsResult grouped = apply(Json{{"op", "group"}, {"layers", Json::array({"Logo/Spark", "Logo/Badge"})}, {"name", "Pair"}});
    ASSERT_TRUE(grouped.ok) << grouped.error();
    const Json& logo = grouped.doc.layers()[1]["children"];
    ASSERT_EQ(logo.size(), 2u);
    EXPECT_EQ(logo[0]["name"], "Spark");
    EXPECT_EQ(logo[1]["name"], "Pair") << "the group takes the topmost member's place";
    EXPECT_EQ(logo[1]["children"].size(), 2u);
    EXPECT_EQ(failure(apply(Json{{"op", "group"}, {"layers", Json::array({"Title", "Logo/Badge"})}}))->code, "bad_layer")
        << "siblings only";

    const TitleOpsResult ungrouped = apply(Json{{"op", "ungroup"}, {"layer", "Logo"}});
    ASSERT_TRUE(ungrouped.ok) << ungrouped.error();
    ASSERT_EQ(ungrouped.doc.layers().size(), 4u);
    // the group's transform (at 100, 0, turned 90 clockwise, scale 2) folded into Spark at (10, 0)
    const Json& spark = ungrouped.doc.layers()[1]["transform"];
    EXPECT_NEAR(spark["position"][0].get<double>(), 100.0, 1e-9);
    EXPECT_NEAR(spark["position"][1].get<double>(), -20.0, 1e-9) << "+x turned 90 clockwise on screen points down (y up)";
    EXPECT_NEAR(spark["rotation"].get<double>(), 90.0, 1e-9);
    EXPECT_NEAR(spark["scale"].get<double>(), 2.0, 1e-9);
    EXPECT_EQ(ungrouped.undoName, "Ungroup Logo");
}

TEST_F(TitleArtOpsTest, EffectOps) {
    const TitleOpsResult add = apply(Json{{"op", "add_effect"}, {"layer", "Title"}, {"part", "rim#1"}, {"type", "test_shadow"},
                                          {"params", {{"distance", 900}}}});
    ASSERT_TRUE(add.ok) << add.error();
    const Json& stack = add.doc.layers()[0]["part_effects"]["rim:00000012"];
    ASSERT_EQ(stack.size(), 1u) << "part aliases are stored by id";
    EXPECT_EQ(stack[0]["params"]["distance"], 500);
    EXPECT_EQ(stack[0]["params"]["color"], "#000000");
    EXPECT_EQ(add.undoName, "Add Effect Test Shadow");
    EXPECT_TRUE(add.newIds["0"].contains("effect"));
    EXPECT_EQ(failure(apply(Json{{"op", "add_effect"}, {"layer", "Title"}, {"type", "test_shadw"}}))->nearest, "test_shadow");
    EXPECT_EQ(failure(apply(Json{{"op", "add_effect"}, {"layer", "Title"}, {"part", "rim#1"}, {"type", "test_layer_only"}}))->code, "bad_value");
    EXPECT_EQ(failure(apply(Json{{"op", "add_effect"}, {"layer", "Title"}, {"part", "rim#9"}, {"type", "test_shadow"}}))->code, "bad_value");
    EXPECT_EQ(failure(apply(Json{{"op", "add_effect"}, {"layer", "Title"}, {"part", "bevel"}, {"type", "test_shadow"}}))->code, "not_available");

    const TitleOpsResult two = apply(Json::array({Json{{"op", "add_effect"}, {"layer", "Title"}, {"type", "test_shadow"}, {"index", 0}},
                                                  Json{{"op", "move_effect"}, {"layer", "Title"}, {"effect", "fx:test_shadow#1"}, {"index", 0}},
                                                  Json{{"op", "remove_effect"}, {"layer", "Title"}, {"effect", "id:00000021"}}}));
    ASSERT_TRUE(two.ok) << two.error();
    EXPECT_EQ(two.doc.layers()[0]["effects"].size(), 1u);
    EXPECT_NE(two.doc.layers()[0]["effects"][0]["id"], "00000021");
    const TitleOpsResult missing = apply(Json{{"op", "remove_effect"}, {"layer", "Title"}, {"effect", "fx:outer_glow"}});
    ASSERT_TRUE(failure(missing));
    EXPECT_EQ(failure(missing)->validHere, (std::vector<std::string>{"fx:test_shadow"}));
}

TEST_F(TitleArtOpsTest, RimAndMaskOps) {
    const TitleOpsResult rim = apply(Json{{"op", "add_rim"}, {"layer", "Title"}, {"index", 0}, {"values", {{"width", 7}, {"join", "round"}}}});
    ASSERT_TRUE(rim.ok) << rim.error();
    const Json& rims = rim.doc.layers()[0]["style"]["rims"];
    ASSERT_EQ(rims.size(), 3u);
    EXPECT_EQ(rims[0]["width"], 7);
    EXPECT_EQ(rims[0]["join"], "round");
    EXPECT_TRUE(titleart::isTitleArtId(rims[0]["id"].get<std::string>()));
    // removing a rim drops its part effects
    const TitleOpsResult parts = apply(Json::array({Json{{"op", "add_effect"}, {"layer", "Title"}, {"part", "rim:00000011"}, {"type", "test_shadow"}},
                                                    Json{{"op", "remove_rim"}, {"layer", "Title"}, {"rim", "rim#0"}}}));
    ASSERT_TRUE(parts.ok) << parts.error();
    EXPECT_FALSE(parts.doc.layers()[0]["part_effects"].contains("rim:00000011"));
    const TitleOpsResult moved = apply(Json{{"op", "move_rim"}, {"layer", "Title"}, {"rim", "rim:00000012"}, {"index", 0}});
    ASSERT_TRUE(moved.ok) << moved.error();
    EXPECT_EQ(moved.doc.layers()[0]["style"]["rims"][0]["id"], "00000012");
    EXPECT_EQ(failure(apply(Json{{"op", "add_rim"}, {"layer", "Logo"}}))->code, "bad_layer") << "groups have no rims";

    const TitleOpsResult mask = apply(Json{{"op", "add_mask"}, {"layer", "Title"}, {"type", "layer"}, {"values", {{"layer", "Logo/Badge"}, {"feather", 4}}}});
    ASSERT_TRUE(mask.ok) << mask.error();
    EXPECT_EQ(mask.doc.layers()[0]["masks"][0]["layer"], "00000005") << "a layer mask names its layer by id";
    EXPECT_EQ(mask.doc.layers()[0]["masks"][0]["feather"], 4);
    EXPECT_EQ(mask.undoName, "Add Layer Mask");
    const TitleOpsResult removed = apply(Json{{"op", "remove_mask"}, {"layer", "Logo/Badge"}, {"mask", "mask#0"}});
    ASSERT_TRUE(removed.ok) << removed.error();
    EXPECT_TRUE(titleart::findLayer(removed.doc.root, "00000005")->value("masks", Json::array()).empty());
    EXPECT_EQ(failure(apply(Json{{"op", "add_mask"}, {"layer", "Title"}, {"type", "noise"}}))->code, "bad_value");
}

TEST_F(TitleArtOpsTest, ExposeChecksItsTargets) {
    const TitleOpsResult r = apply(Json{{"op", "expose"}, {"name", "glow_color"}, {"type", "color"},
                                        {"targets", Json::array({Json{{"layer", "Title"}, {"field", "face.fill.color"}}})}});
    ASSERT_TRUE(r.ok) << r.error();
    const Json& exposed = r.doc.root["exposed"].back();
    EXPECT_EQ(exposed["targets"][0]["layer"], "00000001");
    EXPECT_EQ(exposed["targets"][0]["field"], "style.face.fill.color") << "stored in its long form";
    EXPECT_EQ(failure(apply(Json{{"op", "expose"}, {"name", "size"}, {"type", "color"},
                                 {"targets", Json::array({Json{{"layer", "Title"}, {"field", "size"}}})}}))->code, "bad_value")
        << "a number field is not a colour";
    EXPECT_EQ(failure(apply(Json{{"op", "expose"}, {"name", "Bad Name"}, {"type", "text"},
                                 {"targets", Json::array({Json{{"layer", "Title"}, {"field", "text"}}})}}))->code, "bad_value");
    EXPECT_EQ(failure(apply(Json{{"op", "expose"}, {"name", "title"}, {"type", "text"},
                                 {"targets", Json::array({Json{{"layer", "Title"}, {"field", "text"}}})}}))->code, "bad_value")
        << "exposed already";
    const TitleOpsResult scheme = apply(Json{{"op", "expose"}, {"name", "colors"}, {"type", "color_scheme"}});
    EXPECT_TRUE(scheme.ok) << scheme.error();
    const TitleOpsResult un = apply(Json{{"op", "unexpose"}, {"name", "title"}});
    ASSERT_TRUE(un.ok);
    EXPECT_TRUE(un.doc.root["exposed"].empty());
    EXPECT_EQ(failure(apply(Json{{"op", "unexpose"}, {"name", "titel"}}))->nearest, "title");
}

TEST_F(TitleArtOpsTest, LinkedMaterialsStayEqual) {
    const TitleOpsResult linked = apply(Json::array({
        Json{{"op", "link_material"}, {"layer", "Title"}, {"material", "face"}, {"name", "Gold"}},
        Json{{"op", "set"}, {"layer", "Title"}, {"values", {{"face.fill.color", "#ffd060"}}}}}));
    ASSERT_TRUE(linked.ok) << linked.error();
    const std::string key = linked.newIds["0"]["link"].get<std::string>();
    EXPECT_EQ(linked.doc.root["material_links"][key]["name"], "Gold");
    // a second material joins the link: it takes the group's material
    const TitleOpsResult joined = TitleArtOps::apply(linked.doc, Json{{"op", "link_material"}, {"layer", "Title"},
                                                                       {"material", "rim#0.material"}, {"link", key}});
    ASSERT_TRUE(joined.ok) << joined.error();
    EXPECT_EQ(joined.doc.layers()[0]["style"]["rims"][0]["material"]["fill"]["color"], "#ffd060");
    // an edit of either is copied to the other in the same step
    const TitleOpsResult edited = TitleArtOps::apply(joined.doc, Json{{"op", "set"}, {"layer", "Title"},
                                                                       {"values", {{"rim#0.material.bevel.enabled", true}}}});
    ASSERT_TRUE(edited.ok) << edited.error();
    EXPECT_EQ(edited.doc.layers()[0]["style"]["face"]["bevel"]["enabled"], true);
    EXPECT_EQ(edited.doc.layers()[0]["style"]["face"], edited.doc.layers()[0]["style"]["rims"][0]["material"]);
    // unlinking both removes the unused link
    const TitleOpsResult unlinked = TitleArtOps::apply(edited.doc, Json::array({
        Json{{"op", "unlink_material"}, {"layer", "Title"}, {"material", "face"}},
        Json{{"op", "unlink_material"}, {"layer", "Title"}, {"material", "rim#0.material"}}}));
    ASSERT_TRUE(unlinked.ok) << unlinked.error();
    EXPECT_FALSE(unlinked.doc.root["material_links"].contains(key));
    EXPECT_EQ(failure(apply(Json{{"op", "link_material"}, {"layer", "Title"}, {"material", "opacity"}}))->code, "bad_path");
}

TEST_F(TitleArtOpsTest, ReservedOpsNameTheirOwner) {
    for (const ReservedTitleOp& reserved : TitleArtOps::reservedOps()) {
        // an op its owner registered in this build (S86's, once registerTitle3DEditor ran) is no longer pending
        if (TitleArtOps::pendingOwner(reserved.name).empty()) continue;
        const TitleOpsResult r = apply(Json{{"op", reserved.name}});
        ASSERT_TRUE(failure(r)) << reserved.name;
        EXPECT_EQ(failure(r)->code, "not_available") << reserved.name;
        EXPECT_NE(failure(r)->message.find(reserved.owner), std::string::npos) << reserved.name;
    }
    // the table's owner, whether or not a test before this one ran S84's registration (the
    // order of the suites differs between platforms)
    const std::vector<ReservedTitleOp>& reserved = TitleArtOps::reservedOps();
    const auto look = std::find_if(reserved.begin(), reserved.end(),
                                   [](const ReservedTitleOp& op) { return std::string(op.name) == "apply_look"; });
    ASSERT_NE(look, reserved.end());
    EXPECT_STREQ(look->owner, "S84");
}

// S87's create.from importers register by kind and are chosen by the file's content
TEST_F(TitleArtOpsTest, CreateFromPicksTheImporterByContent) {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "je_title_create_from";
    std::filesystem::create_directories(dir);
    const std::filesystem::path file = dir / "logo.bin";
    {
        std::ofstream out(file, std::ios::binary);
        out << "TEST-FORMAT body";
    }
    TitleCreateRequest request;
    request.sourcePath = file.string();
    request.documentPath = (dir / "logo.titleart").string();
    std::string seen;
    ASSERT_TRUE(TitleArtOps::registerCreateImporter(
        "test_kind", [](const std::string& head, const std::string&) { return head.rfind("TEST-FORMAT", 0) == 0; },
        [&](const TitleCreateRequest& r, TitleCreateResult& out, std::string&, std::string&) {
            seen = r.documentPath;
            out.doc = titleart::makeNewTitleArt();
            out.files.push_back({(dir / "logo_assets" / "a.png").string(), "png"});
            return true;
        }));
    EXPECT_FALSE(TitleArtOps::registerCreateImporter("test_kind", [](const std::string&, const std::string&) { return true; },
                                                     [](const TitleCreateRequest&, TitleCreateResult&, std::string&, std::string&) { return true; }))
        << "one importer per kind";
    TitleCreateResult result;
    std::string code, error;
    ASSERT_TRUE(TitleArtOps::createFrom(request, result, code, error)) << error;
    EXPECT_EQ(result.kind, "test_kind");
    EXPECT_EQ(seen, request.documentPath);
    EXPECT_EQ(result.files.size(), 1u);
    {
        std::ofstream out(file, std::ios::binary);
        out << "something else";
    }
    EXPECT_FALSE(TitleArtOps::createFrom(request, result, code, error));
    EXPECT_EQ(code, "import_unknown_kind");
    request.sourcePath = (dir / "missing.psd").string();
    EXPECT_FALSE(TitleArtOps::createFrom(request, result, code, error));
    EXPECT_EQ(code, "file_not_found");
    EXPECT_TRUE(TitleArtOps::unregisterCreateImporter("test_kind"));
    std::filesystem::remove_all(dir);
}

// (an op of its own: a reserved one's owner may have registered it in an earlier test)
TEST_F(TitleArtOpsTest, ARegisteredOpRunsInTheBatch) {
    const std::string name = "test_fill_color";
    const TitleOpsResult before = apply(Json{{"op", name}, {"layer", "Title"}});
    ASSERT_TRUE(failure(before));
    EXPECT_EQ(failure(before)->code, "bad_op");
    ASSERT_TRUE(TitleArtOps::registerOp(name, [](TitleOpContext& ctx, const Json& op) {
        Json* layer = ctx.layer(op);
        if (!layer) return false;
        return ctx.set(layer, "face.fill.color", op.value("color", std::string("#c0a040")));
    }));
    EXPECT_FALSE(TitleArtOps::registerOp("set", [](TitleOpContext&, const Json&) { return true; })) << "S80's ops are S80's";
    EXPECT_FALSE(TitleArtOps::registerOp(name, [](TitleOpContext&, const Json&) { return true; })) << "one handler";
    const TitleOpsResult r = apply(Json::array({Json{{"op", name}, {"layer", "Title"}, {"color", "#123456"}},
                                                Json{{"op", name}, {"layer", "Nope"}}}));
    EXPECT_FALSE(r.ok) << "all or nothing for registered ops too";
    EXPECT_EQ(failure(r)->code, "bad_layer");
    const TitleOpsResult ok = apply(Json{{"op", name}, {"layer", "Title"}, {"color", "#123456"}});
    ASSERT_TRUE(ok.ok) << ok.error();
    EXPECT_EQ(ok.doc.layers()[0]["style"]["face"]["fill"]["color"], "#123456");
    EXPECT_TRUE(TitleArtOps::unregisterOp(name));
    const TitleOpsResult after = apply(Json{{"op", name}, {"layer", "Title"}});
    ASSERT_TRUE(failure(after));
    EXPECT_EQ(failure(after)->code, "bad_op");
}

TEST_F(TitleArtOpsTest, CommitInvariantsClearAClipWithoutABase) {
    const TitleOpsResult r = apply(Json{{"op", "set"}, {"layer", "Title"}, {"values", {{"clip", true}}}});
    ASSERT_TRUE(r.ok) << r.error();
    EXPECT_EQ(r.doc.layers()[0]["clip"], false) << "the bottom layer has no base to clip to";
    const TitleOpsResult based = apply(Json{{"op", "set"}, {"layer", "Logo/Spark#1"}, {"values", {{"clip", true}}}});
    ASSERT_TRUE(based.ok);
    EXPECT_EQ(based.doc.layers()[1]["children"][1]["clip"], true);
}

TEST_F(TitleArtOpsTest, SetTextDropsCharAdjustmentsPastTheEnd) {
    const TitleOpsResult adjusted = apply(Json{{"op", "set"}, {"layer", "Title"}, {"values", {{"chars.4.rotation", 10}, {"chars.1.rotation", 5}}}});
    ASSERT_TRUE(adjusted.ok) << adjusted.error();
    const TitleOpsResult r = TitleArtOps::apply(adjusted.doc, Json{{"op", "set_text"}, {"layer", "Title"}, {"text", "Win"}});
    ASSERT_TRUE(r.ok) << r.error();
    EXPECT_TRUE(r.doc.layers()[0]["chars"].contains("1"));
    EXPECT_FALSE(r.doc.layers()[0]["chars"].contains("4"));
    const TitleOpsResult kept = TitleArtOps::apply(adjusted.doc, Json{{"op", "set_text"}, {"layer", "Title"}, {"text", "Win"}, {"keep_chars", true}});
    EXPECT_TRUE(kept.doc.layers()[0]["chars"].contains("4"));
    EXPECT_EQ(failure(apply(Json{{"op", "set_text"}, {"layer", "Logo/Badge"}, {"text", "x"}}))->code, "bad_layer");
}

TEST_F(TitleArtOpsTest, ARegisteredMemberIsSuggested) {
    titleart::SchemaMember member;
    member.key = "glint_rate";
    member.type = titleart::ValueType::Number;
    member.defaultValue = 1.0;
    member.min = 0;
    member.max = 10;
    ASSERT_TRUE(titleart::TitleArtSchema::get().addMember("layer.text", member));
    const TitleOpsResult r = apply(Json{{"op", "set"}, {"layer", "Title"}, {"values", {{"glint_rat", 2}}}});
    ASSERT_TRUE(failure(r));
    EXPECT_EQ(failure(r)->nearest, "glint_rate");
    const TitleOpsResult ok = apply(Json{{"op", "set"}, {"layer", "Title"}, {"values", {{"glint_rate", 20}}}});
    ASSERT_TRUE(ok.ok) << ok.error();
    EXPECT_EQ(ok.doc.layers()[0]["glint_rate"], 10);
    EXPECT_TRUE(titleart::TitleArtSchema::get().removeMember("layer.text", "glint_rate"));
}
