// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The .aseprite codec against files Aseprite wrote and the PNGs Aseprite exported from them
// (docs/specs/S44-aseprite-import.md AC1, AC2, AC4, AC5; the corpus and its sources are in
// editor/tests/fixtures/aseprite/README.md).

#include "AsepriteTestSupport.h"

#include "pixelart/aseprite/AsepriteCodecInternal.h"

#include <gtest/gtest.h>

#include <set>

using namespace jasperengine::editor::pixelart;
using namespace jasperengine::editor::pixelart::aseprite;
using namespace jasperengine::editor::pixelart::testing;
namespace fs = std::filesystem;

namespace {

    fs::path asefile(const std::string& name) { return asepriteFixtures() / "asefile" / name; }
    fs::path upstream(const std::string& name) { return asepriteFixtures() / "aseprite-tests" / name; }
    fs::path tenten(const std::string& name) { return asepriteFixtures() / "tenten" / name; }

    // the chunk types of each frame, from a file's provenance
    std::vector<std::vector<uint16_t>> chunkTypes(const Document& doc) {
        std::vector<std::vector<uint16_t>> out;
        if (!doc.provenance) return out;
        for (const detail::FrameRecord& f : doc.provenance->frames) {
            std::vector<uint16_t> types;
            for (const detail::ChunkRecord& c : f.chunks) types.push_back(c.type);
            out.push_back(types);
        }
        return out;
    }

    // chunk byte ranges [offset, size) of a file, frame by frame, for diffs of chunk lists
    struct ChunkBytes {
        uint16_t type;
        std::vector<uint8_t> bytes;
    };
    std::vector<std::vector<ChunkBytes>> splitChunks(const std::vector<uint8_t>& file) {
        ReadResult r = read(file);
        std::vector<std::vector<ChunkBytes>> out;
        if (!r.ok()) return out;
        for (const detail::FrameRecord& f : r.document->provenance->frames) {
            std::vector<ChunkBytes> chunks;
            for (const detail::ChunkRecord& c : f.chunks) {
                chunks.push_back({c.type, std::vector<uint8_t>(file.begin() + static_cast<std::ptrdiff_t>(c.offset),
                                                               file.begin() + static_cast<std::ptrdiff_t>(c.offset + c.size))});
            }
            out.push_back(std::move(chunks));
        }
        return out;
    }

    const PropertyValue& prop(const UserData& ud, uint32_t map, const std::string& name) {
        static const PropertyValue none;
        auto it = ud.properties.find(map);
        if (it == ud.properties.end()) return none;
        const PropertyValue* v = findProperty(it->second, name);
        return v ? *v : none;
    }

    int64_t asInt(const PropertyValue& v) {
        if (const int64_t* i = std::get_if<int64_t>(&v.value)) return *i;
        if (const uint64_t* u = std::get_if<uint64_t>(&v.value)) return static_cast<int64_t>(*u);
        return INT64_MIN;
    }

    uint32_t extensionId(const Document& doc, const std::string& name) {
        for (const ExternalFile& f : doc.externalFiles) {
            if (f.name == name) return f.id;
        }
        return 0;
    }

}

// AC1: the Overbuffed files decode, and buff/spiker equal Aseprite's own horizontal sheet exports
TEST(AsepriteCodecTest, ReadsTentenFixtures) {
    for (const char* name : {"blob", "brute", "buff", "dew", "gem", "petal", "shot", "spiker"}) {
        SCOPED_TRACE(name);
        Document doc = loadDocument(tenten(std::string(name) + ".aseprite"));
        EXPECT_EQ(doc.mode, ColorMode::Rgba);
        ASSERT_EQ(doc.frameCount(), 3);
        for (uint16_t d : doc.frameDurations) EXPECT_EQ(d, 125);
        ASSERT_TRUE(doc.colorProfile.has_value());
        EXPECT_EQ(doc.colorProfile->type, 1);   // sRGB
        // frame 0: colour profile, the old 8-bit palette chunk, the layer, a compressed cel
        const auto types = chunkTypes(doc);
        ASSERT_FALSE(types.empty());
        EXPECT_NE(std::find(types[0].begin(), types[0].end(), kChunkOldPalette), types[0].end());
        EXPECT_EQ(std::find(types[0].begin(), types[0].end(), kChunkPalette), types[0].end());
        EXPECT_EQ(doc.palette.size(), 256u);
        for (const Cel& cel : doc.cels) {
            EXPECT_TRUE(cel.image && !cel.image->empty());
            EXPECT_EQ(cel.linkedFrame, -1);
        }
    }
    for (const auto& [name, png] : {std::pair{"buff", "buff@22x22.png"}, std::pair{"spiker", "spiker@16x16.png"}}) {
        SCOPED_TRACE(name);
        const Document doc = loadDocument(tenten(std::string(name) + ".aseprite"));
        const PixelImage expected = loadPng(tenten(png));
        EXPECT_TRUE(samePicture(horizontalSheet(doc), expected));

        // red control: the same comparison with every cel offset ignored fails, so the goldens
        // see where cels are placed
        Document moved = doc;
        bool anyOffset = false;
        for (Cel& cel : moved.cels) {
            anyOffset = anyOffset || cel.x != 0 || cel.y != 0;
            cel.x = 0;
            cel.y = 0;
        }
        if (anyOffset) {
            EXPECT_FALSE(samePicture(horizontalSheet(moved), expected));
        }
    }
}

