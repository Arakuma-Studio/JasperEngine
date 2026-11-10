// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The import of .aseprite files into sheets, clips and sub-assets (docs/specs/S44-aseprite-import.md
// AC8-AC11, §6.5-§6.7) on files Aseprite wrote (editor/tests/fixtures/aseprite/README.md).

#include "AsepriteTestSupport.h"
#include "pixelart/aseprite/AsepriteImport.h"
#include "pixelart/aseprite/AsepriteImporter.h"
#include "util/AssetDatabase.h"

#include "animation/AnimationClipIO.h"
#include "sprite/SpriteSheetIO.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iterator>
#include <map>
#include <set>

using namespace jasperengine;
using namespace jasperengine::editor::pixelart;
using namespace jasperengine::editor::pixelart::aseprite;
using namespace jasperengine::editor::pixelart::testing;
namespace fs = std::filesystem;

namespace {

    fs::path importFixture(const std::string& folder, const std::string& name) { return asepriteFixtures() / folder / name; }

    AsepriteImport importOf(const fs::path& file, const AsepriteImportSettings& settings = {}, const std::string& source = "sprites/hero.aseprite") {
        AsepriteImportContext ctx;
        ctx.sourcePath = source;
        return AsepriteImport::run(loadDocument(file), settings, ctx);
    }

    // a sheet frame put back at its trim offset on a canvas of its source size
    PixelImage untrimmedFrame(const AsepriteImport& import, int index) {
        const SpriteFrame& f = import.sheet.frames[static_cast<size_t>(index)];
        PixelImage out(static_cast<int>(f.sourceSize.x), static_cast<int>(f.sourceSize.y));
        const PixelImage& page = import.pages[f.page];
        for (int y = 0; y < static_cast<int>(f.rect.getHeight()); y++) {
            for (int x = 0; x < static_cast<int>(f.rect.getWidth()); x++) {
                out.set(static_cast<int>(f.offset.x) + x, static_cast<int>(f.offset.y) + y,
                        page.at(static_cast<int>(f.rect.getX()) + x, static_cast<int>(f.rect.getY()) + y));
            }
        }
        return out;
    }


}

// AC9: trimmed, packed frames put back at their offsets are Aseprite's own exports of the frames;
// a layer selection is the export of those layers alone
TEST(AsepriteImportTest, Layers) {
    const fs::path file = importFixture("asefile", "layers_and_tags.aseprite");
    const AsepriteImport all = importOf(file);
    ASSERT_TRUE(all.ok()) << all.error.message;
    ASSERT_EQ(all.sheet.frames.size(), 4u);
    const char* exports[] = {"layers_and_tags_01.png", "layers_and_tags_02.png", "layers_and_tags_03.png", "layers_and_tags_04.png"};
    for (int i = 0; i < 4; i++) {
        EXPECT_TRUE(samePicture(untrimmedFrame(all, i), loadPng(importFixture("asefile", exports[i])))) << "frame " << i;
    }
    EXPECT_EQ(all.sheet.frames[0].name, "hero 0");
    EXPECT_EQ(all.sheet.frames[0].key, "frame#0");

    // §6.7: include adds and exclude removes, so "Layer 1 alone" excludes the others
    const Document structure = loadDocument(file);
    AsepriteImportSettings only;
    for (int l = 0; l < static_cast<int>(structure.layers.size()); l++) {
        if (structure.layers[static_cast<size_t>(l)].name != "Layer 1" && !structure.layers[static_cast<size_t>(l)].isGroup()) {
            only.exclude.push_back(structure.layerPath(l));
        }
    }
    const AsepriteImport one = importOf(file, only);
    ASSERT_TRUE(one.ok()) << one.error.message;
    EXPECT_TRUE(samePicture(untrimmedFrame(one, 2), loadPng(importFixture("asefile", "single_layer.png"))));

    // the hidden layer stays out unless asked for, a prefix leaves layers out, an unknown name warns
    std::vector<PixelProblem> warnings;
    const Document doc = loadDocument(file);
    const std::vector<int> byDefault = selectLayers(doc, AsepriteImportSettings(), warnings);
    AsepriteImportSettings hidden;
    hidden.includeHidden = true;
    const std::vector<int> withHidden = selectLayers(doc, hidden, warnings);
    EXPECT_GT(withHidden.size(), byDefault.size());
    EXPECT_EQ(std::count(byDefault.begin(), byDefault.end(), doc.findLayer("invisible")), 0);
    EXPECT_EQ(std::count(withHidden.begin(), withHidden.end(), doc.findLayer("invisible")), 1);
    AsepriteImportSettings prefixed;
    prefixed.ignorePrefixes = {"Layer"};
    EXPECT_LT(selectLayers(doc, prefixed, warnings).size(), byDefault.size());
    warnings.clear();
    AsepriteImportSettings unknown;
    unknown.include = {"Layer 1", "Nope"};
    selectLayers(doc, unknown, warnings);
    ASSERT_FALSE(warnings.empty());
    EXPECT_NE(warnings[0].message.find("Nope"), std::string::npos);
    EXPECT_NE(warnings[0].fix.find("Layer 1"), std::string::npos) << "the fix names the layers there are";

    // layers_as_sprites: a set of frames per top-level layer, each at the canvas origin
    AsepriteImportSettings split;
    split.importMode = ImportMode::LayersAsSprites;
    const AsepriteImport layered = importOf(file, split);
    ASSERT_TRUE(layered.ok()) << layered.error.message;
    EXPECT_EQ(layered.sheet.frames.size() % 4, 0u);
    EXPECT_GT(layered.sheet.frames.size(), 4u);
    EXPECT_NE(layered.sheet.frames[0].name.find('/'), std::string::npos) << layered.sheet.frames[0].name;
    EXPECT_EQ(layered.sheet.frames[0].key.rfind("frame:", 0), 0u) << layered.sheet.frames[0].key;
    for (const SpriteFrame& f : layered.sheet.frames) EXPECT_EQ(f.sourceSize, all.sheet.frames[0].sourceSize);
}

