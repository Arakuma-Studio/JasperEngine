// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S47 AC7 (R24, R25): layers, frames (tags shifting as Aseprite does), links, the palette and the
// canvas; a plain PNG refuses layers and frames.

#include "SpriteDrawTestSupport.h"

#include "miniz.h"

using namespace jasperengine::editor::pixelart;
using namespace jasperengine::editor::pixelart::draw;
using jasperengine::editor::pixelart::s47test::json;

namespace ase = jasperengine::editor::pixelart::aseprite;

TEST(StructureOps, AddMoveRemoveLayersKeepCels) {
    draw::SpriteDoc sprite = s47test::makeSprite(4, 1, ase::ColorMode::Indexed, 1, {"bottom", "top"});
    S47_APPLY(sprite, json::array({{{"op", "pixel"}, {"points", {{0, 0}}}, {"color", 1}, {"layer", "top"}}}));
    S47_APPLY(sprite, json::array({{{"op", "layer"}, {"action", "add"}, {"name", "mid"}, {"at", 1}}}));
    ASSERT_EQ(sprite.doc.layers.size(), 3u);
    EXPECT_EQ(sprite.doc.layers[1].name, "mid");
    EXPECT_EQ(s47test::canvasRows(sprite, 2)[0], "1...") << "top moved to index 2 with its cel";
    S47_APPLY(sprite, json::array({{{"op", "layer"}, {"action", "move"}, {"name", "top"}, {"at", 0}}}));
    EXPECT_EQ(sprite.doc.layers[0].name, "top");
    EXPECT_EQ(s47test::canvasRows(sprite, 0)[0], "1...");
    S47_APPLY(sprite, json::array({{{"op", "layer"}, {"action", "remove"}, {"name", "top"}}}));
    EXPECT_TRUE(sprite.doc.cels.empty());
    EXPECT_EQ(sprite.doc.layers[0].name, "bottom");
}

TEST(StructureOps, LayerSetAndRename) {
    draw::SpriteDoc sprite = s47test::makeSprite(2, 1);
    S47_APPLY(sprite, json::array({{{"op", "layer"}, {"action", "set"}, {"name", "A"}, {"opacity", 10}, {"visible", false}, {"blend_mode", "multiply"}},
                                   {{"op", "layer"}, {"action", "rename"}, {"name", "A"}, {"new_name", "B"}}}));
    EXPECT_EQ(sprite.doc.layers[0].name, "B");
    EXPECT_EQ(sprite.doc.layers[0].opacity, 10);
    EXPECT_FALSE(sprite.doc.layers[0].visible());
    EXPECT_EQ(sprite.doc.layers[0].blend, ase::BlendMode::Multiply);
}

TEST(StructureOps, LastLayerCannotBeRemoved) {
    draw::SpriteDoc sprite = s47test::makeSprite(2, 1);
    s47test::Run run = s47test::apply(sprite, json::array({{{"op", "layer"}, {"action", "remove"}, {"name", "A"}}}));
    ASSERT_FALSE(run.ok);
    EXPECT_EQ(run.problem.kind, "last_layer");
}

TEST(StructureOps, GroupedLayerStaysInItsGroup) {
    draw::SpriteDoc sprite = s47test::makeSprite(2, 1);
    sprite.doc.layers.clear();
    ase::Layer group;
    group.name = "Body";
    group.type = ase::LayerType::Group;
    ase::Layer child;
    child.name = "Arm";
    child.childLevel = 1;
    child.parent = 0;
    sprite.doc.layers = {group, child};
    S47_APPLY(sprite, json::array({{{"op", "layer"}, {"action", "add"}, {"name", "Leg"}, {"group", "Body"}}}));
    EXPECT_EQ(sprite.doc.layerPath(2), "Body/Leg");
    S47_APPLY(sprite, json::array({{{"op", "pixel"}, {"points", {{0, 0}}}, {"color", 1}, {"layer", "Body/Leg"}}}));
    EXPECT_EQ(s47test::canvasRows(sprite, 2)[0], "1.");
}