// AC2: every golden of the corpus, frame by frame (and layer by layer where Aseprite exported one)
TEST(AsepriteCodecTest, GoldenCorpus) {
    struct Case {
        const char* file;
        int frame;
        const char* png;
        const char* soloLayer = nullptr;
        bool legacyHsl = false;   // exported by Aseprite <= 1.3.12 (AsepriteBlendTest)
    };
    const Case cases[] = {
        {"basic-16x16.aseprite", 0, "basic-16x16.png"},
        {"layers_and_tags.aseprite", 0, "layers_and_tags_01.png"},
        {"layers_and_tags.aseprite", 1, "layers_and_tags_02.png"},
        {"layers_and_tags.aseprite", 2, "layers_and_tags_03.png"},
        {"layers_and_tags.aseprite", 3, "layers_and_tags_04.png"},
        {"layers_and_tags.aseprite", 2, "single_layer.png", "Layer 1"},
        {"big.aseprite", 0, "big.png"},
        {"transparency.aseprite", 0, "transparency_01.png"},
        {"transparency.aseprite", 1, "transparency_02.png"},
        {"background.aseprite", 0, "background.png"},
        {"linked_cels.aseprite", 0, "linked_cels_01.png"},
        {"linked_cels.aseprite", 1, "linked_cels_02.png"},
        {"linked_cels.aseprite", 2, "linked_cels_03.png"},
        {"indexed.aseprite", 0, "indexed_01.png"},
        {"grayscale.aseprite", 0, "grayscale.png"},
        {"256_color_old_palette_chunk.aseprite", 0, "256_color_old_palette_chunk.png"},
        {"tilemap.aseprite", 0, "tilemap.png"},
        {"tilemap_indexed.aseprite", 0, "tilemap_indexed.png"},
        {"tilemap_grayscale.aseprite", 0, "tilemap_grayscale.png"},
        {"tilemap_multi.aseprite", 0, "tilemap_multi.png"},
        {"tilemap_multi.aseprite", 0, "tilemap_multi_map1.png", "Tilemap 1"},
        {"tilemap_multi.aseprite", 0, "tilemap_multi_map2.png", "Tilemap 2"},
        {"blend_saturation_bug.aseprite", 0, "blend_saturation_bug.png", nullptr, true},
    };
    for (const Case& c : cases) {
        SCOPED_TRACE(std::string(c.file) + " frame " + std::to_string(c.frame) + " -> " + c.png);
        const Document doc = loadDocument(asefile(c.file));
        RenderOptions options;
        options.legacyHsl = c.legacyHsl;
        if (c.soloLayer) {
            options.soloLayer = doc.findLayer(c.soloLayer);
            ASSERT_GE(options.soloLayer, 0);
        }
        EXPECT_TRUE(samePicture(renderFrame(doc, c.frame, options), loadPng(asefile(c.png))));
    }

    // the four frames as one horizontal sheet
    EXPECT_TRUE(samePicture(horizontalSheet(loadDocument(asefile("layers_and_tags.aseprite"))), loadPng(asefile("layers_and_tags.png"))));

    // tilesets: the strip, single tiles, the tiles a tilemap uses
    {
        const Document doc = loadDocument(asefile("tileset.aseprite"));
        ASSERT_EQ(doc.tilesets.size(), 2u);
        EXPECT_EQ(doc.tilesets[0].id, 0u);
        EXPECT_TRUE(samePicture(doc.tilesets[0].tiles, loadPng(asefile("tileset.png"))));
        EXPECT_TRUE(samePicture(doc.tilesets[0].tile(1), loadPng(asefile("tileset_1.png"))));
    }
    {
        const Document doc = loadDocument(asefile("tilemap_multi.aseprite"));
        const int layer = doc.findLayer("Tilemap 1");
        ASSERT_GE(layer, 0);
        const Cel* cel = doc.cel(layer, 0);
        ASSERT_TRUE(cel && cel->tilemap);
        const Tilemap& tm = *cel->tilemap;
        const Tileset* tilemapTileset = nullptr;
        for (const Tileset& t : doc.tilesets) {
            if (t.id == doc.layers[static_cast<size_t>(layer)].tilesetIndex) tilemapTileset = &t;
        }
        ASSERT_TRUE(tilemapTileset);
        // asefile addresses tiles on the canvas grid (13 x 16 tiles of 20x16 here); the cel sits at
        // (0, 16), one row down, and holds the tiles from there
        EXPECT_EQ(doc.width / tilemapTileset->tileWidth + (doc.width % tilemapTileset->tileWidth ? 1 : 0), 13);
        EXPECT_EQ(doc.height / tilemapTileset->tileHeight, 16);
        auto id = [&](int u, int v) -> uint32_t {
            const int cu = u - cel->x / tilemapTileset->tileWidth;
            const int cv = v - cel->y / tilemapTileset->tileHeight;
            if (cu < 0 || cv < 0 || cu >= tm.columns || cv >= tm.rows) return 0;
            return tm.at(cu, cv) & tm.idMask;
        };
        EXPECT_EQ(id(0, 0), 0u);
        EXPECT_EQ(id(0, 2), 4u);
        EXPECT_EQ(id(0, 3), 2u);
        EXPECT_EQ(id(11, 5), 3u);
        EXPECT_EQ(id(12, 15), 0u);
        EXPECT_EQ(id(4, 7), 3u);
        const Tileset* ts = nullptr;
        for (const Tileset& t : doc.tilesets) {
            if (t.id == doc.layers[static_cast<size_t>(layer)].tilesetIndex) ts = &t;
        }
        ASSERT_TRUE(ts);
        EXPECT_TRUE(samePicture(ts->tile(3), loadPng(asefile("tilemap_single_tile_1.png"))));
    }
    {
        const Document doc = loadDocument(asefile("tilemap_empty_edges.aseprite"));
        const Cel* cel = doc.cel(0, 0);
        ASSERT_TRUE(cel && cel->tilemap);
        EXPECT_EQ(cel->tilemap->at(0, 0) & cel->tilemap->idMask, 1u);
        EXPECT_TRUE(samePicture(doc.tilesets[0].tile(1), loadPng(asefile("tilemap_empty_edges_0_0.png"))));
        EXPECT_TRUE(samePicture(doc.tilesets[0].tile(static_cast<int>(cel->tilemap->at(0, 1) & cel->tilemap->idMask)),
                                loadPng(asefile("tilemap_empty_edges_0_1.png"))));
    }
}