// AC10 (§6.6, exact): the key lists, times, lengths and wraps of every direction and repeat,
// worked out from the spec's rules (not from the importer's own sequence()), and the events layer
TEST(AsepriteImportTest, Clips) {
    // four frames of 100, 50, 150 and 100 ms; every tag spans frames 1..3
    Document doc;
    doc.width = 2;
    doc.height = 2;
    doc.mode = ColorMode::Rgba;
    doc.frameDurations = {100, 50, 150, 100};
    Layer body;
    body.name = "Body";
    Layer events;
    events.name = "events";
    doc.layers = {body, events};
    doc.rebuildTreeFromLevels();
    for (int f = 0; f < 4; f++) {
        PixelImage image(2, 2);
        image.fill({static_cast<uint8_t>(40 * f + 20), 0, 0, 255});
        Cel cel;
        cel.layer = 0;
        cel.frame = f;
        cel.image = std::make_shared<const PixelImage>(image);
        doc.cels.push_back(cel);
    }
    Cel marker;   // the events layer's cel on frame 2 says what the clip calls there
    marker.layer = 1;
    marker.frame = 2;
    marker.image = std::make_shared<const PixelImage>(PixelImage(1, 1));
    marker.userData.text = "onStep(\"left\", 1)";
    doc.cels.push_back(marker);
    struct Expected {
        const char* name;
        TagDirection direction;
        uint16_t repeat;
        std::vector<int> frames;
        std::vector<double> times;
        double length;
        const char* wrap;
    };
    const std::vector<Expected> table = {
        {"fwd0", TagDirection::Forward, 0, {1, 2, 3}, {0.0, 0.05, 0.2}, 0.3, "loop"},
        {"rev0", TagDirection::Reverse, 0, {3, 2, 1}, {0.0, 0.1, 0.25}, 0.3, "loop"},
        {"pp0", TagDirection::PingPong, 0, {1, 2, 3, 2}, {0.0, 0.05, 0.2, 0.3}, 0.45, "loop"},
        {"ppr0", TagDirection::PingPongReverse, 0, {3, 2, 1, 2}, {0.0, 0.1, 0.25, 0.3}, 0.45, "loop"},
        {"fwd3", TagDirection::Forward, 3, {1, 2, 3, 1, 2, 3, 1, 2, 3}, {0.0, 0.05, 0.2, 0.3, 0.35, 0.5, 0.6, 0.65, 0.8}, 0.9, "once"},
        {"rev1", TagDirection::Reverse, 1, {3, 2, 1}, {0.0, 0.1, 0.25}, 0.3, "once"},
        {"pp1", TagDirection::PingPong, 1, {1, 2, 3}, {0.0, 0.05, 0.2}, 0.3, "once"},
        {"pp3", TagDirection::PingPong, 3, {1, 2, 3, 2, 1, 2, 3}, {0.0, 0.05, 0.2, 0.3, 0.45, 0.5, 0.65}, 0.75, "once"},
        {"ppr2", TagDirection::PingPongReverse, 2, {3, 2, 1, 2, 3}, {0.0, 0.1, 0.25, 0.3, 0.45}, 0.55, "once"},
    };
    for (const Expected& e : table) {
        Tag tag;
        tag.name = e.name;
        tag.from = 1;
        tag.to = 3;
        tag.direction = e.direction;
        tag.repeat = e.repeat;
        doc.tags.push_back(tag);
    }
    doc.sortCels();
    AsepriteImportSettings settings;
    settings.trim = false;
    const AsepriteImport import = AsepriteImport::run(doc, settings, AsepriteImportContext{"sprites/table.aseprite", Guid(), 1});
    ASSERT_TRUE(import.ok()) << import.error.message;
    ASSERT_EQ(import.clips.size(), table.size());
    for (size_t i = 0; i < table.size(); i++) {
        const Expected& e = table[i];
        const ClipOutput& clip = import.clips[i];
        SCOPED_TRACE(e.name);
        EXPECT_EQ(clip.name, e.name);
        EXPECT_EQ(clip.key, std::string("animation:") + e.name);
        EXPECT_EQ(clip.wrap, e.wrap);
        const anim::ClipParseResult parsed = anim::parseClip(clip.text);
        ASSERT_TRUE(parsed.ok());
        const ClipData& data = *parsed.data;
        EXPECT_NEAR(data.length, e.length, 1e-6);
        EXPECT_FLOAT_EQ(data.frameRate, 20.0f);   // gcd(50, 100, 150) = 50 ms
        EXPECT_EQ(data.source.kind, "aseprite");
        EXPECT_EQ(data.source.path, "sprites/table.aseprite");
        EXPECT_EQ(data.source.name, e.name);
        const ClipTrackData* frameTrack = nullptr;
        for (const ClipTrackData& t : data.tracks) {
            if (t.component == "sprite" && t.property == "frame") frameTrack = &t;
        }
        ASSERT_NE(frameTrack, nullptr);
        EXPECT_TRUE(frameTrack->path.empty());
        EXPECT_TRUE(frameTrack->discrete);   // step keys: a frame index never interpolates
        ASSERT_EQ(frameTrack->keys.size(), e.frames.size());
        for (size_t k = 0; k < e.frames.size(); k++) {
            EXPECT_EQ(static_cast<int>(frameTrack->keys[k].value.at(0)), e.frames[k]) << "key " << k;
            EXPECT_NEAR(frameTrack->keys[k].time, e.times[k], 1e-6) << "key " << k;
        }
        // the events layer's frame 2 text, at frame 2's start in this tag's playback (once per visit)
        size_t visits = 0;
        for (size_t k = 0; k < e.frames.size(); k++) visits += e.frames[k] == 2 ? 1 : 0;
        ASSERT_EQ(data.events.size(), visits);
        EXPECT_EQ(data.events[0].function, "onStep");
        ASSERT_EQ(data.events[0].args.size(), 2u);
    }

    // a file without tags: one clip of all frames named after the file; generate off: none
    const AsepriteImport untagged = importOf(importFixture("asefile", "basic-16x16.aseprite"), {}, "sprites/basic.aseprite");
    ASSERT_TRUE(untagged.ok());
    ASSERT_EQ(untagged.clips.size(), 1u);
    EXPECT_EQ(untagged.clips[0].key, "animation#0");
    EXPECT_EQ(untagged.clips[0].name, "basic");
    AsepriteImportSettings none;
    none.generateClips = false;
    EXPECT_TRUE(importOf(importFixture("aseprite-tests", "tags3.aseprite"), none).clips.empty());
}