TEST(StructureOps, FrameAddCopiesAndLinks) {
    draw::SpriteDoc sprite = s47test::makeSprite(2, 1);
    S47_APPLY(sprite, json::array({{{"op", "pixel"}, {"points", {{0, 0}}}, {"color", 1}}}));
    S47_APPLY(sprite, json::array({{{"op", "frame"}, {"action", "add"}}}));
    EXPECT_EQ(sprite.doc.frameCount(), 2);
    EXPECT_EQ(s47test::canvasRows(sprite, 0, 1)[0], "1.");
    EXPECT_EQ(sprite.doc.celRecord(0, 1)->linkedFrame, -1);
    S47_APPLY(sprite, json::array({{{"op", "frame"}, {"action", "add"}, {"link", true}}}));
    EXPECT_EQ(sprite.doc.celRecord(0, 2)->linkedFrame, 1);
    S47_APPLY(sprite, json::array({{{"op", "frame"}, {"action", "add"}, {"copy", false}}}));
    EXPECT_EQ(sprite.doc.celRecord(0, 3), nullptr);
}

TEST(StructureOps, EditingALinkedCelEditsEveryFrameShowingIt) {
    draw::SpriteDoc sprite = s47test::makeSprite(2, 1);
    S47_APPLY(sprite, json::array({{{"op", "pixel"}, {"points", {{0, 0}}}, {"color", 1}}, {{"op", "frame"}, {"action", "add"}, {"link", true}}}));
    S47_APPLY(sprite, json::array({{{"op", "pixel"}, {"points", {{1, 0}}}, {"color", 2}, {"frame", 2}}}));
    EXPECT_EQ(s47test::canvasRows(sprite, 0, 0)[0], "12");
    EXPECT_EQ(s47test::canvasRows(sprite, 0, 1)[0], "12");
}

TEST(StructureOps, RemovingFramesShiftsTagsAsAseprite) {
    draw::SpriteDoc sprite = s47test::makeSprite(2, 1, ase::ColorMode::Indexed, 6);
    S47_APPLY(sprite, json::array({{{"op", "tag"}, {"action", "set"}, {"name", "walk"}, {"from", 2}, {"to", 4}},
                                   {{"op", "tag"}, {"action", "set"}, {"name", "end"}, {"from", 6}, {"to", 6}},
                                   {{"op", "frame"}, {"action", "remove"}, {"from", 3}}}));
    ASSERT_EQ(sprite.doc.tags.size(), 2u);
    EXPECT_EQ(sprite.doc.tags[0].from, 1);
    EXPECT_EQ(sprite.doc.tags[0].to, 2) << "walk shrinks";
    EXPECT_EQ(sprite.doc.tags[1].from, 4) << "end shifts";
    s47test::Run run = s47test::apply(sprite, json::array({{{"op", "frame"}, {"action", "remove"}, {"from", 5}}}));
    ASSERT_TRUE(run.ok) << run.problem.message;
    EXPECT_EQ(sprite.doc.tags.size(), 1u) << "a tag left empty goes";
    bool warned = false;
    for (const draw::EditWarning& w : run.result.warnings) warned = warned || w.kind == "tag_removed";
    EXPECT_TRUE(warned);
}

TEST(StructureOps, RemovingAHolderFrameKeepsTheLinkedFrames) {
    draw::SpriteDoc sprite = s47test::makeSprite(2, 1);
    S47_APPLY(sprite, json::array({{{"op", "pixel"}, {"points", {{0, 0}}}, {"color", 1}}, {{"op", "frame"}, {"action", "add"}, {"link", true}, {"count", 2}}}));
    S47_APPLY(sprite, json::array({{{"op", "frame"}, {"action", "remove"}, {"from", 1}}}));
    EXPECT_EQ(s47test::canvasRows(sprite, 0, 0)[0], "1.");
    EXPECT_EQ(s47test::canvasRows(sprite, 0, 1)[0], "1.");
}

TEST(StructureOps, LastFrameCannotBeRemoved) {
    draw::SpriteDoc sprite = s47test::makeSprite(2, 1);
    s47test::Run run = s47test::apply(sprite, json::array({{{"op", "frame"}, {"action", "remove"}, {"from", 1}}}));
    ASSERT_FALSE(run.ok);
    EXPECT_EQ(run.problem.kind, "last_frame");
}