// AC2: what the files say, as Aseprite's own tests and asefile's know it
TEST(AsepriteCodecTest, KnownContent) {
    {
        const Document doc = loadDocument(asefile("layers_and_tags.aseprite"));
        EXPECT_EQ(doc.frameCount(), 4);
        EXPECT_EQ(doc.layers.size(), 6u);
        EXPECT_EQ(doc.tags.size(), 3u);
        EXPECT_EQ(doc.findLayer("Layer 1"), 1);
        EXPECT_FALSE(doc.layers[2].visible());   // "invisible"
        EXPECT_TRUE(doc.layers[3].isGroup());
        EXPECT_EQ(doc.layers[4].parent, 3);
        EXPECT_EQ(doc.layerPath(4), "Group 1/Layer 5");
    }
    {
        const Document doc = loadDocument(asefile("palette.aseprite"));
        ASSERT_EQ(doc.palette.size(), 85u);
        EXPECT_EQ(doc.palette[0].color, (PixelRgba{46, 34, 47, 255}));
        EXPECT_EQ(doc.palette[71].color, (PixelRgba{0, 0, 0, 83}));
    }
    {
        const Document doc = loadDocument(asefile("256_color_old_palette_chunk.aseprite"));
        EXPECT_EQ(doc.mode, ColorMode::Indexed);
        EXPECT_EQ(doc.transparentIndex, 0);
        EXPECT_EQ(doc.palette.size(), 256u);
    }
    {
        const Document doc = loadDocument(asefile("slice_advanced.aseprite"));
        ASSERT_EQ(doc.slices.size(), 2u);
        EXPECT_EQ(doc.slices[0].name, "Slice 1");
        std::vector<uint32_t> frames;
        for (const SliceKey& k : doc.slices[0].keys) frames.push_back(k.frame);
        EXPECT_EQ(frames, (std::vector<uint32_t>{0, 1, 2, 3}));
        ASSERT_TRUE(doc.slices[0].keys[0].pivot);
        EXPECT_EQ(doc.slices[0].keys[0].pivot->x, 4);
        ASSERT_EQ(doc.slices[1].keys.size(), 1u);
        ASSERT_TRUE(doc.slices[1].keys[0].center);
        EXPECT_EQ(*doc.slices[1].keys[0].center, (IntRect{3, 3, 2, 2}));
        // keys hold until the next one
        EXPECT_EQ(doc.slices[1].keyAt(3), &doc.slices[1].keys[0]);
    }
    {
        const Document doc = loadDocument(asefile("user_data.aseprite"));
        EXPECT_EQ(doc.spriteUserData.text, std::optional<std::string>("test_user_data_sprite"));
        EXPECT_EQ(doc.spriteUserData.color, std::optional<PixelRgba>(PixelRgba{0, 255, 0, 255}));
        EXPECT_EQ(doc.layers[0].userData.text, std::optional<std::string>("test_user_data_layer"));
        EXPECT_EQ(doc.layers[0].userData.color, std::optional<PixelRgba>(PixelRgba{255, 0, 0, 255}));
        const Cel* cel = doc.celRecord(0, 0);
        ASSERT_TRUE(cel);
        EXPECT_EQ(cel->userData.text, std::optional<std::string>("test_user_data_cel"));
        EXPECT_EQ(cel->userData.color, std::optional<PixelRgba>(PixelRgba{0, 255, 0, 255}));
        ASSERT_EQ(doc.tags.size(), 3u);
        EXPECT_EQ(doc.tags[0].userData.text, std::optional<std::string>("test_user_data_tag_0"));
        EXPECT_EQ(doc.tags[0].userData.color, std::optional<PixelRgba>(PixelRgba{0, 255, 0, 255}));
        EXPECT_FALSE(doc.tags[1].userData.text.has_value());
        EXPECT_EQ(doc.tags[1].userData.color, std::optional<PixelRgba>(PixelRgba{0, 0, 0, 255}));
        EXPECT_EQ(doc.tags[2].userData.text, std::optional<std::string>("test_user_data_tag_2"));
        EXPECT_EQ(doc.tags[2].userData.color, std::optional<PixelRgba>(PixelRgba{255, 0, 0, 255}));
    }
    {
        // the cel reaches past the canvas: the frame is still the canvas size
        const Document doc = loadDocument(asefile("cel_overflow.aseprite"));
        const PixelImage frame = renderFrame(doc, 0);
        EXPECT_EQ(frame.width(), doc.width);
        EXPECT_EQ(frame.height(), doc.height);
    }
    {
        // Aseprite's tests/sprites/README.md
        const Document abcd = loadDocument(upstream("abcd.aseprite"));
        EXPECT_EQ(abcd.mode, ColorMode::Indexed);
        EXPECT_EQ(abcd.width, 32);
        std::vector<std::string> names;
        for (const Layer& l : abcd.layers) names.push_back(l.name);
        EXPECT_EQ(names, (std::vector<std::string>{"a", "b", "c", "d"}));

        const Document empty3 = loadDocument(upstream("1empty3.aseprite"));
        EXPECT_EQ(empty3.mode, ColorMode::Rgba);
        ASSERT_EQ(empty3.tags.size(), 2u);
        EXPECT_EQ(empty3.tags[0].name, "a");
        EXPECT_EQ(empty3.tags[1].name, "b");
        // the 2nd frame is completely empty
        EXPECT_TRUE(renderFrame(empty3, 1).opaqueBounds().empty());

        const Document g2 = loadDocument(upstream("groups2.aseprite"));
        EXPECT_EQ(g2.findLayer("items/gun") >= 0, true);
        EXPECT_FALSE(g2.layers[static_cast<size_t>(g2.findLayer("items/gun"))].visible());
        EXPECT_EQ(g2.layerPath(g2.findLayer("player/body")), "player/body");

        const Document bg3 = loadDocument(upstream("bg-index-3.aseprite"));
        EXPECT_EQ(bg3.transparentIndex, 3);

        const Document t3 = loadDocument(upstream("tags3.aseprite"));
        ASSERT_EQ(t3.tags.size(), 3u);
        EXPECT_EQ(t3.tags[0].name, "forward");
        EXPECT_EQ(t3.tags[1].direction, TagDirection::Reverse);
        EXPECT_EQ(t3.tags[2].direction, TagDirection::PingPong);
        // several linked cels
        int links = 0;
        for (const Cel& c : t3.cels) links += c.linkedFrame >= 0 ? 1 : 0;
        EXPECT_GT(links, 0);

        const Document reps = loadDocument(upstream("tags3x123reps.aseprite"));
        ASSERT_EQ(reps.tags.size(), 3u);
        EXPECT_EQ(reps.tags[0].name, "forward");
        EXPECT_EQ(reps.tags[0].repeat, 1);
        EXPECT_EQ(reps.tags[1].direction, TagDirection::Reverse);
        EXPECT_EQ(reps.tags[1].repeat, 1);
        EXPECT_EQ(reps.tags[2].direction, TagDirection::PingPong);
        EXPECT_EQ(reps.tags[2].repeat, 2);

        const Document moving = loadDocument(upstream("slices-moving.aseprite"));
        ASSERT_EQ(moving.slices.size(), 1u);
        EXPECT_EQ(moving.slices[0].keys.size(), 4u);
        EXPECT_TRUE(moving.layers[0].background());

        const Document tiles = loadDocument(upstream("2x2tilemap2x2tile.aseprite"));
        ASSERT_EQ(tiles.tilesets.size(), 1u);
        EXPECT_EQ(tiles.tilesets[0].tileCount, 5);
        EXPECT_EQ(tiles.tilesets[0].tileWidth, 2);
    }
}

