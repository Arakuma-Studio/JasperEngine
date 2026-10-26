// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Characterization tests of AssetDatabase (Unity's: assets by GUID, the GUID written in the
// asset file) on a project in a TempDir, written with prefabs. Since S04 every asset type is
// tracked (docs/specs/S04-asset-identity.md); its own suites are in editor/tests/asset/.

#include "util/AssetDatabase.h"

#include "TempDir.h"

#include <gtest/gtest.h>
#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::test;

namespace {

    using Change = AssetDatabase::Change;

    const std::string guidA = "0123456789abcdef0123456789abcdef";
    const std::string guidB = "fedcba9876543210fedcba9876543210";

    std::string prefab(const std::string& guid, const std::string& name = "Thing"){
        std::string text;
        if (!guid.empty())
            text += "guid: " + guid + "\n";
        return text + "name: " + name + "\nentities: []\n";
    }

    class AssetDatabaseTest : public ::testing::Test {
    protected:
        TempDir project;
        AssetDatabase database;

        void open(){
            database.open(project.path());
        }

        size_t count(const std::vector<Change>& changes, Change::Type type){
            return std::count_if(changes.begin(), changes.end(), [type](const Change& c){ return c.type == type; });
        }
    };

}

TEST(AssetDatabaseGuid, GeneratedGuidsAreThirtyTwoHexDigits){
    std::set<std::string> seen;
    for (int i = 0; i < 100; i++){
        const std::string guid = AssetDatabase::generateGuid();
        EXPECT_TRUE(AssetDatabase::isGuid(guid)) << guid;
        EXPECT_EQ(guid.size(), 32u);
        EXPECT_EQ(guid.find_first_not_of("0123456789abcdef"), std::string::npos) << guid;
        seen.insert(guid);
    }
    EXPECT_EQ(seen.size(), 100u);
}

TEST(AssetDatabaseGuid, IsGuidChecksLengthAndDigits){
    EXPECT_TRUE(AssetDatabase::isGuid(guidA));
    EXPECT_TRUE(AssetDatabase::isGuid("0123456789ABCDEF0123456789ABCDEF"));
    EXPECT_FALSE(AssetDatabase::isGuid(guidA.substr(1)));
    EXPECT_FALSE(AssetDatabase::isGuid(guidA + "0"));
    EXPECT_FALSE(AssetDatabase::isGuid("0123456789abcdef0123456789abcdeg"));
    EXPECT_FALSE(AssetDatabase::isGuid(""));
}

TEST(AssetDatabaseGuid, LegacyGuidIsStableAndPathBased){
    const std::string guid = AssetDatabase::legacyGuid("prefabs/enemy.prefab");
    EXPECT_TRUE(AssetDatabase::isGuid(guid));
    EXPECT_EQ(AssetDatabase::legacyGuid("prefabs/enemy.prefab"), guid);
    EXPECT_EQ(AssetDatabase::legacyGuid("prefabs/./enemy.prefab"), guid);
    EXPECT_NE(AssetDatabase::legacyGuid("prefabs/boss.prefab"), guid);
}

TEST(AssetDatabaseGuid, TracksEveryAssetType){
    AssetDatabase database;
    EXPECT_TRUE(database.isTrackedFile("a/b.prefab"));
    EXPECT_TRUE(database.isTrackedFile("a/b.scene"));
    EXPECT_TRUE(database.isTrackedFile("a/b.png"));
    EXPECT_FALSE(database.isTrackedFile("a/b.png.meta"));
    EXPECT_FALSE(database.isTrackedFile("a/b.bak"));
}

TEST(AssetDatabaseGuid, ReadGuidFindsTheTopLevelKey){
    TempDir dir;
    dir.write("a.prefab", "name: A\nguid: " + guidA + "  # the asset\n");
    dir.write("b.prefab", "name: B\nchild:\n  guid: " + guidB + "\n");
    dir.write("c.prefab", "guid: \"" + guidB + "\"\r\nname: C\r\n");
    EXPECT_EQ(AssetDatabase::readGuid(dir.file("a.prefab")), guidA);
    EXPECT_EQ(AssetDatabase::readGuid(dir.file("b.prefab")), "");
    EXPECT_EQ(AssetDatabase::readGuid(dir.file("c.prefab")), guidB);
    EXPECT_EQ(AssetDatabase::readGuid(dir.file("missing.prefab")), "");
}