// A fully transparent sprite keeps its frames and tags: the sheet has a 1x1 page for its empty
// frames to point at, and survives its own record's round trip
TEST(AsepriteImportTest, TransparentSpriteKeepsItsFrames) {
    Document doc;
    doc.width = 8;
    doc.height = 8;
    doc.mode = ColorMode::Rgba;
    doc.frameDurations = {100, 100, 100};
    Layer layer;
    layer.name = "Empty";
    doc.layers = {layer};
    doc.rebuildTreeFromLevels();
    Tag tag;
    tag.name = "idle";
    tag.from = 0;
    tag.to = 2;
    doc.tags = {tag};
    const AsepriteImport import = AsepriteImport::run(doc, AsepriteImportSettings(), AsepriteImportContext{"sprites/ghost.aseprite", Guid(), 1});
    ASSERT_TRUE(import.ok()) << import.error.message;
    ASSERT_EQ(import.pages.size(), 1u);
    EXPECT_EQ(import.pages[0].width(), 1);
    EXPECT_EQ(import.sheet.frames.size(), 3u);
    const SpriteSheetParseResult parsed = parseSheet(writeSheet(import.sheet));
    ASSERT_TRUE(parsed.ok());
    EXPECT_EQ(parsed.sheet->frames.size(), 3u);
    ASSERT_EQ(parsed.sheet->tags.size(), 1u);
    EXPECT_EQ(parsed.sheet->tags[0].to, 2);
    EXPECT_EQ(import.clips.size(), 1u);
}