// AC2: every one of Aseprite's 19 property types and extension maps (tests/scripts/userdata_codec.lua)
TEST(AsepriteCodecTest, UserDataProperties) {
    const Document doc = loadDocument(upstream("file-tests-props.aseprite"));
    const uint32_t ext = extensionId(doc, "ext");
    ASSERT_NE(ext, 0u);

    const UserData& sprite = doc.spriteUserData;
    ASSERT_TRUE(sprite.userProperties());
    EXPECT_EQ(sprite.userProperties()->size(), 5u);
    EXPECT_EQ(std::get<bool>(prop(sprite, 0, "a").value), true);
    EXPECT_EQ(asInt(prop(sprite, 0, "b")), 1);
    EXPECT_EQ(std::get<std::string>(prop(sprite, 0, "c").value), "hi");
    EXPECT_EQ(std::get<double>(prop(sprite, 0, "d").value), 2.3);
    EXPECT_EQ(std::get<double>(prop(sprite, 0, "e").value), 8.123456e-12);
    const PropertyVector& ea = std::get<PropertyVector>(prop(sprite, ext, "a").value);
    ASSERT_EQ(ea.size(), 3u);
    EXPECT_EQ(std::get<std::string>(ea[2].value), "three");

    // Lua's spr.layers[2] is the second top-level layer from the bottom: "Layer 3"
    const UserData& layer = doc.layers[static_cast<size_t>(doc.findLayer("Layer 3"))].userData;
    EXPECT_EQ(std::get<std::string>(prop(layer, 0, "a").value), "i'm the layer 3");
    EXPECT_EQ(asInt(prop(layer, ext, "a")), 100);

    ASSERT_EQ(doc.tilesets.size(), 1u);
    const Tileset& ts = doc.tilesets[0];
    EXPECT_EQ(std::get<std::string>(prop(ts.userData, 0, "a").value), "i'm a tilemap");
    EXPECT_EQ(asInt(prop(ts.userData, 0, "b")), 11);
    EXPECT_EQ(std::get<std::string>(prop(ts.userData, ext, "a").value), "text from extension");
    ASSERT_GE(ts.tileUserData.size(), 3u);
    EXPECT_EQ(asInt(prop(ts.tileUserData[1], 0, "a")), 320);
    EXPECT_EQ(asInt(prop(ts.tileUserData[1], 0, "b")), 330);
    EXPECT_EQ(asInt(prop(ts.tileUserData[2], 0, "a")), 640);
    EXPECT_EQ(asInt(prop(ts.tileUserData[2], 0, "b")), 650);

    const UserData& tag = doc.tags[0].userData;
    EXPECT_EQ(std::get<IntPoint>(prop(tag, 0, "a").value), (IntPoint{1, 2}));
    const PropertyMap& b = std::get<PropertyMap>(prop(tag, 0, "b").value);
    EXPECT_EQ(std::get<std::string>(findProperty(b, "a")->value), "text");
    EXPECT_EQ(std::get<double>(findProperty(b, "b")->value), 35.567);
    EXPECT_EQ(std::get<IntRect>(findProperty(b, "c")->value), (IntRect{1, 2, 3, 4}));
    const PropertyMap& tagExt = std::get<PropertyMap>(prop(tag, ext, "a").value);
    EXPECT_EQ(std::get<IntSize>(findProperty(tagExt, "a")->value), (IntSize{50, 60}));
    EXPECT_EQ(asInt(*findProperty(tagExt, "b")), 43985943434);

    // spr.layers[4].cels[4]: the bottom layer's 4th cel (frame 3)
    const Cel* cel = doc.celRecord(doc.findLayer("Layer 1"), 3);
    ASSERT_TRUE(cel);
    const PropertyVector& ca = std::get<PropertyVector>(prop(cel->userData, 0, "a").value);
    ASSERT_EQ(ca.size(), 3u);
    EXPECT_EQ(asInt(ca[0]), 1);
    EXPECT_EQ(asInt(ca[2]), 3);
    const PropertyMap& cb = std::get<PropertyMap>(prop(cel->userData, 0, "b").value);
    EXPECT_EQ(std::get<std::string>(findProperty(cb, "c")->value), "6");
    const PropertyVector& cext = std::get<PropertyVector>(prop(cel->userData, ext, "a").value);
    EXPECT_EQ(std::get<std::string>(cext[1].value), "b");

    const UserData& slice = doc.slices[0].userData;
    EXPECT_EQ(std::get<IntPoint>(prop(slice, 0, "a").value), (IntPoint{3, 4}));
    EXPECT_EQ(std::get<IntSize>(prop(slice, 0, "b").value), (IntSize{10, 20}));
    EXPECT_EQ(std::get<IntRect>(prop(slice, ext, "a").value), (IntRect{10, 20, 30, 40}));
}

// Aseprite's tests/scripts/z_order.lua, with its compose_groups preference on
TEST(AsepriteCodecTest, ZOrderLikeAseprite) {
    Document doc = loadDocument(upstream("z-order.aseprite"));
    // Lua's s.layers are the top-level layers bottom first: L1 L2 L3 L4 G1 L5
    const int L3 = doc.findLayer("L3"), L4 = doc.findLayer("L4"), G1 = doc.findLayer("G1"), L5 = doc.findLayer("L5");
    ASSERT_TRUE(L3 >= 0 && L4 >= 0 && G1 >= 0 && L5 >= 0);
    RenderOptions compose;
    compose.groups = GroupMode::Compose;
    auto pixel = [&](int x, int y) { return renderFrame(doc, 0, compose).at(x, y); };
    auto setVisible = [&](int layer, bool on) {
        uint16_t& f = doc.layers[static_cast<size_t>(layer)].flags;
        f = on ? (f | kLayerVisible) : (f & ~kLayerVisible);
    };
    doc.celRecord(L4, 0)->zIndex = -1;
    setVisible(G1, false);
    setVisible(L5, false);
    EXPECT_EQ(pixel(1, 5), (PixelRgba{0, 255, 0, 255}));
    setVisible(L3, false);
    EXPECT_EQ(pixel(1, 5), (PixelRgba{255, 0, 0, 255}));
    setVisible(L3, true);
    doc.celRecord(L4, 0)->zIndex = 1;
    setVisible(G1, true);
    setVisible(L5, true);
    EXPECT_EQ(pixel(0, 0), (PixelRgba{64, 64, 64, 255}));
    setVisible(G1, false);
    EXPECT_EQ(pixel(0, 0), (PixelRgba{64, 64, 64, 255}));
}