TEST(AssetDatabaseGuid, WriteGuidReplacesTheLine){
    TempDir dir;
    dir.write("a.prefab", prefab(guidA));
    ASSERT_TRUE(AssetDatabase::writeGuid(dir.file("a.prefab"), guidB));
    EXPECT_EQ(dir.read("a.prefab"), prefab(guidB));
}

TEST(AssetDatabaseGuid, WriteGuidInsertsAfterCommentsAndDocumentStart){
    TempDir dir;
    dir.write("a.prefab", "# a prefab\n---\nname: A\n");
    ASSERT_TRUE(AssetDatabase::writeGuid(dir.file("a.prefab"), guidA));
    EXPECT_EQ(dir.read("a.prefab"), "# a prefab\n---\nguid: " + guidA + "\nname: A\n");
}

TEST(AssetDatabaseGuid, WriteGuidKeepsWindowsLineEnds){
    TempDir dir;
    dir.write("a.prefab", "name: A\r\n");
    ASSERT_TRUE(AssetDatabase::writeGuid(dir.file("a.prefab"), guidA));
    EXPECT_EQ(dir.read("a.prefab"), "guid: " + guidA + "\r\nname: A\r\n");
}

TEST(AssetDatabaseGuid, WriteGuidRewritesAFlowStyleMap){
    TempDir dir;
    dir.write("a.prefab", "{name: A, guid: x}\n");
    ASSERT_TRUE(AssetDatabase::writeGuid(dir.file("a.prefab"), guidA));
    const YAML::Node node = YAML::LoadFile(dir.file("a.prefab").string());
    EXPECT_EQ(node["guid"].as<std::string>(), guidA);
    EXPECT_EQ(node["name"].as<std::string>(), "A");
}

TEST(AssetDatabaseGuid, WriteGuidFailsWithoutAFile){
    TempDir dir;
    EXPECT_FALSE(AssetDatabase::writeGuid(dir.file("none.prefab"), guidA));
}

TEST_F(AssetDatabaseTest, OpenRegistersEveryPrefab){
    project.write("prefabs/a.prefab", prefab(guidA));
    project.write("prefabs/deep/b.prefab", prefab(guidB));
    project.write("scenes/main.scene", "name: Main\n");
    open();
    EXPECT_TRUE(database.isOpen());
    EXPECT_EQ(database.getPath(guidA), std::filesystem::path("prefabs/a.prefab"));
    EXPECT_EQ(database.getGuid("prefabs/deep/b.prefab"), guidB);
    EXPECT_TRUE(database.contains(guidB));
    // a scene is an asset too: written before GUIDs, it gets its path's legacy GUID
    EXPECT_EQ(database.getGuid("scenes/main.scene"), AssetDatabase::legacyGuid("scenes/main.scene"));
}

TEST_F(AssetDatabaseTest, OpenSkipsHiddenAndBuildDirectories){
    project.write(".jasperengine/cache.prefab", prefab(guidA));
    project.write("build/out.prefab", prefab(guidB));
    open();
    EXPECT_FALSE(database.contains(guidA));
    EXPECT_FALSE(database.contains(guidB));
}

TEST_F(AssetDatabaseTest, LegacyPrefabsGetTheirPathGuidWrittenIntoThem){
    project.write("old.prefab", prefab(""));
    open();
    const std::string expected = AssetDatabase::legacyGuid("old.prefab");
    EXPECT_EQ(database.getGuid("old.prefab"), expected);
    EXPECT_EQ(AssetDatabase::readGuid(project.file("old.prefab")), expected);
}

TEST_F(AssetDatabaseTest, AnInvalidGuidIsReplaced){
    project.write("bad.prefab", prefab("not-a-guid"));
    open();
    const std::string guid = database.getGuid("bad.prefab");
    EXPECT_TRUE(AssetDatabase::isGuid(guid));
    EXPECT_NE(guid, AssetDatabase::legacyGuid("bad.prefab"));
    EXPECT_EQ(AssetDatabase::readGuid(project.file("bad.prefab")), guid);
}