// AC11: the pivot from a slice's pivot, the border from the 9-patch slice (that frame untrimmed),
// per-frame keys kept in the sheet
TEST(AsepriteImportTest, Slices) {
    const fs::path file = importFixture("asefile", "slice_advanced.aseprite");
    const Document doc = loadDocument(file);
    const AsepriteImport import = importOf(file);
    ASSERT_TRUE(import.ok()) << import.error.message;
    ASSERT_EQ(import.sheet.slices.size(), doc.slices.size());
    EXPECT_EQ(import.sheet.slices[0].keys.size(), doc.slices[0].keys.size());
    // "Slice 1" has a pivot on every frame: the frames' pivots are it
    for (size_t i = 0; i < import.sheet.frames.size() && i < doc.slices[0].keys.size(); i++) {
        const SliceKey& key = doc.slices[0].keys[i];
        ASSERT_TRUE(key.pivot);
        EXPECT_EQ(import.sheet.frames[i].pivot, Vector2(static_cast<float>(key.bounds.x + key.pivot->x),
                                                        static_cast<float>(key.bounds.y + key.pivot->y))) << "frame " << i;
    }
    // the 9-patch slice is the border, in frame pixels; such frames are not trimmed
    const SliceKey& nine = doc.slices[1].keys[0];
    ASSERT_TRUE(nine.center);
    const SpriteFrame& f = import.sheet.frames[0];
    EXPECT_EQ(f.border, Vector4(static_cast<float>(nine.bounds.x + nine.center->x), static_cast<float>(nine.bounds.y + nine.center->y),
                                f.sourceSize.x - static_cast<float>(nine.bounds.x + nine.center->x + nine.center->w),
                                f.sourceSize.y - static_cast<float>(nine.bounds.y + nine.center->y + nine.center->h)));
    EXPECT_EQ(Vector2(f.rect.getWidth(), f.rect.getHeight()), f.sourceSize);
    // keys that move per frame (Aseprite's slices-moving) survive as keys
    const Document moving = loadDocument(importFixture("aseprite-tests", "slices-moving.aseprite"));
    const AsepriteImport moved = importOf(importFixture("aseprite-tests", "slices-moving.aseprite"));
    ASSERT_TRUE(moved.ok());
    ASSERT_FALSE(moved.sheet.slices.empty());
    EXPECT_EQ(moved.sheet.slices[0].keys.size(), moving.slices[0].keys.size());
}