// Aseprite's tests/cli/save-as.sh: which layers --layer renders (hidden children of a selected
// group stay hidden, a layer asked for by name shows, a/* is every child)
TEST(AsepriteCodecTest, LayerSelectionLikeAseprite) {
    const Document g3 = loadDocument(upstream("groups3abc.aseprite"));
    auto only = [&](std::vector<std::string> paths) {
        RenderOptions o;
        for (const std::string& p : paths) o.layers.push_back(g3.findLayer(p));
        return renderFrame(g3, 0, o);
    };
    auto children = [&](const std::string& group) {
        RenderOptions o;
        for (int c : g3.children(g3.findLayer(group))) o.layers.push_back(c);
        return renderFrame(g3, 0, o);
    };
    EXPECT_EQ(only({"a"}), only({"a/a", "a/b", "a/c"}));
    EXPECT_EQ(only({"b"}), only({"b/a", "b/c"}));
    EXPECT_EQ(only({"c"}), only({"c/c"}));
    EXPECT_EQ(children("b"), only({"b/a", "b/b", "b/c"}));
    EXPECT_EQ(children("c"), only({"c/a", "c/b", "c/c"}));
    EXPECT_NE(only({"b/b"}), PixelImage(g3.width, g3.height));   // hidden, but shown when asked for
}

// AC4: write(read(f)) == f byte for byte, for every file of the corpus
TEST(AsepriteCodecTest, ResaveIsIdentical) {
    const std::vector<fs::path> corpus = asepriteCorpus();
    ASSERT_GE(corpus.size(), 40u);
    for (const fs::path& path : corpus) {
        SCOPED_TRACE(path.filename().string());
        const std::vector<uint8_t> original = readBytes(path);
        ReadResult r = read(original);
        ASSERT_TRUE(r.ok()) << r.error.kind << ": " << r.error.message;
        std::vector<uint8_t> written;
        CodecError error;
        ASSERT_TRUE(write(*r.document, written, &error)) << error.message;
        EXPECT_TRUE(written == original) << "written " << written.size() << " bytes, original " << original.size();
    }
}

// AC4: one changed pixel re-encodes that cel chunk, its frame's header and the file size only
TEST(AsepriteCodecTest, EditTouchesOnlyItsChunks) {
    const std::vector<uint8_t> original = readBytes(asefile("layers_and_tags.aseprite"));
    ReadResult r = read(original);
    ASSERT_TRUE(r.ok());
    Document doc = std::move(*r.document);
    Cel* cel = doc.celRecord(1, 1);
    ASSERT_TRUE(cel && cel->image);
    PixelImage edited = *cel->image;
    PixelRgba c = edited.at(0, 0);
    c[0] = static_cast<uint8_t>(c[0] ^ 0x40);
    c[3] = 255;
    edited.set(0, 0, c);
    cel->image = std::make_shared<const PixelImage>(edited);

    std::vector<uint8_t> written;
    CodecError error;
    ASSERT_TRUE(write(doc, written, &error)) << error.message;
    ASSERT_NE(written, original);
    // the header differs in the file size only
    EXPECT_TRUE(std::equal(original.begin() + 4, original.begin() + 128, written.begin() + 4));
    const auto before = splitChunks(original);
    const auto after = splitChunks(written);
    ASSERT_EQ(before.size(), after.size());
    int changed = 0;
    for (size_t f = 0; f < before.size(); f++) {
        ASSERT_EQ(before[f].size(), after[f].size());
        for (size_t i = 0; i < before[f].size(); i++) {
            EXPECT_EQ(before[f][i].type, after[f][i].type);
            if (before[f][i].bytes != after[f][i].bytes) {
                changed++;
                EXPECT_EQ(f, 1u);
                EXPECT_EQ(before[f][i].type, kChunkCel);
            }
        }
    }
    EXPECT_EQ(changed, 1);
    // the edit reads back, everything else is the same document
    ReadResult back = read(written);
    ASSERT_TRUE(back.ok());
    EXPECT_EQ(back.document->celRecord(1, 1)->image->at(0, 0), c);
    EXPECT_EQ(describe(*back.document, Detail::Cels), describe(doc, Detail::Cels));
}

namespace {

