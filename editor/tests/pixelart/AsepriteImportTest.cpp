// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The import of .aseprite files into sheets, clips and sub-assets (docs/specs/S44-aseprite-import.md
// AC8-AC11, §6.5-§6.7) on files Aseprite wrote (editor/tests/fixtures/aseprite/README.md).

#include "AsepriteTestSupport.h"
#include "pixelart/aseprite/AsepriteImport.h"

#include <gtest/gtest.h>

#include <algorithm>
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

    std::set<std::string> keysOf(const AsepriteImport& import) {
        std::set<std::string> keys;
        for (const SubAssetOutput& o : import.subAssets) keys.insert(o.key);
        return keys;
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

    AsepriteImportSettings only;
    only.include = {"Layer 1"};
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
    EXPECT_EQ(withHidden.size(), byDefault.size() + 1);
    EXPECT_EQ(std::count(byDefault.begin(), byDefault.end(), doc.findLayer("invisible")), 0);
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

// AC10: one clip per tag with the tag's playback, loop when repeat is 0 and once otherwise; a
// file without tags gets one clip named after the file
TEST(AsepriteImportTest, Clips) {
    for (const char* name : {"tags3.aseprite", "tags3x123reps.aseprite"}) {
        const AsepriteImport import = importOf(importFixture("aseprite-tests", name));
        ASSERT_TRUE(import.ok()) << import.error.message;
        ASSERT_EQ(import.clips.size(), import.sheet.tags.size()) << name;
        for (const ClipOutput& clip : import.clips) {
            ASSERT_GE(clip.tag, 0);
            const SpriteTag& tag = import.sheet.tags[static_cast<size_t>(clip.tag)];
            EXPECT_EQ(clip.name, tag.name);
            EXPECT_EQ(clip.key, "animation:" + tag.name);
            EXPECT_EQ(clip.wrap, tag.repeat == 0 ? "loop" : "once") << name << " " << tag.name;
            const std::vector<int> frames = tag.repeat == 0 ? tag.sequence() : tag.playback();
            EXPECT_EQ(clip.keys, frames.size()) << name << " " << tag.name;
            double ms = 0;
            for (int f : frames) ms += import.sheet.frames[static_cast<size_t>(f)].durationMs;
            EXPECT_NEAR(clip.length, ms / 1000.0, 1e-4) << name << " " << tag.name;
            EXPECT_NE(clip.text.find("frame"), std::string::npos) << "a sprite.frame track";
        }
    }
    const AsepriteImport untagged = importOf(importFixture("asefile", "basic-16x16.aseprite"), {}, "sprites/basic.aseprite");
    ASSERT_TRUE(untagged.ok());
    ASSERT_EQ(untagged.clips.size(), 1u);
    EXPECT_EQ(untagged.clips[0].key, "animation#0");
    EXPECT_EQ(untagged.clips[0].name, "basic");
    AsepriteImportSettings none;
    none.generateClips = false;
    EXPECT_TRUE(importOf(importFixture("aseprite-tests", "tags3.aseprite"), none).clips.empty());
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

// AC8 (the outputs' part): sub-asset keys never contain the file's name, so a renamed file keeps
// every derived GUID; an added frame keeps the keys of the frames there were; a renamed tag
// changes only that clip's key (the asset database's matching keeps its GUID)
TEST(AsepriteImportTest, GuidsAreStable) {
    const fs::path file = importFixture("asefile", "layers_and_tags.aseprite");
    const std::set<std::string> keys = keysOf(importOf(file, {}, "sprites/hero.aseprite"));
    EXPECT_EQ(keysOf(importOf(file, {}, "characters/renamed.aseprite")), keys);
    for (const std::string& k : keys) EXPECT_EQ(k.find("hero"), std::string::npos) << k;
    EXPECT_TRUE(keys.count("animation:T1") && keys.count("frame#0") && keys.count("frame#3"));

    Document added = loadDocument(file);
    added.frameDurations.push_back(added.frameDurations.back());
    const int last = added.frameCount() - 1;
    for (const Cel& c : std::vector<Cel>(added.cels)) {
        if (c.frame != last - 1) continue;
        Cel copy = c;
        copy.frame = last;
        copy.linkedFrame = -1;
        added.cels.push_back(copy);
    }
    added.sortCels();
    AsepriteImportContext ctx;
    ctx.sourcePath = "sprites/hero.aseprite";
    std::set<std::string> grown;
    for (const SubAssetOutput& o : AsepriteImport::run(added, {}, ctx).subAssets) grown.insert(o.key);
    for (const std::string& k : keys) EXPECT_TRUE(grown.count(k)) << "kept: " << k;
    EXPECT_TRUE(grown.count("frame#4"));

    Document renamed = loadDocument(file);
    renamed.tags[0].name = "Walk";
    std::set<std::string> after;
    for (const SubAssetOutput& o : AsepriteImport::run(renamed, {}, ctx).subAssets) after.insert(o.key);
    EXPECT_FALSE(after.count("animation:T1"));
    EXPECT_TRUE(after.count("animation:Walk"));
    std::set<std::string> rest = keys;
    rest.erase("animation:T1");
    for (const std::string& k : rest) EXPECT_TRUE(after.count(k)) << "kept: " << k;
}