// AC8: the asset database's sub-asset GUIDs of a file survive a rename of the file (with its meta),
// an added frame and a renamed tag (the clip keeps its GUID, its key and name follow the tag), and
// the import hands out exactly those GUIDs (derived ones for index keys, the database's for named)
TEST(AsepriteImportTest, GuidsAreStable) {
    const fs::path root = fs::temp_directory_path() / ("je-aseprite-guids-" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()) +
                                                       "-" + std::to_string(reinterpret_cast<uintptr_t>(&root) & 0xffff));
    fs::remove_all(root);
    fs::create_directories(root / "assets/sprites");
    const fs::path source = importFixture("asefile", "layers_and_tags.aseprite");
    fs::copy_file(source, root / "assets/sprites/hero.aseprite");
    jasperengine::editor::AssetDatabase database;
    database.open(root, "assets");
    auto guidsOf = [&](const std::string& path) {
        std::map<std::string, Guid> byKey;
        jasperengine::editor::AssetDatabase::AssetInfo info;
        if (database.getInfo(fs::path(path), info)) {
            for (const jasperengine::editor::AssetDatabase::SubAsset& sub : info.subAssets) byKey[sub.key] = sub.guid;
            byKey["#file"] = info.guid;
        }
        return byKey;
    };
    auto later = [](const fs::path& file) { fs::last_write_time(file, fs::last_write_time(file) + std::chrono::seconds(2)); };
    const std::map<std::string, Guid> first = guidsOf("assets/sprites/hero.aseprite");
    ASSERT_TRUE(first.count("animation:T1") && first.count("animation:T2") && first.count("animation:T3")) << first.size();
    EXPECT_FALSE(first.at("animation:T1").isNull());

    // the import's outputs: named keys take the database's GUIDs, index keys Guid::derive's
    auto importGuids = [&](const std::string& assetPath, const std::map<std::string, Guid>& known) {
        ImportRequest request;
        request.file = root / "assets" / assetPath;
        request.assetPath = assetPath;
        request.sourceGuid = known.at("#file");
        for (const auto& [key, guid] : known) {
            if (key != "#file") request.namedGuids[key] = guid;
        }
        request.cacheRoot = root / ".jasperengine/cache/imported/v1";
        std::shared_ptr<ImportRecord> record = AsepriteImporter::runImport(request);
        std::map<std::string, Guid> out;
        for (const ImportRecord::Output& o : record->outputs) out[o.key] = o.guid;
        return out;
    };
    const std::map<std::string, Guid> imported = importGuids("sprites/hero.aseprite", first);
    EXPECT_EQ(imported.at("animation:T1"), first.at("animation:T1"));
    EXPECT_EQ(imported.at("frame#0"), Guid::derive(first.at("#file"), "frame#0"));

    // a rename with its meta: the same file GUID, the same sub-asset GUIDs, the same import GUIDs
    fs::create_directories(root / "assets/characters");
    fs::rename(root / "assets/sprites/hero.aseprite", root / "assets/characters/knight.aseprite");
    fs::rename(root / "assets/sprites/hero.aseprite.meta", root / "assets/characters/knight.aseprite.meta");
    database.refresh();
    const std::map<std::string, Guid> renamed = guidsOf("assets/characters/knight.aseprite");
    EXPECT_EQ(renamed, first);
    const std::map<std::string, Guid> reimported = importGuids("characters/knight.aseprite", renamed);
    EXPECT_EQ(reimported, imported);

    // an added frame keeps every GUID there was
    Document grown = loadDocument(root / "assets/characters/knight.aseprite");
    grown.frameDurations.push_back(grown.frameDurations.back());
    ASSERT_TRUE(writeFile(grown, root / "assets/characters/knight.aseprite", nullptr));
    later(root / "assets/characters/knight.aseprite");
    database.refresh();
    const std::map<std::string, Guid> withFrame = guidsOf("assets/characters/knight.aseprite");
    const std::map<std::string, Guid> grownImport = importGuids("characters/knight.aseprite", withFrame);
    for (const auto& [key, guid] : imported) EXPECT_EQ(grownImport.at(key), guid) << key;
    EXPECT_TRUE(grownImport.count("frame#4"));

    // a renamed tag: the clip keeps its GUID under the new key (the reference follows the name)
    Document retagged = loadDocument(root / "assets/characters/knight.aseprite");
    retagged.tags[0].name = "Walk";
    ASSERT_TRUE(writeFile(retagged, root / "assets/characters/knight.aseprite", nullptr));
    later(root / "assets/characters/knight.aseprite");
    database.refresh();
    const std::map<std::string, Guid> afterRename = guidsOf("assets/characters/knight.aseprite");
    EXPECT_FALSE(afterRename.count("animation:T1"));
    ASSERT_TRUE(afterRename.count("animation:Walk"));
    EXPECT_EQ(afterRename.at("animation:Walk"), first.at("animation:T1"));
    EXPECT_EQ(afterRename.at("animation:T2"), first.at("animation:T2"));
    EXPECT_EQ(importGuids("characters/knight.aseprite", afterRename).at("animation:Walk"), first.at("animation:T1"));
    fs::remove_all(root);
}