    Document makeDocument(ColorMode mode) {
        Document d;
        d.width = 8;
        d.height = 6;
        d.mode = mode;
        d.headerFlags = kHeaderLayerOpacity | kHeaderGroupBlend | kHeaderLayerUuids;
        d.transparentIndex = mode == ColorMode::Indexed ? 2 : 0;
        d.frameDurations = {100, 80, 120};
        for (int i = 0; i < 6; i++) {
            PaletteEntry e;
            e.color = {static_cast<uint8_t>(i * 40), static_cast<uint8_t>(255 - i * 30), static_cast<uint8_t>(i * 7), 255};
            if (i == 4) e.name = "skin";
            d.palette.push_back(e);
        }
        Layer bg;
        bg.name = "Background";
        bg.flags = kLayerVisible | kLayerEditable | kLayerBackground | kLayerLockMovement;
        Layer group;
        group.name = "Body";
        group.type = LayerType::Group;
        group.blend = BlendMode::Multiply;
        group.opacity = 200;
        Layer arm;
        arm.name = "Arm";
        arm.childLevel = 1;
        arm.blend = BlendMode::Screen;
        arm.opacity = 128;
        Layer hidden;
        hidden.name = "_guide";
        hidden.flags = kLayerEditable;
        hidden.childLevel = 1;
        Layer reference;
        reference.name = "ref";
        reference.flags = kLayerVisible | kLayerReference;
        d.layers = {bg, group, arm, hidden, reference};
        for (size_t i = 0; i < d.layers.size(); i++) {
            Uuid u;
            u.bytes[0] = static_cast<uint8_t>(i + 1);
            u.bytes[15] = 0xAB;
            d.layers[i].uuid = u;
        }
        d.rebuildTreeFromLevels();
        const std::vector<uint8_t> pal = d.paletteRgba();
        auto image = [&](int w, int h, uint8_t seed, bool background) {
            if (mode == ColorMode::Indexed) {
                std::vector<uint8_t> idx(static_cast<size_t>(w) * h);
                for (size_t i = 0; i < idx.size(); i++) idx[i] = static_cast<uint8_t>((i + seed) % 6);
                return std::make_shared<const PixelImage>(PixelImage::fromIndices(w, h, idx, pal, background ? -1 : 2));
            }
            PixelImage img(w, h);
            for (int y = 0; y < h; y++) {
                for (int x = 0; x < w; x++) {
                    const uint8_t v = static_cast<uint8_t>(seed * 31 + x * 17 + y * 5);
                    const uint8_t a = static_cast<uint8_t>((x + y + seed) % 3 == 0 ? 0 : 255 - x * 9);
                    if (mode == ColorMode::Grayscale) img.set(x, y, {v, v, v, a});
                    else img.set(x, y, {v, static_cast<uint8_t>(v * 3), static_cast<uint8_t>(255 - v), a});
                }
            }
            return std::make_shared<const PixelImage>(img);
        };
        for (int f = 0; f < 3; f++) {
            Cel bgCel;
            bgCel.layer = 0;
            bgCel.frame = f;
            bgCel.image = image(8, 6, static_cast<uint8_t>(f), true);
            d.cels.push_back(bgCel);
            Cel armCel;
            armCel.layer = 2;
            armCel.frame = f;
            armCel.x = f == 2 ? -2 : 3;
            armCel.y = 1;
            armCel.opacity = 190;
            armCel.zIndex = static_cast<int16_t>(f == 1 ? -1 : 0);
            if (f == 1) {
                armCel.linkedFrame = 0;
                armCel.x = 3;
                armCel.image = d.cels[1].image;
            } else {
                armCel.image = image(4, 4, static_cast<uint8_t>(f + 7), false);
            }
            d.cels.push_back(armCel);
        }
        d.cels[1].userData.text = "arm frame 0";
        Cel refCel;
        refCel.layer = 4;
        refCel.frame = 0;
        refCel.image = image(2, 2, 3, false);
        refCel.extra = CelExtra{1, 1 << 15, 3 << 15, 5 << 16, 2 << 16};
        d.cels.push_back(refCel);
        d.sortCels();

        Tag walk;
        walk.name = "walk";
        walk.from = 0;
        walk.to = 2;
        walk.direction = TagDirection::PingPongReverse;
        walk.repeat = 3;
        walk.legacyColor = {10, 20, 30, 255};
        walk.userData.color = PixelRgba{200, 10, 10, 255};
        Tag idle;
        idle.name = "idle";
        d.tags = {walk, idle};

        Slice hurt;
        hurt.name = "hurt";
        hurt.keys.push_back({0, {1, 1, 4, 3}, IntRect{1, 1, 2, 1}, IntPoint{2, 3}});
        // with a 9-patch or pivot on one key every key has one (the file has the fields per key)
        hurt.keys.push_back({2, {0, 0, 0, 0}, IntRect{}, IntPoint{}});
        hurt.userData.text = "sensor";
        d.slices = {hurt};

        // every property type, mixed vectors and three levels of nesting
        PropertyMap nested2 = {{"deep", PropertyValue::string("bottom")}};
        PropertyMap nested1 = {{"level2", PropertyValue::map(nested2)}, {"n", PropertyValue::integer(-70000)}};
        PropertyValue f32{PropertyType::Float, 1.5, 0};
        PropertyValue fixed{PropertyType::Fixed, -2.25, 0};
        PropertyValue i8{PropertyType::Int8, int64_t{-5}, 0};
        PropertyValue i16{PropertyType::Int16, int64_t{-300}, 0};
        PropertyValue i32{PropertyType::Int32, int64_t{-100000}, 0};
        PropertyValue i64{PropertyType::Int64, int64_t{-5000000000LL}, 0};
        PropertyValue u8{PropertyType::Uint8, uint64_t{200}, 0};
        PropertyValue u16{PropertyType::Uint16, uint64_t{60000}, 0};
        PropertyValue u32{PropertyType::Uint32, uint64_t{4000000000u}, 0};
        PropertyValue u64{PropertyType::Uint64, uint64_t{18000000000000000000ull}, 0};
        Uuid uuid;
        for (int i = 0; i < 16; i++) uuid.bytes[static_cast<size_t>(i)] = static_cast<uint8_t>(i * 13);
        PropertyMap all = {
            {"bool", PropertyValue::boolean(true)}, {"i8", i8}, {"u8", u8}, {"i16", i16}, {"u16", u16}, {"i32", i32}, {"u32", u32},
            {"i64", i64}, {"u64", u64}, {"fixed", fixed}, {"float", f32}, {"double", PropertyValue::number(0.1)},
            {"string", PropertyValue::string("héllo")}, {"point", PropertyValue::point({-1, 2})}, {"size", PropertyValue::size({3, 4})},
            {"rect", PropertyValue::rect({5, 6, 7, 8})},
            {"mixed", PropertyValue::vector({PropertyValue::integer(3), PropertyValue::string("x"), PropertyValue::vector({PropertyValue::integer(1)})})},
            {"same", PropertyValue::vector({PropertyValue::integer(1), PropertyValue::integer(2)})},
            {"nested", PropertyValue::map(nested1)}, {"uuid", PropertyValue::uuid(uuid)},
            {"light2d.intensity", PropertyValue::number(2.5)},
        };
        d.spriteUserData.properties[0] = all;
        d.spriteUserData.properties[1] = {{"ext_value", PropertyValue::integer(9)}};
        d.externalFiles = {{1, 2, "me/MyExtension"}};

        Tileset ts;
        ts.id = 0;
        ts.name = "ground";
        ts.tileWidth = 2;
        ts.tileHeight = 2;
        ts.tileCount = 3;
        ts.baseIndex = 1;
        ts.flags = 2 | 4 | 8;
        if (mode == ColorMode::Indexed) {
            std::vector<uint8_t> idx(2 * 6);
            for (size_t i = 0; i < idx.size(); i++) idx[i] = static_cast<uint8_t>(i < 4 ? 2 : i % 6);
            ts.tiles = PixelImage::fromIndices(2, 6, idx, pal, 2);
        } else {
            ts.tiles = PixelImage(2, 6);
            for (int y = 2; y < 6; y++) {
                for (int x = 0; x < 2; x++) {
                    const uint8_t v = static_cast<uint8_t>(y * 40 + x * 10);
                    ts.tiles.set(x, y, {v, v, v, 255});
                }
            }
        }
        ts.userData.text = "ground tiles";
        ts.tileUserData.resize(3);
        ts.tileUserData[2].properties[0] = {{"solid", PropertyValue::boolean(true)}};
        d.tilesets = {ts};
        Layer tilemapLayer;
        tilemapLayer.name = "Tiles";
        tilemapLayer.uuid = Uuid{{9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9}};
        tilemapLayer.type = LayerType::Tilemap;
        tilemapLayer.tilesetIndex = 0;
        d.layers.push_back(tilemapLayer);
        d.rebuildTreeFromLevels();
        Cel tilemapCel;
        tilemapCel.layer = 5;
        tilemapCel.frame = 0;
        auto tm = std::make_shared<Tilemap>();
        tm->columns = 3;
        tm->rows = 2;
        tm->tiles = {1, 2 | 0x80000000u, 0, 2 | 0x40000000u, 1 | 0x20000000u, 2 | 0xE0000000u};
        tilemapCel.tilemap = tm;
        d.cels.push_back(tilemapCel);
        d.sortCels();

        OpaqueChunk unknown;
        unknown.type = 0x7777;
        unknown.frame = 1;
        unknown.body = {1, 2, 3, 4, 5};
        d.opaque.push_back(unknown);
        if (mode != ColorMode::Indexed) {
            ColorProfile profile;
            profile.type = 2;
            profile.icc = {0x10, 0x20, 0x30};
            d.colorProfile = profile;
        }
        return d;
    }

