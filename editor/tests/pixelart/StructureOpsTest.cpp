// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S47 AC7 (R24, R25): layers, frames (tags shifting as Aseprite does), links, the palette and the
// canvas; a plain PNG refuses layers and frames.

#include "SpriteDrawTestSupport.h"


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

TEST(StructureOps, MovingOrRemovingAGroupCarriesItsLayers) {
    draw::SpriteDoc sprite = s47test::makeSprite(2, 1);
    sprite.doc.layers.clear();
    ase::Layer group;
    group.name = "Body";
    group.type = ase::LayerType::Group;
    ase::Layer a, b, c;
    a.name = "Arm";
    a.childLevel = 1;
    a.parent = 0;
    b.name = "Leg";
    b.childLevel = 1;
    b.parent = 0;
    c.name = "Hat";
    sprite.doc.layers = {group, a, b, c};
    S47_APPLY(sprite, json::array({{{"op", "pixel"}, {"points", {{1, 0}}}, {"color", 1}, {"layer", "Hat"}},
                                   {{"op", "layer"}, {"action", "move"}, {"name", "Body/Leg"}, {"group", ""}, {"at", 0}}}));
    ASSERT_EQ(sprite.doc.layers.size(), 4u);
    EXPECT_EQ(sprite.doc.layerPath(0), "Leg") << "moved out of the group, to the bottom";
    S47_APPLY(sprite, json::array({{{"op", "layer"}, {"action", "remove"}, {"name", "Body"}}}));
    ASSERT_EQ(sprite.doc.layers.size(), 2u) << "the group and its Arm go";
    EXPECT_EQ(s47test::canvasRows(sprite, 1)[0], ".1") << "Hat kept its cel";
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
    // a fake CRC: only this chunk's bytes matter here, and nothing checks an ancillary chunk's
    chunk.insert(chunk.end(), {0, 0, 0, 0});
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

TEST(StructureOps, MovingAHolderPastItsLinkKeepsTheFileWritable) {
    draw::SpriteDoc sprite = s47test::makeSprite(2, 1, ase::ColorMode::Indexed, 1);
    S47_APPLY(sprite, json::array({{{"op", "pixel"}, {"points", {{0, 0}}}, {"color", 1}}, {{"op", "frame"}, {"action", "add"}, {"link", true}}}));
    S47_APPLY(sprite, json::array({{{"op", "frame"}, {"action", "move"}, {"from", 1}, {"at", 3}}}));
    std::vector<uint8_t> bytes;
    PixelProblem problem;
    ASSERT_TRUE(draw::encodeSprite(sprite, bytes, problem)) << problem.kind << ": " << problem.message;
    EXPECT_EQ(s47test::canvasRows(sprite, 0, 0)[0], "1.");
    EXPECT_EQ(s47test::canvasRows(sprite, 0, 1)[0], "1.");
    EXPECT_EQ(sprite.doc.celRecord(0, 0)->linkedFrame, -1) << "the earliest frame holds the image";
}

TEST(StructureOps, LinkingToALaterFrameKeepsTheFileWritable) {
    draw::SpriteDoc sprite = s47test::makeSprite(2, 1, ase::ColorMode::Indexed, 3);
    S47_APPLY(sprite, json::array({{{"op", "pixel"}, {"points", {{1, 0}}}, {"color", 2}, {"frame", 3}}, {{"op", "cel"}, {"action", "link"}, {"frame", 1}, {"from", 3}}}));
    std::vector<uint8_t> bytes;
    PixelProblem problem;
    ASSERT_TRUE(draw::encodeSprite(sprite, bytes, problem)) << problem.kind << ": " << problem.message;
    EXPECT_EQ(s47test::canvasRows(sprite, 0, 0)[0], ".2");
    EXPECT_EQ(s47test::canvasRows(sprite, 0, 2)[0], ".2");
}

TEST(StructureOps, TagsFollowAFrameMoveAsAseprite) {
    draw::SpriteDoc sprite = s47test::makeSprite(2, 1, ase::ColorMode::Indexed, 4);
    S47_APPLY(sprite, json::array({{{"op", "tag"}, {"action", "set"}, {"name", "t"}, {"from", 3}, {"to", 4}}}));
    S47_APPLY(sprite, json::array({{{"op", "frame"}, {"action", "move"}, {"from", 1}, {"at", 5}}}));
    ASSERT_EQ(sprite.doc.tags.size(), 1u);
    EXPECT_EQ(sprite.doc.tags[0].from, 1);
    EXPECT_EQ(sprite.doc.tags[0].to, 3) << "the moved frame joins the tag it lands at the end of (DocApi::adjustTags)";
}

TEST(StructureOps, GroupingIntoAGroupAboveABackgroundRespectsTheOrder) {
    draw::SpriteDoc sprite = s47test::makeSprite(1, 1, ase::ColorMode::Indexed, 1, {"bg", "G"});
    sprite.doc.layers[0].flags |= ase::kLayerBackground;
    sprite.doc.layers[1].type = ase::LayerType::Group;
    const s47test::Run run = s47test::apply(sprite, json::array({{{"op", "layer"}, {"action", "set"}, {"name", "bg"}, {"group", "G"}}}));
    ASSERT_FALSE(run.ok);
    EXPECT_NE(run.problem.message.find("background"), std::string::npos) << run.problem.message;
}

TEST(StructureOps, ConvertingGrayscaleToRgbaWritesRgbaPixels) {
    draw::SpriteDoc gray = s47test::makeSprite(2, 1, ase::ColorMode::Grayscale);
    S47_APPLY(gray, json::array({{{"op", "pixel"}, {"points", {{0, 0}}}, {"color", "#808080"}}}));
    std::vector<uint8_t> bytes;
    PixelProblem problem;
    ASSERT_TRUE(draw::encodeSprite(gray, bytes, problem)) << problem.message;
    // read back so the document carries provenance, as a real session does
    draw::SpriteDoc session;
    ASSERT_TRUE(draw::decodeSprite("g.aseprite", bytes, session, problem)) << problem.message;
    S47_APPLY(session, json::array({{{"op", "canvas"}, {"action", "convert"}, {"mode", "rgba"}}}));
    std::vector<uint8_t> converted;
    ASSERT_TRUE(draw::encodeSprite(session, converted, problem)) << problem.message;
    draw::SpriteDoc reread;
    ASSERT_TRUE(draw::decodeSprite("g.aseprite", converted, reread, problem)) << problem.kind << ": " << problem.message;
    EXPECT_EQ(reread.doc.mode, ase::ColorMode::Rgba);
    EXPECT_EQ(draw::celCanvas(reread.doc, 0, 0).at(0, 0), (PixelRgba{128, 128, 128, 255}));
    EXPECT_EQ(draw::celCanvas(reread.doc, 0, 0).at(1, 0)[3], 0);
}

TEST(StructureOps, GrowingAnIndexedBackgroundKeepsItsIndices) {
    draw::SpriteDoc sprite = s47test::makeSprite(2, 2);
    sprite.doc.layers[0].flags |= ase::kLayerBackground;
    S47_APPLY(sprite, json::array({{{"op", "rect"}, {"rect", {0, 0, 2, 2}}, {"color", 1}, {"fill", true}}}));
    S47_APPLY(sprite, json::array({{{"op", "canvas"}, {"action", "resize"}, {"size", {4, 2}}, {"anchor", "left"}}}));
    const PixelImage canvas = draw::celCanvas(sprite.doc, 0, 0);
    ASSERT_TRUE(canvas.hasIndices());
    EXPECT_EQ(canvas.indexAt(0, 0), 1);
    EXPECT_EQ(canvas.indexAt(3, 0), 0) << "the new area is the background's index";
    std::vector<uint8_t> bytes;
    PixelProblem problem;
    EXPECT_TRUE(draw::encodeSprite(sprite, bytes, problem)) << problem.message;
}

TEST(StructureOps, TilesetsRefuseThePaletteOpsThatWouldStrandThem) {
    draw::SpriteDoc sprite = s47test::makeSprite(2, 1);
    sprite.doc.tilesets.push_back(ase::Tileset{});
    s47test::Run run = s47test::apply(sprite, json::array({{{"op", "palette"}, {"action", "set"}, {"colors", {"#000000", "#ff0000"}}}}));
    ASSERT_FALSE(run.ok);
    EXPECT_EQ(run.problem.kind, "unsupported_feature");
    draw::SpriteDoc rgba = s47test::makeSprite(2, 1, ase::ColorMode::Rgba);
    rgba.doc.tilesets.push_back(ase::Tileset{});
    run = s47test::apply(rgba, json::array({{{"op", "canvas"}, {"action", "convert"}, {"mode", "indexed"}}}));
    ASSERT_FALSE(run.ok);
    EXPECT_EQ(run.problem.kind, "unsupported_feature");
}

TEST(StructureOps, RemovingAFrameOfALinkedGroupRehomes) {
    draw::SpriteDoc sprite = s47test::makeSprite(2, 1);
    S47_APPLY(sprite, json::array({{{"op", "pixel"}, {"points", {{0, 0}}}, {"color", 1}},
                                   {{"op", "frame"}, {"action", "add"}, {"link", true}, {"count", 2}},
                                   {{"op", "frame"}, {"action", "remove"}, {"from", 2}}}));
    std::vector<uint8_t> bytes;
    PixelProblem problem;
    ASSERT_TRUE(draw::encodeSprite(sprite, bytes, problem)) << problem.message;
    EXPECT_EQ(s47test::canvasRows(sprite, 0, 1)[0], "1.");
}

TEST(StructureOps, RgbPngKeepsItsColourTypeAndRefusesAlpha) {
    std::vector<uint8_t> file = s47test::pngSignature();
    s47test::appendChunk(file, "IHDR", s47test::ihdrBytes(2, 1, 2));
    s47test::appendChunk(file, "IDAT", s47test::storedZlib(s47test::filterNone(6, 1, {255, 0, 0, 0, 0, 255})));
    s47test::appendChunk(file, "IEND", {});
    draw::SpriteDoc sprite;
    PixelProblem problem;
    ASSERT_TRUE(draw::decodeSprite("rgb.png", file, sprite, problem)) << problem.message;
    S47_APPLY(sprite, json::array({{{"op", "pixel"}, {"points", {{1, 0}}}, {"color", "#00ff00"}}}));
    std::vector<uint8_t> written;
    ASSERT_TRUE(draw::encodeSprite(sprite, written, problem)) << problem.message;
    ASSERT_GT(written.size(), 26u);
    EXPECT_EQ(written[25], 2) << "IHDR's colour type stays RGB";
    S47_APPLY(sprite, json::array({{{"op", "pixel"}, {"points", {{0, 0}}}, {"color", "transparent"}}}));
    written.clear();
    ASSERT_FALSE(draw::encodeSprite(sprite, written, problem));
    EXPECT_EQ(problem.kind, "unsupported_feature");
}

TEST(StructureOps, GrayscalePngIsRefused) {
    std::vector<uint8_t> file = s47test::pngSignature();
    s47test::appendChunk(file, "IHDR", s47test::ihdrBytes(1, 1, 0));
    s47test::appendChunk(file, "IDAT", s47test::storedZlib(s47test::filterNone(1, 1, {128})));
    s47test::appendChunk(file, "IEND", {});
    draw::SpriteDoc sprite;
    PixelProblem problem;
    ASSERT_TRUE(draw::decodeSprite("gray.png", file, sprite, problem)) << problem.message;
    const s47test::Run run = s47test::apply(sprite, json::array({{{"op", "pixel"}, {"points", {{0, 0}}}, {"color", "#808080"}}}));
    ASSERT_FALSE(run.ok);
    EXPECT_EQ(run.problem.kind, "unsupported_feature");
}

TEST(StructureOps, AnimatedPngIsRefused) {
    std::vector<uint8_t> file = s47test::palettePng(1, 1, {1}, {{0, 0, 0}, {255, 0, 0}}, {}, "");
    // an APNG marker where a kept chunk goes, before IDAT
    std::vector<uint8_t> actl(8, 0);
    actl[3] = 1;
    std::vector<uint8_t> chunk;
    s47test::appendChunk(chunk, "acTL", actl);
    file.insert(file.begin() + 33, chunk.begin(), chunk.end());
    draw::SpriteDoc sprite;
    PixelProblem problem;
    ASSERT_TRUE(draw::decodeSprite("a.png", file, sprite, problem)) << problem.message;
    const s47test::Run run = s47test::apply(sprite, json::array({{{"op", "pixel"}, {"points", {{0, 0}}}, {"color", 1}}}));
    ASSERT_FALSE(run.ok);
    EXPECT_EQ(run.problem.kind, "unsupported_feature");
}

TEST(StructureOps, PalettePngRoundTripKeepsChunksInOrder) {
    // IHDR, PLTE, tRNS, tEXt, IDAT, IEND: only IHDR, PLTE, tRNS and IDAT change; the tEXt stays between them
    const std::vector<uint8_t> file = s47test::palettePng(2, 1, {1, 2}, {{0, 0, 0}, {255, 0, 0}, {0, 255, 0}}, {0, 255}, std::string("keep\0me", 7));
    draw::SpriteDoc sprite;
    PixelProblem problem;
    ASSERT_TRUE(draw::decodeSprite("p.png", file, sprite, problem)) << problem.message;
    ASSERT_TRUE(sprite.indexed());
    S47_APPLY(sprite, json::array({{{"op", "pixel"}, {"points", {{0, 0}}}, {"color", 2}}}));
    std::vector<uint8_t> written;
    ASSERT_TRUE(draw::encodeSprite(sprite, written, problem)) << problem.message;
    std::vector<std::string> types;
    for (size_t pos = 8; pos + 8 <= written.size();) {
        const uint32_t length = (uint32_t(written[pos]) << 24) | (uint32_t(written[pos + 1]) << 16) | (uint32_t(written[pos + 2]) << 8) | written[pos + 3];
        types.emplace_back(reinterpret_cast<const char*>(&written[pos + 4]), 4);
        pos += 12 + length;
    }
    EXPECT_EQ(types, (std::vector<std::string>{"IHDR", "PLTE", "tRNS", "tEXt", "IDAT", "IEND"}));
    const std::string text(written.begin(), written.end());
    EXPECT_NE(text.find(std::string("keep\0me", 7)), std::string::npos);
    draw::SpriteDoc reread;
    ASSERT_TRUE(draw::decodeSprite("p.png", written, reread, problem)) << problem.message;
    EXPECT_EQ(reread.doc.transparentIndex, 0);
    EXPECT_EQ(draw::celCanvas(reread.doc, 0, 0).indexAt(0, 0), 2);
    EXPECT_EQ(draw::celCanvas(reread.doc, 0, 0).indexAt(1, 0), 2);
}

TEST(StructureOps, GrowingAnOpaquePalettePngWrites) {
    // no clear entry: decoded as a background layer, whose growth used to crash
    const std::vector<uint8_t> file = s47test::palettePng(1, 1, {1}, {{0, 0, 0}, {255, 0, 0}});
    draw::SpriteDoc sprite;
    PixelProblem problem;
    ASSERT_TRUE(draw::decodeSprite("o.png", file, sprite, problem)) << problem.message;
    S47_APPLY(sprite, json::array({{{"op", "canvas"}, {"action", "resize"}, {"size", {2, 1}}, {"anchor", "left"}}}));
    std::vector<uint8_t> written;
    EXPECT_TRUE(draw::encodeSprite(sprite, written, problem)) << problem.message;
}