// The import cache is keyed by the file's path and GUID too: a copy of a file under another name
// imports its own frames and clips (not the other file's names), and a renamed file gets a new import
TEST(AsepriteImportTest, CacheKnowsTheFile) {
    const fs::path root = fs::temp_directory_path() / ("je-aseprite-cache-" + std::to_string(reinterpret_cast<uintptr_t>(&root) & 0xffff));
    fs::remove_all(root);
    fs::create_directories(root / "assets");
    fs::copy_file(importFixture("asefile", "basic-16x16.aseprite"), root / "assets/hero.aseprite");
    fs::copy_file(importFixture("asefile", "basic-16x16.aseprite"), root / "assets/slime.aseprite");
    auto run = [&](const std::string& assetPath, const Guid& guid) {
        ImportRequest request;
        request.file = root / "assets" / assetPath;
        request.assetPath = assetPath;
        request.sourceGuid = guid;
        request.cacheRoot = root / "cache";
        return AsepriteImporter::runImport(request);
    };
    const Guid heroGuid = Guid::derive(Guid(), "hero"), slimeGuid = Guid::derive(Guid(), "slime");
    std::shared_ptr<ImportRecord> hero = run("hero.aseprite", heroGuid);
    std::shared_ptr<ImportRecord> slime = run("slime.aseprite", slimeGuid);
    ASSERT_TRUE(hero->ok && slime->ok);
    EXPECT_NE(hero->key, slime->key);
    EXPECT_FALSE(slime->cacheHit);
    auto sheetOf = [](const ImportRecord& r) {
        std::ifstream in(r.sheetFile(), std::ios::binary);
        return parseSheet(std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()));
    };
    const SpriteSheetParseResult slimeSheet = sheetOf(*slime);
    ASSERT_TRUE(slimeSheet.ok());
    EXPECT_EQ(slimeSheet.sheet->source, "slime.aseprite");
    EXPECT_EQ(slimeSheet.sheet->frames[0].name, "slime 0");
    ASSERT_EQ(slime->clips.size(), 1u);
    EXPECT_EQ(slime->clips[0].name, "slime");
    // the same file again: the cache
    EXPECT_TRUE(run("hero.aseprite", heroGuid)->cacheHit);
    // renamed (the GUID kept): a new import with the new name
    fs::rename(root / "assets/hero.aseprite", root / "assets/knight.aseprite");
    std::shared_ptr<ImportRecord> knight = run("knight.aseprite", heroGuid);
    EXPECT_FALSE(knight->cacheHit);
    EXPECT_EQ(sheetOf(*knight).sheet->frames[0].name, "knight 0");
    fs::remove_all(root);
}