    void expectSameDocument(const Document& a, const Document& b) {
        EXPECT_EQ(describe(a, Detail::Cels), describe(b, Detail::Cels));
        EXPECT_EQ(a.headerFlags, b.headerFlags);
        EXPECT_EQ(a.palette, b.palette);
        ASSERT_EQ(a.layers.size(), b.layers.size());
        for (size_t i = 0; i < a.layers.size(); i++) {
            EXPECT_EQ(a.layers[i].userData, b.layers[i].userData);
            EXPECT_TRUE(a.layers[i].uuid == b.layers[i].uuid);
        }
        EXPECT_EQ(a.spriteUserData, b.spriteUserData);
        ASSERT_EQ(a.cels.size(), b.cels.size());
        for (size_t i = 0; i < a.cels.size(); i++) {
            const Cel& x = a.cels[i];
            const Cel& y = b.cels[i];
            EXPECT_EQ(x.layer, y.layer);
            EXPECT_EQ(x.frame, y.frame);
            EXPECT_EQ(x.linkedFrame, y.linkedFrame);
            EXPECT_EQ(x.userData, y.userData);
            EXPECT_EQ(x.extra.has_value(), y.extra.has_value());
            if (x.extra && y.extra) {
                EXPECT_TRUE(*x.extra == *y.extra);
            }
            if (x.image && y.image) {
                EXPECT_EQ(*x.image, *y.image);
            }
            if (x.tilemap && y.tilemap) {
                EXPECT_TRUE(*x.tilemap == *y.tilemap);
            }
        }
        ASSERT_EQ(a.tags.size(), b.tags.size());
        for (size_t i = 0; i < a.tags.size(); i++) EXPECT_EQ(a.tags[i].userData, b.tags[i].userData);
        ASSERT_EQ(a.slices.size(), b.slices.size());
        for (size_t i = 0; i < a.slices.size(); i++) {
            EXPECT_EQ(a.slices[i].keys, b.slices[i].keys);
            EXPECT_EQ(a.slices[i].userData, b.slices[i].userData);
        }
        ASSERT_EQ(a.tilesets.size(), b.tilesets.size());
        for (size_t i = 0; i < a.tilesets.size(); i++) {
            EXPECT_EQ(a.tilesets[i].tiles, b.tilesets[i].tiles);
            EXPECT_EQ(a.tilesets[i].userData, b.tilesets[i].userData);
            ASSERT_GE(b.tilesets[i].tileUserData.size(), 3u);
            EXPECT_EQ(a.tilesets[i].tileUserData[2], b.tilesets[i].tileUserData[2]);
        }
        EXPECT_EQ(a.opaque.size(), b.opaque.size());
        for (size_t i = 0; i < std::min(a.opaque.size(), b.opaque.size()); i++) {
            EXPECT_EQ(a.opaque[i].type, b.opaque[i].type);
            EXPECT_EQ(a.opaque[i].frame, b.opaque[i].frame);
            EXPECT_EQ(a.opaque[i].body, b.opaque[i].body);
        }
        EXPECT_EQ(a.colorProfile.has_value(), b.colorProfile.has_value());
        if (a.colorProfile && b.colorProfile) {
            EXPECT_TRUE(*a.colorProfile == *b.colorProfile);
        }
        for (int f = 0; f < a.frameCount(); f++) EXPECT_EQ(renderFrame(a, f), renderFrame(b, f)) << "frame " << f;
    }

}

// AC5: documents built in code survive read(write(d)) field for field, in every colour mode
TEST(AsepriteCodecTest, WriteRoundTrip) {
    for (ColorMode mode : {ColorMode::Rgba, ColorMode::Grayscale, ColorMode::Indexed}) {
        SCOPED_TRACE(colorModeName(mode));
        const Document doc = makeDocument(mode);
        std::vector<uint8_t> bytes;
        CodecError error;
        ASSERT_TRUE(write(doc, bytes, &error)) << error.kind << ": " << error.message;
        ReadResult r = read(bytes);
        ASSERT_TRUE(r.ok()) << r.error.kind << ": " << r.error.message;
        EXPECT_TRUE(r.warnings.empty()) << r.warnings.front().message;
        expectSameDocument(doc, *r.document);
        // deterministic, and a written file re-saves byte for byte too
        std::vector<uint8_t> again;
        ASSERT_TRUE(write(doc, again, &error));
        EXPECT_EQ(again, bytes);
        std::vector<uint8_t> resaved;
        ASSERT_TRUE(write(*r.document, resaved, &error));
        EXPECT_EQ(resaved, bytes);
    }
    // every blend mode, on a layer and on a group
    for (int m = 0; m <= static_cast<int>(BlendMode::Divide); m++) {
        Document doc = makeDocument(ColorMode::Rgba);
        doc.layers[1].blend = static_cast<BlendMode>(m);
        doc.layers[2].blend = static_cast<BlendMode>(18 - m);
        std::vector<uint8_t> bytes;
        ASSERT_TRUE(write(doc, bytes, nullptr));
        ReadResult r = read(bytes);
        ASSERT_TRUE(r.ok());
        EXPECT_EQ(r.document->layers[1].blend, static_cast<BlendMode>(m));
        EXPECT_EQ(r.document->layers[2].blend, static_cast<BlendMode>(18 - m));
    }
}

// A document without a palette (an RGBA sprite made in code) is written with the 256 black
// entries any reader starts from, so the file it gives re-saves byte for byte
TEST(AsepriteCodecTest, PaletteLessDocumentResavesIdentically) {
    Document doc = makeDocument(ColorMode::Rgba);
    doc.palette.clear();
    std::vector<uint8_t> bytes;
    CodecError error;
    ASSERT_TRUE(write(doc, bytes, &error)) << error.message;
    ReadResult r = read(bytes);
    ASSERT_TRUE(r.ok()) << r.error.message;
    EXPECT_EQ(r.document->palette.size(), 256u);
    std::vector<uint8_t> resaved;
    ASSERT_TRUE(write(*r.document, resaved, &error)) << error.message;
    EXPECT_TRUE(resaved == bytes) << resaved.size() << " bytes, first written " << bytes.size();
}