TEST(StructureOps, FrameMoveAndDuration) {
    draw::SpriteDoc sprite = s47test::makeSprite(2, 1, ase::ColorMode::Indexed, 3);
    S47_APPLY(sprite, json::array({{{"op", "pixel"}, {"points", {{0, 0}}}, {"color", 1}, {"frame", 1}},
                                   {{"op", "frame"}, {"action", "set"}, {"from", 1}, {"duration_ms", 70}},
                                   {{"op", "frame"}, {"action", "move"}, {"from", 1}, {"at", 4}}}));
    EXPECT_EQ(s47test::canvasRows(sprite, 0, 2)[0], "1.");
    EXPECT_EQ(sprite.doc.frameDurations[2], 70);
    EXPECT_EQ(sprite.doc.frameDurations[0], 100);
}

TEST(StructureOps, SliceAndCelOps) {
    draw::SpriteDoc sprite = s47test::makeSprite(8, 8, ase::ColorMode::Indexed, 2);
    S47_APPLY(sprite, json::array({{{"op", "slice"}, {"action", "set"}, {"name", "hit"}, {"rect", {1, 1, 4, 4}}, {"pivot", {2, 2}}, {"center", {1, 1, 2, 2}}},
                                   {{"op", "pixel"}, {"points", {{0, 0}}}, {"color", 1}},
                                   {{"op", "cel"}, {"action", "link"}, {"frame", 2}, {"from", 1}},
                                   {{"op", "cel"}, {"action", "set"}, {"frame", 1}, {"opacity", 200}, {"z_index", 2}}}));
    ASSERT_EQ(sprite.doc.slices.size(), 1u);
    EXPECT_EQ(sprite.doc.slices[0].keys[0].bounds, (IntRect{1, 1, 4, 4}));
    EXPECT_EQ(sprite.doc.celRecord(0, 1)->linkedFrame, 0);
    EXPECT_EQ(sprite.doc.celRecord(0, 0)->opacity, 200);
    S47_APPLY(sprite, json::array({{{"op", "cel"}, {"action", "unlink"}, {"frame", 2}}}));
    EXPECT_EQ(sprite.doc.celRecord(0, 1)->linkedFrame, -1);
    S47_APPLY(sprite, json::array({{{"op", "cel"}, {"action", "clear"}, {"frame", 2}}}));
    EXPECT_EQ(sprite.doc.celRecord(0, 1), nullptr);
}

TEST(StructureOps, PaletteAddReplaceAndIndex0) {
    draw::SpriteDoc sprite = s47test::makeSprite(1, 1);
    S47_APPLY(sprite, json::array({{{"op", "palette"}, {"action", "add"}, {"colors", {"#102030"}}}, {{"op", "pixel"}, {"points", {{0, 0}}}, {"color", "#102030"}}}));
    EXPECT_EQ(draw::celCanvas(sprite.doc, 0, 0).indexAt(0, 0), 6);
    S47_APPLY(sprite, json::array({{{"op", "palette"}, {"action", "replace"}, {"index", 6}, {"color", "#ff00ff"}}}));
    EXPECT_EQ(draw::celCanvas(sprite.doc, 0, 0).at(0, 0), (PixelRgba{255, 0, 255, 255})) << "the pixels show the new colour";
    s47test::Run refused = s47test::apply(sprite, json::array({{{"op", "palette"}, {"action", "replace"}, {"index", 0}, {"color", "#ff00ff"}}}));
    ASSERT_FALSE(refused.ok);
    EXPECT_EQ(refused.problem.kind, "index_0");
}

TEST(StructureOps, CanvasResizeAnchorsAndCrop) {
    draw::SpriteDoc sprite = s47test::makeSprite(4, 4);
    S47_APPLY(sprite, json::array({{{"op", "pixel"}, {"points", {{1, 1}}}, {"color", 1}}}));
    S47_APPLY(sprite, json::array({{{"op", "canvas"}, {"action", "resize"}, {"size", {8, 8}}, {"anchor", "center"}}}));
    EXPECT_EQ(sprite.doc.width, 8);
    EXPECT_EQ(s47test::canvasRows(sprite)[3][3], '1');
    S47_APPLY(sprite, json::array({{{"op", "canvas"}, {"action", "crop"}, {"rect", {3, 3, 2, 2}}}}));
    EXPECT_EQ(sprite.doc.width, 2);
    EXPECT_EQ(s47test::canvasRows(sprite)[0][0], '1');
}

