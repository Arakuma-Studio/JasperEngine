// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include "titleart/TitleArtHash.h"
#include "titleart/TitleArtIO.h"

using namespace jasperengine;
using namespace jasperengine::titleart;

namespace {

    class TitleArtHashTest : public ::testing::Test {
    protected:
        TitleArtDoc doc;
        std::string id;

        void SetUp() override {
            const std::string text = R"({"format": "jasperengine.titleart", "version": 1, "layers": [
                {"id": "00000001", "type": "text", "text": "確定",
                 "style": {"face": {"bevel": {"enabled": true}}, "rims": [{"id": "00000002", "width": 4}]},
                 "effects": [{"id": "00000003", "type": "drop_shadow"}]},
                {"id": "00000004", "type": "shape", "masks": [{"id": "00000005", "type": "layer", "layer": "00000001"}]}]})";
            const TitleArtParseResult parsed = parseTitleArt(text);
            ASSERT_TRUE(parsed.ok);
            doc = parsed.doc;
            id = "00000001";
        }

        LayerHashes hashesAfter(const std::string& path, const Json& value, const std::string& layer = "00000001") {
            TitleArtDoc copy = doc;
            Json* target = layer.empty() ? &copy.root : findLayer(copy.root, layer);
            const FieldSetResult set = setTitleArtField(*target, layer.empty() ? "document" : "layer", path, value);
            EXPECT_TRUE(set.ok) << path << ": " << set.error;
            return hashTitleArt(copy).layers.at(id);
        }
    };

}

TEST_F(TitleArtHashTest, AColorChangeKeepsMasksAndFields) {
    const LayerHashes before = hashTitleArt(doc).layers.at(id);
    const LayerHashes after = hashesAfter("face.fill.color", "#ff0000");
    EXPECT_EQ(after.geometry, before.geometry);
    EXPECT_EQ(after.mask, before.mask);
    EXPECT_EQ(after.field, before.field);
    EXPECT_NE(after.stack, before.stack);
    EXPECT_NE(after.node, before.node);
}

// the alpha hash (what a stack's alpha depends on): colours shade colour only; opacity, a fill
// kind or a gradient (its stops carry alpha) and the fields change it
TEST_F(TitleArtHashTest, AlphaFollowsAllButColours) {
    const LayerHashes before = hashTitleArt(doc).layers.at(id);
    EXPECT_EQ(hashesAfter("face.fill.color", "#ff0000").alpha, before.alpha);
    EXPECT_EQ(hashesAfter("face.bevel.highlight_color", "#ffe0e0").alpha, before.alpha);
    EXPECT_EQ(hashesAfter("rim#0.material.fill.color", "#00ff00").alpha, before.alpha);
    EXPECT_NE(hashesAfter("face.opacity", 0.5).alpha, before.alpha);
    EXPECT_NE(hashesAfter("face.fill.kind", "linear").alpha, before.alpha);
    EXPECT_NE(hashesAfter("rim#0.width", 6).alpha, before.alpha);
    EXPECT_NE(hashesAfter("transform.rotation", 10).alpha, before.alpha);
    EXPECT_EQ(hashesAfter("transform.position", Json::array({40, -12})).alpha, before.alpha);
}

TEST_F(TitleArtHashTest, APositionOnlyPlacesAgain) {
    const LayerHashes before = hashTitleArt(doc).layers.at(id);
    const LayerHashes after = hashesAfter("transform.position", Json::array({40, -12}));
    EXPECT_EQ(after.stack, before.stack);
    EXPECT_EQ(after.effects, before.effects);
    EXPECT_NE(after.placement, before.placement);
    EXPECT_NE(after.node, before.node);
}

TEST_F(TitleArtHashTest, TheCascadeFollowsTheScope) {
    const LayerHashes before = hashTitleArt(doc).layers.at(id);
    const LayerHashes rim = hashesAfter("rim#0.width", 8);
    EXPECT_EQ(rim.geometry, before.geometry);
    EXPECT_NE(rim.mask, before.mask);
    EXPECT_NE(rim.field, before.field);
    const LayerHashes bevel = hashesAfter("face.bevel.size", 6);
    EXPECT_EQ(bevel.mask, before.mask);
    EXPECT_NE(bevel.field, before.field);
    const LayerHashes text = hashesAfter("text", "当選");
    EXPECT_NE(text.geometry, before.geometry);
    const LayerHashes effect = hashesAfter("effects[0].enabled", false);
    EXPECT_EQ(effect.stack, before.stack);
    EXPECT_NE(effect.effects, before.effects);
    const LayerHashes light = hashesAfter("global_light.angle", 45, "");
    EXPECT_EQ(light.field, before.field);
    EXPECT_NE(light.stack, before.stack) << "the bevel light is fixed on screen: the stack re-shades";
    const LayerHashes rotation = hashesAfter("transform.rotation", 15);
    EXPECT_EQ(rotation.field, before.field);
    EXPECT_NE(rotation.stack, before.stack);
    const LayerHashes name = hashesAfter("name", "Renamed");
    EXPECT_EQ(name.node, before.node) << "names feed no hash";
}