// A new file follows Aseprite 1.3.5's layout: the 8-bit palette chunk when it can hold the palette
TEST(AsepriteCodecTest, NewFileLayout) {
    Document doc = makeDocument(ColorMode::Rgba);
    for (PaletteEntry& e : doc.palette) e.name.reset();
    std::vector<uint8_t> bytes;
    ASSERT_TRUE(write(doc, bytes, nullptr));
    ReadResult r = read(bytes);
    ASSERT_TRUE(r.ok());
    const auto types = chunkTypes(*r.document);
    ASSERT_FALSE(types.empty());
    const std::vector<uint16_t>& f0 = types[0];
    auto at = [&](uint16_t t) { return std::find(f0.begin(), f0.end(), t) - f0.begin(); };
    EXPECT_LT(at(kChunkExternalFiles), at(kChunkColorProfile));
    EXPECT_LT(at(kChunkColorProfile), at(kChunkOldPalette));
    EXPECT_EQ(at(kChunkPalette), static_cast<std::ptrdiff_t>(f0.size()));   // no 0x2019
    EXPECT_LT(at(kChunkOldPalette), at(kChunkTileset));
    EXPECT_LT(at(kChunkTileset), at(kChunkTags));
    EXPECT_LT(at(kChunkTags), at(kChunkLayer));
    EXPECT_LT(at(kChunkLayer), at(kChunkSlice));
    EXPECT_LT(at(kChunkSlice), at(kChunkCel));
    // entries with names or alpha need the new chunk
    doc.palette[1].name = "named";
    ASSERT_TRUE(write(doc, bytes, nullptr));
    const auto named = chunkTypes(*read(bytes).document);
    EXPECT_NE(std::find(named[0].begin(), named[0].end(), kChunkPalette), named[0].end());
    EXPECT_EQ(read(bytes).document->palette[1].name, std::optional<std::string>("named"));
}

// Unknown chunks and user data with no owner go back where they were (R5)
TEST(AsepriteCodecTest, KeepsUnknownChunksInPlace) {
    Document doc = makeDocument(ColorMode::Rgba);
    doc.opaque.clear();
    OpaqueChunk afterLayer;
    afterLayer.type = 0x2017;   // the unused path chunk
    afterLayer.frame = 0;
    afterLayer.anchor = {ChunkAnchor::Kind::Layer, 2};
    afterLayer.body = {9, 9};
    doc.opaque.push_back(afterLayer);
    std::vector<uint8_t> bytes;
    ASSERT_TRUE(write(doc, bytes, nullptr));
    ReadResult r = read(bytes);
    ASSERT_TRUE(r.ok());
    ASSERT_EQ(r.document->opaque.size(), 1u);
    EXPECT_EQ(r.document->opaque[0].type, 0x2017);
    EXPECT_TRUE(r.document->opaque[0].anchor == afterLayer.anchor);
    EXPECT_EQ(r.document->opaque[0].body, afterLayer.body);
}

TEST(AsepriteCodecTest, IndexedCelWithoutPaletteColourIsATypedError) {
    Document doc = makeDocument(ColorMode::Indexed);
    PixelImage img = *doc.celRecord(2, 0)->image;
    img.set(0, 0, {1, 2, 3, 255});   // drops the index plane, and is no palette colour
    doc.celRecord(2, 0)->image = std::make_shared<const PixelImage>(img);
    std::vector<uint8_t> bytes;
    CodecError error;
    EXPECT_FALSE(write(doc, bytes, &error));
    EXPECT_EQ(error.kind, "invalid_document");
    EXPECT_NE(error.message.find("#010203ff"), std::string::npos) << error.message;
    // a palette colour maps back to its index
    img.set(0, 0, doc.palette[4].color);
    doc.celRecord(2, 0)->image = std::make_shared<const PixelImage>(img);
    ASSERT_TRUE(write(doc, bytes, &error)) << error.message;
    EXPECT_EQ(read(bytes).document->celRecord(2, 0)->image->indexAt(0, 0), 4);
}

TEST(AsepriteCodecTest, TypedErrors) {
    std::vector<uint8_t> bytes = readBytes(asefile("basic-16x16.aseprite"));
    {
        std::vector<uint8_t> bad = bytes;
        bad[4] = 0;
        EXPECT_EQ(read(bad).error.kind, "not_aseprite");
    }
    {
        std::vector<uint8_t> cut(bytes.begin(), bytes.begin() + 100);
        EXPECT_EQ(read(cut).error.kind, "truncated");
        std::vector<uint8_t> half(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(bytes.size() / 2));
        const ReadResult r = read(half);
        EXPECT_FALSE(r.ok());
        EXPECT_TRUE(r.error.kind == "truncated" || r.error.kind == "corrupt_chunk") << r.error.kind;
        EXPECT_FALSE(r.error.fix.empty());
    }
    {
        std::vector<uint8_t> bad = bytes;
        bad[12] = 24;   // colour depth
        EXPECT_EQ(read(bad).error.kind, "unsupported_depth");
    }
    {
        std::vector<uint8_t> bad = bytes;
        bad[8] = 0xff;
        bad[9] = 0x7f;   // 32767 wide
        EXPECT_EQ(read(bad).error.kind, "too_large");
    }
    EXPECT_EQ(readFile(asepriteFixtures() / "missing.aseprite").error.kind, "import_failed");
}

// A scan without pixels gives the structure, as the asset database reads it
TEST(AsepriteCodecTest, StructureOnlyRead) {
    ReadOptions quick;
    quick.pixels = false;
    const Document doc = loadDocument(asefile("layers_and_tags.aseprite"), quick);
    EXPECT_EQ(doc.tags.size(), 3u);
    EXPECT_EQ(doc.layers.size(), 6u);
    EXPECT_TRUE(doc.cels.empty());
    EXPECT_FALSE(doc.provenance);
}

// R9: one JSON shape; the summary has no cel table, cels detail has it
TEST(AsepriteCodecTest, Describe) {
    const Document doc = loadDocument(asefile("layers_and_tags.aseprite"));
    const nlohmann::json summary = describe(doc, Detail::Summary);
    EXPECT_EQ(summary["width"], 16);
    EXPECT_EQ(summary["frame_count"], 4);
    EXPECT_EQ(summary["layers"].size(), 6u);
    EXPECT_EQ(summary["layers"][4]["path"], "Group 1/Layer 5");
    EXPECT_EQ(summary["tags"].size(), 3u);
    EXPECT_FALSE(summary.contains("cels"));
    EXPECT_FALSE(summary["palette"].contains("colors"));
    const nlohmann::json cels = describe(doc, Detail::Cels);
    ASSERT_TRUE(cels.contains("cels"));
    EXPECT_EQ(cels["cels"].size(), doc.cels.size());
    EXPECT_EQ(cels["palette"]["colors"].size(), doc.palette.size());
}