// AC20 (the editor's side of ImportedAssetsTest): the provider and an export's index.json come from
// one table of the import, so every key an export ships resolves in the editor to the very file
// the export copies, and the sheet record names its pages by their exported paths
// S50 R12: an aseprite-source tileset draws the strip the import writes and the provider answers
TEST(AsepriteImportTest, TilesetStripsAreCachedAndResolved) {
    const fs::path root = fs::temp_directory_path() / ("je-aseprite-tileset-" + std::to_string(reinterpret_cast<uintptr_t>(&root) & 0xffff));
    fs::remove_all(root);
    fs::create_directories(root / "assets");
    fs::copy_file(importFixture("asefile", "tileset.aseprite"), root / "assets/tiles.aseprite");
    const Document document = loadDocument(root / "assets/tiles.aseprite");
    ImportRequest request;
    request.file = root / "assets/tiles.aseprite";
    request.assetPath = "tiles.aseprite";
    request.cacheRoot = root / "cache";
    std::shared_ptr<ImportRecord> record = AsepriteImporter::runImport(request);
    ASSERT_TRUE(record->ok) << record->error.message;
    ASSERT_EQ(record->tilesetFiles.size(), document.tilesets.size());
    const std::string base = "je-imported/" + record->key.substr(0, 2) + "/" + record->key + "/";
    const std::vector<ImportRecord::ExportedFile> files = record->exportedFiles();
    for (size_t t = 0; t < document.tilesets.size(); t++) {
        SCOPED_TRACE(t);
        fs::path resolved;
        ASSERT_TRUE(record->resolveKey("tileset#" + std::to_string(t), resolved));
        EXPECT_TRUE(samePicture(loadPng(resolved), document.tilesets[t].tiles));
        const auto exported = std::find_if(files.begin(), files.end(), [&](const ImportRecord::ExportedFile& f) { return f.key == "tileset#" + std::to_string(t); });
        ASSERT_NE(exported, files.end());
        EXPECT_EQ(exported->cacheFile, resolved);
        EXPECT_EQ(exported->exportPath, base + resolved.filename().string());
    }
    fs::path none;
    EXPECT_FALSE(record->resolveKey("tileset#99999999999999999999999", none));
    // a cache hit knows the strips too: import.json lists them
    std::shared_ptr<ImportRecord> cached = AsepriteImporter::runImport(request);
    EXPECT_TRUE(cached->cacheHit);
    EXPECT_EQ(cached->tilesetFiles, record->tilesetFiles);
    fs::remove_all(root);
}

TEST(AsepriteImportTest, ProviderAndExportAgree) {
    const fs::path root = fs::temp_directory_path() / ("je-aseprite-export-" + std::to_string(reinterpret_cast<uintptr_t>(&root) & 0xffff));
    fs::remove_all(root);
    fs::create_directories(root / "assets");
    fs::copy_file(importFixture("asefile", "layers_and_tags.aseprite"), root / "assets/hero.aseprite");
    ImportRequest request;
    request.file = root / "assets/hero.aseprite";
    request.assetPath = "hero.aseprite";
    request.cacheRoot = root / "cache";
    std::shared_ptr<ImportRecord> record = AsepriteImporter::runImport(request);
    ASSERT_TRUE(record->ok) << record->error.message;
    const std::vector<ImportRecord::ExportedFile> files = record->exportedFiles();
    ASSERT_EQ(files.size(), 1 + record->pages.size() + record->clips.size());
    const std::string base = "je-imported/" + record->key.substr(0, 2) + "/" + record->key + "/";
    for (const ImportRecord::ExportedFile& f : files) {
        SCOPED_TRACE(f.key);
        fs::path resolved;
        ASSERT_TRUE(record->resolveKey(f.key, resolved));
        EXPECT_EQ(resolved, f.cacheFile);
        EXPECT_TRUE(fs::is_regular_file(f.cacheFile));
        EXPECT_EQ(f.exportPath, base + f.cacheFile.filename().string());
    }
    for (const ImportRecord::Clip& clip : record->clips) {
        fs::path byName, byKey;
        ASSERT_TRUE(record->resolveKey("animation:" + clip.name, byName) && record->resolveKey(clip.key, byKey));
        EXPECT_EQ(byName, byKey);
    }
    fs::path none;
    EXPECT_FALSE(record->resolveKey("image#99999999999999999999999", none));
    EXPECT_FALSE(record->resolveKey("animation:nope", none));
    std::ifstream in(record->sheetFile(), std::ios::binary);
    const SpriteSheetParseResult sheet = parseSheet(std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()));
    ASSERT_TRUE(sheet.ok());
    for (size_t p = 0; p < sheet.sheet->pages.size(); p++) EXPECT_EQ(sheet.sheet->pages[p].texture, base + "page" + std::to_string(p) + ".png");
    fs::remove_all(root);
}
