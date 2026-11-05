// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The .soundevent as an asset (docs/specs/S37-sound-events.md R2, R3; AC1): a sidecar GUID in its
// .meta given when the file appears and kept by a rename; one dependency edge per clip.

#include <gtest/gtest.h>

#include "asset/AssetIndex.h"
#include "asset/AssetType.h"
#include "asset/AssetTypes.h"
#include "audio/SoundEventIO.h"
#include "audio/SoundTypes.h"
#include "util/AssetDatabase.h"

#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace fs = std::filesystem;

namespace {

    class SoundEventTestDir {
    public:
        SoundEventTestDir() {
            std::random_device device;
            path = fs::temp_directory_path() / ("je-soundevent-test-" + std::to_string(device()) + std::to_string(device()));
            fs::create_directories(path / "assets/sounds");
        }
        ~SoundEventTestDir() {
            std::error_code ec;
            fs::remove_all(path, ec);
        }
        fs::path path;
    };

    void writeFile(const fs::path& path, const std::string& text) {
        fs::create_directories(path.parent_path());
        std::ofstream(path, std::ios::binary) << text;
    }

    SoundEventDef eventOf(const std::vector<AssetRef>& refs) {
        SoundEventDef def;
        def.mode = SoundEventMode::Shuffle;
        for (const AssetRef& ref : refs) {
            SoundEventClip clip;
            clip.clip = ref;
            def.clips.push_back(clip);
        }
        return def;
    }

}

TEST(SoundEventAsset, TypeIsSidecarInTheAssets) {
    const AssetTypeInfo* info = AssetTypes::find("assets/steps.soundevent", "assets");
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(info->type, AssetType::SoundEvent);
    EXPECT_EQ(info->identity, AssetIdentity::Sidecar);
    EXPECT_EQ(info->base, AssetPathBase::Assets);
    EXPECT_STREQ(assetTypeName(AssetType::SoundEvent), "sound_event");
    EXPECT_EQ(assetTypeFromName("sound_event"), AssetType::SoundEvent);
    EXPECT_TRUE(AssetIndex::isReferrerType(AssetType::SoundEvent, "assets/steps.soundevent"));
}

TEST(SoundEventAsset, GetsAMetaGuidAndARenameKeepsIt) {
    SoundEventTestDir dir;
    writeFile(dir.path / "assets/steps.soundevent", SoundEventIO::write(eventOf({AssetRef("sounds/a.wav")})));
    AssetDatabase database;
    database.open(dir.path, "assets");
    database.waitForBackgroundWork();
    const Guid guid = database.getAssetGuid("assets/steps.soundevent");
    ASSERT_FALSE(guid.isNull());
    EXPECT_TRUE(fs::exists(dir.path / "assets/steps.soundevent.meta"));

    // the file body holds no GUID: identity is the .meta's alone
    std::ifstream in(dir.path / "assets/steps.soundevent", std::ios::binary);
    const std::string body((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    EXPECT_EQ(body.find(guid.str()), std::string::npos);

    fs::create_directories(dir.path / "assets/moved");
    fs::rename(dir.path / "assets/steps.soundevent", dir.path / "assets/moved/walk.soundevent");
    const std::vector<AssetDatabase::Change> changes = database.refresh();
    ASSERT_EQ(changes.size(), 1u);
    EXPECT_EQ(changes[0].type, AssetDatabase::Change::Type::Moved);
    EXPECT_EQ(database.getAssetGuid("assets/moved/walk.soundevent"), guid);
    EXPECT_TRUE(fs::exists(dir.path / "assets/moved/walk.soundevent.meta"));
}

TEST(SoundEventAsset, OneEdgePerClipByGuidThenPath) {
    SoundEventTestDir dir;
    writeFile(dir.path / "assets/sounds/a.wav", "a");
    writeFile(dir.path / "assets/sounds/b.wav", "b");
    AssetDatabase database;
    database.open(dir.path, "assets");
    database.waitForBackgroundWork();
    const Guid a = database.getAssetGuid("assets/sounds/a.wav");
    const Guid b = database.getAssetGuid("assets/sounds/b.wav");
    ASSERT_FALSE(a.isNull());
    ASSERT_FALSE(b.isNull());

    // a by GUID with a stale hint, b by its path, and one that is missing
    writeFile(dir.path / "assets/steps.soundevent",
              SoundEventIO::write(eventOf({AssetRef("sounds/old.wav", a), AssetRef("sounds/b.wav"), AssetRef("sounds/gone.wav")})));
    const AssetIndex::Roots roots{"assets", ""};
    const std::vector<DepEdge> edges = AssetIndex::extractFile(database, dir.path, roots, "assets/steps.soundevent", AssetType::SoundEvent, Guid(1, 2));
    ASSERT_EQ(edges.size(), 3u);
    EXPECT_EQ(edges[0].field, "clips[0].clip");
    EXPECT_EQ(edges[0].target, a);
    EXPECT_EQ(edges[0].expected, AssetType::Audio);
    EXPECT_EQ(edges[1].field, "clips[1].clip");
    EXPECT_EQ(edges[1].target, b);
    EXPECT_EQ(edges[2].field, "clips[2].clip");
    EXPECT_TRUE(edges[2].target.isNull());
    EXPECT_EQ(edges[2].targetPath, "assets/sounds/gone.wav");
}

// the engine cannot include the editor's header, so the event's "type" vocabulary is listed twice
TEST(SoundEventAsset, TheTypeVocabularyIsTheEditors) {
    std::vector<std::string> editorTypes;
    for (const SoundTypes::SoundType& type : SoundTypes::all()) editorTypes.push_back(type.name);
    EXPECT_EQ(SoundEventIO::soundTypes(), editorTypes);
}