TEST(StructureOps, ConvertToIndexedNeedsMatchingColorsOrSnap) {
    draw::SpriteDoc sprite = s47test::makeSprite(2, 1, ase::ColorMode::Rgba);
    S47_APPLY(sprite, json::array({{{"op", "pixel"}, {"points", {{0, 0}}}, {"color", "#fe0101"}}}));
    s47test::Run refused = s47test::apply(sprite, json::array({{{"op", "canvas"}, {"action", "convert"}, {"mode", "indexed"}}}));
    ASSERT_FALSE(refused.ok);
    EXPECT_EQ(refused.problem.kind, "too_many_colors");
    s47test::Run snapped = s47test::apply(sprite, json::array({{{"op", "canvas"}, {"action", "convert"}, {"mode", "indexed"}, {"snap", true}}}));
    ASSERT_TRUE(snapped.ok) << snapped.problem.message;
    ASSERT_EQ(snapped.result.warnings.size(), 1u);
    EXPECT_NE(snapped.result.warnings[0].message.find("1 pixels"), std::string::npos) << snapped.result.warnings[0].message;
    EXPECT_EQ(sprite.doc.mode, ase::ColorMode::Indexed);
    EXPECT_EQ(draw::celCanvas(sprite.doc, 0, 0).indexAt(0, 0), 1);
    // back again is exact
    S47_APPLY(sprite, json::array({{{"op", "canvas"}, {"action", "convert"}, {"mode", "rgba"}}}));
    EXPECT_EQ(draw::celCanvas(sprite.doc, 0, 0).at(0, 0), (PixelRgba{255, 0, 0, 255}));
}

TEST(StructureOps, PngRefusesLayerFrameTagSliceAndCelOps) {
    draw::SpriteDoc sprite = s47test::makeSprite(1, 1);
    sprite.kind = draw::SpriteKind::Png;
    s47test::Run run = s47test::apply(sprite, json::array({{{"op", "frame"}, {"action", "add"}}}));
    ASSERT_FALSE(run.ok);
    EXPECT_EQ(run.problem.kind, "png_structure");
    EXPECT_NE(run.problem.fix.find("create_sprite"), std::string::npos);
}

TEST(StructureOps, PngRoundTripKeepsAncillaryChunks) {
    // a 2x1 RGBA PNG with a tEXt chunk: an edit rewrites the pixels and keeps the chunk
    PixelImage image(2, 1);
    image.set(0, 0, {255, 0, 0, 255});
    const std::string encoded = image.encodePng();
    std::vector<uint8_t> bytes(encoded.begin(), encoded.end());
    std::vector<uint8_t> chunk;
    const std::string body("jasper.recipe\0payload", 21);
    const uint32_t size = static_cast<uint32_t>(body.size());
    for (int s = 24; s >= 0; s -= 8) chunk.push_back(static_cast<uint8_t>(size >> s));
    const std::string typed = "tEXt" + body;
    chunk.insert(chunk.end(), typed.begin(), typed.end());
    const uint32_t crc = static_cast<uint32_t>(mz_crc32(MZ_CRC32_INIT, reinterpret_cast<const uint8_t*>(typed.data()), typed.size()));
    for (int s = 24; s >= 0; s -= 8) chunk.push_back(static_cast<uint8_t>(crc >> s));
    // after IHDR
    bytes.insert(bytes.begin() + 33, chunk.begin(), chunk.end());
    draw::SpriteDoc sprite;
    PixelProblem problem;
    ASSERT_TRUE(draw::decodeSprite("hero.png", bytes, sprite, problem)) << problem.message;
    EXPECT_TRUE(sprite.isPng());
    S47_APPLY(sprite, json::array({{{"op", "pixel"}, {"points", {{1, 0}}}, {"color", "#00ff00"}}}));
    std::vector<uint8_t> written;
    ASSERT_TRUE(draw::encodeSprite(sprite, written, problem)) << problem.message;
    const std::string text(written.begin(), written.end());
    EXPECT_NE(text.find(body), std::string::npos) << "the tEXt chunk stays byte for byte";
    std::optional<PixelImage> back = PixelImage::decodePng(written, nullptr);
    ASSERT_TRUE(back.has_value());
    EXPECT_EQ(back->at(1, 0), (PixelRgba{0, 255, 0, 255}));
}