TEST_F(TitleArtHashTest, FreshIdsGiveTheSameHashes) {
    const TitleArtHashes before = hashTitleArt(doc);
    std::string text = writeTitleArt(doc);
    for (const auto& [from, to] : std::vector<std::pair<std::string, std::string>>{
             {"00000001", "a0000001"}, {"00000002", "a0000002"}, {"00000003", "a0000003"}, {"00000004", "a0000004"}, {"00000005", "a0000005"}}) {
        for (size_t at = text.find(from); at != std::string::npos; at = text.find(from)) text.replace(at, from.size(), to);
    }
    const TitleArtParseResult parsed = parseTitleArt(text);
    ASSERT_TRUE(parsed.ok);
    const TitleArtHashes after = hashTitleArt(parsed.doc);
    EXPECT_EQ(after.document, before.document) << "rendering never depends on an id";
    EXPECT_EQ(after.layers.at("a0000001").node, before.layers.at("00000001").node);
    EXPECT_EQ(after.layers.at("a0000004").node, before.layers.at("00000004").node);
}

TEST_F(TitleArtHashTest, ReadersFollowTheirSourcesAndAssets) {
    const TitleArtHashes before = hashTitleArt(doc);
    TitleArtDoc copy = doc;
    setTitleArtField(*findLayer(copy.root, "00000001"), "layer", "face.fill.color", "#00ff00");
    const TitleArtHashes after = hashTitleArt(copy);
    EXPECT_EQ(after.layers.at("00000004").stack, before.layers.at("00000004").stack);
    EXPECT_NE(after.layers.at("00000004").node, before.layers.at("00000004").node) << "a layer mask reads its source";
    EXPECT_NE(after.document, before.document);

    setTitleArtField(*findLayer(copy.root, "00000001"), "layer", "font", Json{{"path", "fonts/a.ttf"}, {"guid", ""}});
    const TitleArtHashes v1 = hashTitleArt(copy, [](const TitleAssetRef&) { return uint64_t(1); });
    const TitleArtHashes v2 = hashTitleArt(copy, [](const TitleAssetRef&) { return uint64_t(2); });
    EXPECT_NE(v1.layers.at(id).geometry, v2.layers.at(id).geometry) << "an edited font reaches every hash that names it";
}

TEST_F(TitleArtHashTest, AGeneratorsParamsChangeItsNodeCacheKey) {
    // the node cache keys by `content`: a new param value must not hit the old pixels
    const TitleArtParseResult parsed = parseTitleArt(R"({"format": "jasperengine.titleart", "version": 1, "layers": [
        {"id": "00000001", "type": "generator", "generator": {"type": "pattern_fill", "params": {"rotation": 10}}}]})");
    ASSERT_TRUE(parsed.ok);
    TitleArtDoc changed = parsed.doc;
    Json* layer = findLayer(changed.root, "00000001");
    ASSERT_TRUE(setTitleArtField(*layer, "layer", "gen.rotation", Json(20)).ok);
    EXPECT_NE(hashTitleArt(changed).layers.at("00000001").content, hashTitleArt(parsed.doc).layers.at("00000001").content);
}

// two generators that differ only in what they generate are different pixels: the node cache keys
// a layer by its content hash (red: a Fire and Speed Lines following one title shared a node entry)
TEST_F(TitleArtHashTest, AGeneratorsParamsAreItsContent) {
    const std::string text = R"({"format": "jasperengine.titleart", "version": 1, "layers": [
        {"id": "00000001", "type": "text", "text": "GOAL"},
        {"id": "00000002", "type": "generator", "follow": "00000001", "generator": {"type": "speed_lines", "params": {"count": 170}}},
        {"id": "00000003", "type": "generator", "follow": "00000001", "generator": {"type": "fire", "params": {}}}]})";
    const TitleArtParseResult parsed = parseTitleArt(text);
    ASSERT_TRUE(parsed.ok);
    const TitleArtHashes hashes = hashTitleArt(parsed.doc);
    EXPECT_NE(hashes.layers.at("00000002").content, hashes.layers.at("00000003").content);
    EXPECT_NE(hashes.layers.at("00000002").stack, hashes.layers.at("00000003").stack);

    TitleArtDoc copy = parsed.doc;
    const FieldSetResult set = setTitleArtField(*findLayer(copy.root, "00000002"), "layer", "generator.params.count", 40);
    ASSERT_TRUE(set.ok) << set.error;
    const LayerHashes before = hashes.layers.at("00000002"), after = hashTitleArt(copy).layers.at("00000002");
    EXPECT_NE(after.content, before.content) << "a param edit is new pixels, not a cache hit";
    EXPECT_EQ(after.placement, before.placement);
}