TEST_F(AssetDatabaseTest, ADuplicatedGuidStaysWithOneFileAndTheOtherGetsANewOne){
    project.write("a.prefab", prefab(guidA));
    project.write("b.prefab", prefab(guidA));
    open();
    const std::string a = database.getGuid("a.prefab");
    const std::string b = database.getGuid("b.prefab");
    EXPECT_NE(a, b);
    EXPECT_TRUE(a == guidA || b == guidA);
    EXPECT_TRUE(AssetDatabase::isGuid(a) && AssetDatabase::isGuid(b));
}

TEST_F(AssetDatabaseTest, ADuplicatedGuidStaysWithTheFileTheCacheKnows){
    project.write("b.prefab", prefab(guidA));
    open();
    database.close();
    project.write("a.prefab", prefab(guidA));   // a copy made outside the editor
    open();
    EXPECT_EQ(database.getGuid("b.prefab"), guidA);
    EXPECT_NE(database.getGuid("a.prefab"), guidA);
}

TEST_F(AssetDatabaseTest, OpenWritesTheCache){
    project.write("a.prefab", prefab(guidA));
    open();
    EXPECT_TRUE(project.exists(".jasperengine/assetdb/index.json"));
    EXPECT_NE(project.read(".jasperengine/assetdb/index.json").find(guidA), std::string::npos);
}

#if defined(__linux__)
// A file name in another encoding (Latin-1 'é' on Linux, where names are bytes) can't go in the
// JSON cache: its entry is left out, the others are cached, and the open does not throw (it
// failed the project's load)
TEST_F(AssetDatabaseTest, ANonUtf8NameIsLeftOutOfTheCache){
    project.write("a.prefab", prefab(guidA));
    project.write("caf\xe9.prefab", prefab(guidB));
    ASSERT_NO_THROW(open());
    EXPECT_EQ(database.getGuid("caf\xe9.prefab"), guidB);
    const std::string cache = project.read(".jasperengine/assetdb/index.json");
    EXPECT_NE(cache.find(guidA), std::string::npos);
    EXPECT_EQ(cache.find(guidB), std::string::npos);
    database.close();
    ASSERT_NO_THROW(open());
    EXPECT_EQ(database.getGuid("caf\xe9.prefab"), guidB);
}
#endif

TEST_F(AssetDatabaseTest, CloseForgetsEverything){
    project.write("a.prefab", prefab(guidA));
    open();
    database.close();
    EXPECT_FALSE(database.isOpen());
    EXPECT_FALSE(database.contains(guidA));
    EXPECT_TRUE(database.refresh().empty());
}

TEST_F(AssetDatabaseTest, ReopeningKeepsTheGuids){
    project.write("a.prefab", prefab(""));
    project.write("b.prefab", prefab(guidB));
    open();
    const std::string a = database.getGuid("a.prefab");
    database.close();
    open();
    EXPECT_EQ(database.getGuid("a.prefab"), a);
    EXPECT_EQ(database.getGuid("b.prefab"), guidB);
}

TEST_F(AssetDatabaseTest, RefreshWithoutChangesReportsNothing){
    project.write("a.prefab", prefab(guidA));
    open();
    EXPECT_TRUE(database.refresh().empty());
}

TEST_F(AssetDatabaseTest, RefreshSeesAddedFiles){
    open();
    project.write("new.prefab", prefab(guidB));
    const std::vector<Change> changes = database.refresh();
    ASSERT_EQ(changes.size(), 1u);
    EXPECT_EQ(changes[0].type, Change::Type::Added);
    EXPECT_EQ(changes[0].guid, guidB);
    EXPECT_EQ(changes[0].path, std::filesystem::path("new.prefab"));
}

TEST_F(AssetDatabaseTest, RefreshSeesRemovedFiles){
    project.write("a.prefab", prefab(guidA));
    open();
    std::filesystem::remove(project.file("a.prefab"));
    const std::vector<Change> changes = database.refresh();
    ASSERT_EQ(changes.size(), 1u);
    EXPECT_EQ(changes[0].type, Change::Type::Removed);
    EXPECT_EQ(changes[0].guid, guidA);
    EXPECT_FALSE(database.contains(guidA));
}

