// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include "titleart/TitleArtIO.h"
#include "titleart/render/DrawTree.h"

using namespace jasperengine;
using namespace jasperengine::titleart;

namespace {

    TitleArtDoc docOf(const Json& layers) {
        Json root = Json{{"format", TITLE_ART_FORMAT}, {"version", 1}, {"layers", layers}};
        TitleArtParseResult parsed = parseTitleArtJson(root);
        EXPECT_TRUE(parsed.ok);
        return parsed.doc;
    }

    Json layer(const char* id, const char* type, Json extra = Json::object()) {
        Json l = Json{{"id", id}, {"type", type}, {"name", id}};
        for (auto it = extra.begin(); it != extra.end(); ++it) l[it.key()] = it.value();
        return l;
    }

    std::string ids(const std::vector<DrawNode>& nodes) {
        std::string out;
        for (const DrawNode& n : nodes) {
            if (!out.empty()) out += " ";
            out += n.layerId.substr(6);
            if (!n.children.empty()) out += "(" + ids(n.children) + ")";
            if (!n.clipped.empty()) out += "[" + ids(n.clipped) + "]";
        }
        return out;
    }

    class DrawTreeTest : public ::testing::Test {};

}

TEST_F(DrawTreeTest, TheVisibilityRule) {
    const TitleArtDoc doc = docOf(Json::array({
        layer("00000001", "text"),
        layer("00000002", "text", {{"visible", false}}),
        layer("00000003", "text", {{"opacity", 0}}),
        layer("00000004", "text", {{"guide", true}}),
        layer("00000005", "group", {{"visible", false}, {"children", Json::array({layer("00000006", "shape")})}}),
        layer("00000007", "group", {{"children", Json::array({layer("00000008", "shape", {{"visible", false}})})}}),
        layer("00000009", "adjustment"),
        layer("0000000a", "hologram"),
    }));
    EXPECT_EQ(ids(buildDrawTree(doc, {})), "01 09") << "hidden, transparent, guide, hidden group, empty group and unknown type drop";
    DrawTreeOptions guides;
    guides.includeGuides = true;
    EXPECT_EQ(ids(buildDrawTree(doc, guides)), "01 04 09");
    const std::vector<DrawNode> nodes = buildDrawTree(doc, {});
    EXPECT_EQ(nodes[1].kind, DrawNodeKind::Adjust);
    EXPECT_EQ(nodes[0].kind, DrawNodeKind::Leaf);
}

TEST_F(DrawTreeTest, ClipGroups) {
    const TitleArtDoc doc = docOf(Json::array({
        layer("00000001", "text", {{"clip", true}}),                    // no base: draws unclipped
        layer("00000002", "shape"),                                      // a base
        layer("00000003", "image", {{"clip", true}}),
        layer("00000004", "text", {{"clip", true}}),
        layer("00000005", "text", {{"visible", false}}),                // a hidden base
        layer("00000006", "text", {{"clip", true}}),                    // hides with it
        layer("00000007", "adjustment"),                                 // an adjustment is no base
        layer("00000008", "text", {{"clip", true}}),
        layer("00000009", "group", {{"children", Json::array({layer("0000000a", "shape")})}}),
        layer("0000000b", "shape", {{"clip", true}}),
    }));
    EXPECT_EQ(ids(buildDrawTree(doc, {})), "01 02[03 04] 07 08 09(0a)[0b]");
}

TEST_F(DrawTreeTest, IsolationKeepsThePathToTheLayer) {
    const TitleArtDoc doc = docOf(Json::array({
        layer("00000001", "text"),
        layer("00000002", "group", {{"visible", false}, {"children", Json::array({
            layer("00000003", "shape"), layer("00000004", "text", {{"visible", false}}),
            layer("00000005", "group", {{"children", Json::array({layer("00000006", "shape"), layer("00000007", "shape")})}})})}}),
    }));
    DrawTreeOptions alone;
    alone.isolateLayer = "00000004";
    EXPECT_EQ(ids(buildDrawTree(doc, alone)), "02(04)") << "a hidden layer and its hidden group draw when isolated";
    alone.isolateLayer = "00000005";
    EXPECT_EQ(ids(buildDrawTree(doc, alone)), "02(05(06 07))");
    DrawTreeOptions listed;
    listed.content = OutputContent::Layers;
    listed.contentLayers = {"00000001", "00000006"};
    EXPECT_EQ(ids(buildDrawTree(doc, listed)), "01") << "Layers content keeps the visibility rule: the group is hidden";
}

TEST_F(DrawTreeTest, NodesKnowTheirDepth) {
    const TitleArtDoc doc = docOf(Json::array({layer("00000001", "group", {{"children", Json::array({
        layer("00000002", "group", {{"children", Json::array({layer("00000003", "text")})}})})}})}));
    const std::vector<DrawNode> nodes = buildDrawTree(doc, {});
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].children[0].children[0].depth, 2);
    EXPECT_EQ(nodes[0].children[0].children[0].type, LayerType::Text);
    EXPECT_EQ((*nodes[0].children[0].children[0].layer)["text"], "Title");
}