TEST_F(AssetDatabaseTest, RefreshSeesMovedFiles){
    project.write("a.prefab", prefab(guidA));
    open();
    std::filesystem::rename(project.file("a.prefab"), project.file("moved/a.prefab"));
    const std::vector<Change> changes = database.refresh();
    ASSERT_EQ(changes.size(), 1u);
    EXPECT_EQ(changes[0].type, Change::Type::Moved);
    EXPECT_EQ(changes[0].path, std::filesystem::path("moved/a.prefab"));
    EXPECT_EQ(changes[0].oldPath, std::filesystem::path("a.prefab"));
    EXPECT_EQ(database.getPath(guidA), std::filesystem::path("moved/a.prefab"));
}

TEST_F(AssetDatabaseTest, RefreshSeesModifiedFiles){
    project.write("a.prefab", prefab(guidA));
    open();
    project.write("a.prefab", prefab(guidA, "A much longer name than before"));
    const std::vector<Change> changes = database.refresh();
    ASSERT_EQ(changes.size(), 1u);
    EXPECT_EQ(changes[0].type, Change::Type::Modified);
    EXPECT_EQ(changes[0].guid, guidA);
}

TEST_F(AssetDatabaseTest, RefreshPutsBackAGuidAnotherToolDropped){
    project.write("a.prefab", prefab(guidA));
    open();
    project.write("a.prefab", prefab("", "Rewritten by another tool, without its GUID"));
    const std::vector<Change> changes = database.refresh();
    ASSERT_EQ(changes.size(), 1u);
    EXPECT_EQ(changes[0].type, Change::Type::Modified);
    EXPECT_EQ(AssetDatabase::readGuid(project.file("a.prefab")), guidA);
}

TEST_F(AssetDatabaseTest, RefreshGivesACopyANewGuid){
    project.write("a.prefab", prefab(guidA));
    open();
    std::filesystem::copy_file(project.file("a.prefab"), project.file("copy.prefab"));
    const std::vector<Change> changes = database.refresh();
    ASSERT_EQ(changes.size(), 1u);
    EXPECT_EQ(changes[0].type, Change::Type::Added);
    EXPECT_NE(changes[0].guid, guidA);
    EXPECT_EQ(database.getGuid("a.prefab"), guidA);
    EXPECT_EQ(AssetDatabase::readGuid(project.file("copy.prefab")), changes[0].guid);
}

TEST_F(AssetDatabaseTest, RefreshOfAFileReplacedByAnotherAsset){
    project.write("a.prefab", prefab(guidA));
    open();
    project.write("a.prefab", prefab(guidB, "Another asset entirely"));
    const std::vector<Change> changes = database.refresh();
    EXPECT_EQ(count(changes, Change::Type::Removed), 1u);
    EXPECT_EQ(count(changes, Change::Type::Added), 1u);
    EXPECT_EQ(database.getGuid("a.prefab"), guidB);
    EXPECT_FALSE(database.contains(guidA));
}

TEST_F(AssetDatabaseTest, MoveAssetsKeepsTheGuids){
    project.write("dir/a.prefab", prefab(guidA));
    project.write("dir/b.prefab", prefab(guidB));
    open();
    std::filesystem::rename(project.file("dir"), project.file("other"));
    const std::vector<Change> changes = database.moveAssets(project.path() / "dir", project.path() / "other");
    EXPECT_EQ(count(changes, Change::Type::Moved), 2u);
    EXPECT_EQ(database.getPath(guidA), std::filesystem::path("other/a.prefab"));
    EXPECT_EQ(database.getPath(guidB), std::filesystem::path("other/b.prefab"));
}

TEST_F(AssetDatabaseTest, RenamingAwayFromAnAssetTypeRemovesTheAsset){
    project.write("a.prefab", prefab(guidA));
    open();
    std::filesystem::rename(project.file("a.prefab"), project.file("a.bak"));
    const std::vector<Change> changes = database.moveAssets(project.path() / "a.prefab", project.path() / "a.bak");
    ASSERT_EQ(changes.size(), 1u);
    EXPECT_EQ(changes[0].type, Change::Type::Removed);
    EXPECT_FALSE(database.contains(guidA));
}

TEST_F(AssetDatabaseTest, RemoveAssetsTakesADirectory){
    project.write("dir/a.prefab", prefab(guidA));
    project.write("dir/sub/b.prefab", prefab(guidB));
    project.write("keep.prefab", prefab(""));
    open();
    const std::vector<Change> changes = database.removeAssets(project.path() / "dir");
    EXPECT_EQ(count(changes, Change::Type::Removed), 2u);
    EXPECT_FALSE(database.contains(guidA));
    EXPECT_FALSE(database.contains(guidB));
    EXPECT_FALSE(database.getGuid("keep.prefab").empty());
}

TEST_F(AssetDatabaseTest, ImportAssetsAddsNewFiles){
    open();
    project.write("imported/a.prefab", prefab(guidA));
    const std::vector<Change> changes = database.importAssets(project.path() / "imported");
    ASSERT_EQ(changes.size(), 1u);
    EXPECT_EQ(changes[0].type, Change::Type::Added);
    EXPECT_EQ(database.getGuid("imported/a.prefab"), guidA);
}

TEST_F(AssetDatabaseTest, ImportingAFileBackKeepsItsGuid){
    project.write("a.prefab", prefab(guidA));
    open();
    const std::string text = project.read("a.prefab");
    std::filesystem::remove(project.file("a.prefab"));
    database.removeAssets(project.path() / "a.prefab");
    project.write("a.prefab", text);
    database.importAssets(project.path() / "a.prefab");
    EXPECT_EQ(database.getGuid("a.prefab"), guidA);
}

TEST_F(AssetDatabaseTest, ImportingACopyGivesItANewGuidAndRedoRepeatsIt){
    project.write("a.prefab", prefab(guidA));
    open();
    std::filesystem::copy_file(project.file("a.prefab"), project.file("b.prefab"));
    std::map<std::string, std::string> copyGuids;
    const std::vector<Change> first = database.importAssets(project.path() / "b.prefab", true, &copyGuids);
    ASSERT_EQ(first.size(), 1u);
    const std::string copyGuid = database.getGuid("b.prefab");
    EXPECT_NE(copyGuid, guidA);
    EXPECT_EQ(copyGuids["b.prefab"], copyGuid);

    // undo (the copy deleted), then redo: the same GUID again
    std::filesystem::remove(project.file("b.prefab"));
    database.removeAssets(project.path() / "b.prefab");
    std::filesystem::copy_file(project.file("a.prefab"), project.file("b.prefab"));
    database.importAssets(project.path() / "b.prefab", true, &copyGuids);
    EXPECT_EQ(database.getGuid("b.prefab"), copyGuid);
}

TEST_F(AssetDatabaseTest, ImportingAFileWhoseTwinIsGoneTreatsItAsMoved){
    project.write("a.prefab", prefab(guidA));
    open();
    std::filesystem::rename(project.file("a.prefab"), project.file("b.prefab"));
    const std::vector<Change> changes = database.importAssets(project.path() / "b.prefab");
    ASSERT_EQ(changes.size(), 1u);
    EXPECT_EQ(changes[0].type, Change::Type::Moved);
    EXPECT_EQ(changes[0].oldPath, std::filesystem::path("a.prefab"));
}

TEST_F(AssetDatabaseTest, RegisterAssetTakesTheGuidOverFromItsOldPath){
    project.write("a.prefab", prefab(guidA));
    open();
    project.write("saved.prefab", prefab(guidA));
    database.registerAsset(project.path() / "saved.prefab", guidA);
    EXPECT_EQ(database.getPath(guidA), std::filesystem::path("saved.prefab"));
    EXPECT_EQ(database.getGuid("a.prefab"), "");
}

TEST_F(AssetDatabaseTest, ToRelativeStripsTheProject){
    open();
    EXPECT_EQ(database.toRelative(project.path() / "x" / ".." / "y.prefab"), std::filesystem::path("y.prefab"));
    EXPECT_EQ(database.toRelative("already/relative.prefab"), std::filesystem::path("already/relative.prefab"));
    EXPECT_EQ(database.toRelative(""), std::filesystem::path());
}
