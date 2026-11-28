// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Characterization tests of AssetDatabase (Unity's: assets by GUID, the GUID written in the
// asset file) on a project in a TempDir, written with prefabs. Since S04 every asset type is
// tracked (docs/specs/S04-asset-identity.md); its own suites are in editor/tests/asset/.

#include "util/AssetDatabase.h"

#include "TempDir.h"
#include "io/AtomicFile.h"

#include <gtest/gtest.h>
#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <system_error>
#include <vector>

namespace AssetDatabaseTest_cpp {

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::test;

namespace {

    using AssetDatabaseChange = AssetDatabase::Change;

    const std::string assetDatabaseGuidA = "0123456789abcdef0123456789abcdef";
    const std::string assetDatabaseGuidB = "fedcba9876543210fedcba9876543210";

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

        size_t count(const std::vector<AssetDatabaseChange>& changes, AssetDatabaseChange::Type type){
            return std::count_if(changes.begin(), changes.end(), [type](const AssetDatabaseChange& c){ return c.type == type; });
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
    EXPECT_TRUE(AssetDatabase::isGuid(assetDatabaseGuidA));
    EXPECT_TRUE(AssetDatabase::isGuid("0123456789ABCDEF0123456789ABCDEF"));
    EXPECT_FALSE(AssetDatabase::isGuid(assetDatabaseGuidA.substr(1)));
    EXPECT_FALSE(AssetDatabase::isGuid(assetDatabaseGuidA + "0"));
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
    dir.write("a.prefab", "name: A\nguid: " + assetDatabaseGuidA + "  # the asset\n");
    dir.write("b.prefab", "name: B\nchild:\n  guid: " + assetDatabaseGuidB + "\n");
    dir.write("c.prefab", "guid: \"" + assetDatabaseGuidB + "\"\r\nname: C\r\n");
    EXPECT_EQ(AssetDatabase::readGuid(dir.file("a.prefab")), assetDatabaseGuidA);
    EXPECT_EQ(AssetDatabase::readGuid(dir.file("b.prefab")), "");
    EXPECT_EQ(AssetDatabase::readGuid(dir.file("c.prefab")), assetDatabaseGuidB);
    EXPECT_EQ(AssetDatabase::readGuid(dir.file("missing.prefab")), "");
}

TEST(AssetDatabaseGuid, WriteGuidReplacesTheLine){
    TempDir dir;
    dir.write("a.prefab", prefab(assetDatabaseGuidA));
    ASSERT_TRUE(AssetDatabase::writeGuid(dir.file("a.prefab"), assetDatabaseGuidB));
    EXPECT_EQ(dir.read("a.prefab"), prefab(assetDatabaseGuidB));
}

TEST(AssetDatabaseGuid, AnOutsideSaveDuringTheWriteIsNotOverwritten){
    TempDir dir;
    dir.write("a.prefab", prefab("", "Before"));
    // another program saves the file after the GUID line was put into what was read, before
    // the editor's write publishes it
    bool saved = false;
    io::setAtomicFileFaultInjector([&](std::string_view step, const std::filesystem::path&) -> std::error_code {
        if (step == "write" && !saved){
            saved = true;
            dir.write("a.prefab", prefab("", "SavedOutside"));
        }
        return {};
    });
    const bool ok = AssetDatabase::writeGuid(dir.file("a.prefab"), assetDatabaseGuidA);
    io::setAtomicFileFaultInjector(nullptr);
    ASSERT_TRUE(saved);
    ASSERT_TRUE(ok);
    EXPECT_EQ(dir.read("a.prefab"), prefab(assetDatabaseGuidA, "SavedOutside")) << "the GUID goes into the outside save, read again";
}

TEST(AssetDatabaseGuid, WriteGuidInsertsAfterCommentsAndDocumentStart){
    TempDir dir;
    dir.write("a.prefab", "# a prefab\n---\nname: A\n");
    ASSERT_TRUE(AssetDatabase::writeGuid(dir.file("a.prefab"), assetDatabaseGuidA));
    EXPECT_EQ(dir.read("a.prefab"), "# a prefab\n---\nguid: " + assetDatabaseGuidA + "\nname: A\n");
}

TEST(AssetDatabaseGuid, WriteGuidKeepsWindowsLineEnds){
    TempDir dir;
    dir.write("a.prefab", "name: A\r\n");
    ASSERT_TRUE(AssetDatabase::writeGuid(dir.file("a.prefab"), assetDatabaseGuidA));
    EXPECT_EQ(dir.read("a.prefab"), "guid: " + assetDatabaseGuidA + "\r\nname: A\r\n");
}

TEST(AssetDatabaseGuid, WriteGuidRewritesAFlowStyleMap){
    TempDir dir;
    dir.write("a.prefab", "{name: A, guid: x}\n");
    ASSERT_TRUE(AssetDatabase::writeGuid(dir.file("a.prefab"), assetDatabaseGuidA));
    const YAML::Node node = YAML::LoadFile(dir.file("a.prefab").string());
    EXPECT_EQ(node["guid"].as<std::string>(), assetDatabaseGuidA);
    EXPECT_EQ(node["name"].as<std::string>(), "A");
}

TEST(AssetDatabaseGuid, ReadGuidReadsAFlowStyleMap){
    TempDir dir;
    dir.write("a.prefab", "{name: A, guid: " + assetDatabaseGuidA + ", entities: []}\n");
    dir.write("b.prefab", "# another tool's prefab\n{name: B, child: {guid: " + assetDatabaseGuidB + "}}\n");
    dir.write("c.prefab", "--- {guid: '" + assetDatabaseGuidB + "', name: C}\n");
    // block style with a flow value spread over lines: still read line by line
    dir.write("d.prefab", "name: D\nentities: [\n  {name: child}\n]\nguid: " + assetDatabaseGuidA + "\n");
    EXPECT_EQ(AssetDatabase::readGuid(dir.file("a.prefab")), assetDatabaseGuidA);
    EXPECT_EQ(AssetDatabase::readGuid(dir.file("b.prefab")), "");
    EXPECT_EQ(AssetDatabase::readGuid(dir.file("c.prefab")), assetDatabaseGuidB);
    EXPECT_EQ(AssetDatabase::readGuid(dir.file("d.prefab")), assetDatabaseGuidA);
}

TEST(AssetDatabaseGuid, WriteGuidOutputReadsBackInBothStyles){
    TempDir dir;
    const std::map<std::string, std::string> files = {
        {"block-new.prefab", "name: A\nentities: []\n"},
        {"block-old.prefab", prefab(assetDatabaseGuidB, "A")},
        {"flow-new.prefab", "{name: A, entities: [{name: child}]}\n"},
        {"flow-old.prefab", "{name: A, guid: " + assetDatabaseGuidB + "}\n"},
        {"flow-multiline.prefab", "{\n  name: A,\n  guid: x\n}\n"},
    };
    for (const auto& [name, text] : files){
        dir.write(name, text);
        ASSERT_TRUE(AssetDatabase::writeGuid(dir.file(name), assetDatabaseGuidA)) << name;
        EXPECT_EQ(AssetDatabase::readGuid(dir.file(name)), assetDatabaseGuidA) << name << ":\n" << dir.read(name);
        const YAML::Node node = YAML::LoadFile(dir.file(name).string());
        EXPECT_EQ(node["name"].as<std::string>(), "A") << name;
        // and again: a GUID written once is replaced in place
        ASSERT_TRUE(AssetDatabase::writeGuid(dir.file(name), assetDatabaseGuidB)) << name;
        EXPECT_EQ(AssetDatabase::readGuid(dir.file(name)), assetDatabaseGuidB) << name << ":\n" << dir.read(name);
    }
    const YAML::Node nested = YAML::LoadFile(dir.file("flow-new.prefab").string());
    EXPECT_EQ(nested["entities"][0]["name"].as<std::string>(), "child");
}

TEST(AssetDatabaseGuid, InFileIdentityRoundTripsInBothStyles){
    // S04's identity: the former GUIDs travel with the GUID through a flow style rewrite too
    TempDir dir;
    const Guid current = Guid::parse(assetDatabaseGuidA);
    const std::vector<Guid> former = {Guid::parse(assetDatabaseGuidB)};
    for (const std::string& text : {std::string("name: A\n"), std::string("{name: A, entities: [{name: child}]}\n"),
                                   "{name: A, guid: " + assetDatabaseGuidB + ", formerGuids: [" + assetDatabaseGuidB + "]}\n"}){
        dir.write("x.prefab", text);
        Guid read;
        std::vector<Guid> readFormer;
        ASSERT_TRUE(AssetDatabase::writeInFileIdentity(dir.file("x.prefab"), current, former)) << text;
        ASSERT_TRUE(AssetDatabase::readInFileIdentity(dir.file("x.prefab"), read, readFormer)) << text << dir.read("x.prefab");
        EXPECT_EQ(read, current) << dir.read("x.prefab");
        EXPECT_EQ(readFormer, former) << dir.read("x.prefab");
        EXPECT_EQ(YAML::LoadFile(dir.file("x.prefab").string())["name"].as<std::string>(), "A");
    }
    // a flow style document another tool wrote reads as it is
    dir.write("y.prefab", "{name: Y, guid: " + assetDatabaseGuidA + ", formerGuids: [" + assetDatabaseGuidB + "]}\n");
    Guid read;
    std::vector<Guid> readFormer;
    ASSERT_TRUE(AssetDatabase::readInFileIdentity(dir.file("y.prefab"), read, readFormer));
    EXPECT_EQ(read, current);
    EXPECT_EQ(readFormer, former);
}

TEST(AssetDatabaseGuid, WriteGuidFailsWithoutAFile){
    TempDir dir;
    EXPECT_FALSE(AssetDatabase::writeGuid(dir.file("none.prefab"), assetDatabaseGuidA));
}

TEST_F(AssetDatabaseTest, OpenRegistersEveryPrefab){
    project.write("prefabs/a.prefab", prefab(assetDatabaseGuidA));
    project.write("prefabs/deep/b.prefab", prefab(assetDatabaseGuidB));
    project.write("scenes/main.scene", "name: Main\n");
    open();
    EXPECT_TRUE(database.isOpen());
    EXPECT_EQ(database.getPath(assetDatabaseGuidA), std::filesystem::path("prefabs/a.prefab"));
    EXPECT_EQ(database.getGuid("prefabs/deep/b.prefab"), assetDatabaseGuidB);
    EXPECT_TRUE(database.contains(assetDatabaseGuidB));
    // a scene is an asset too: written before GUIDs, it gets its path's legacy GUID
    EXPECT_EQ(database.getGuid("scenes/main.scene"), AssetDatabase::legacyGuid("scenes/main.scene"));
}

TEST_F(AssetDatabaseTest, OpenSkipsHiddenAndBuildDirectories){
    project.write(".jasperengine/cache.prefab", prefab(assetDatabaseGuidA));
    project.write("build/out.prefab", prefab(assetDatabaseGuidB));
    open();
    EXPECT_FALSE(database.contains(assetDatabaseGuidA));
    EXPECT_FALSE(database.contains(assetDatabaseGuidB));
}

TEST_F(AssetDatabaseTest, LegacyPrefabsGetTheirPathGuidWrittenIntoThem){
    project.write("old.prefab", prefab(""));
    open();
    const std::string expected = AssetDatabase::legacyGuid("old.prefab");
    EXPECT_EQ(database.getGuid("old.prefab"), expected);
    EXPECT_EQ(AssetDatabase::readGuid(project.file("old.prefab")), expected);
}

TEST_F(AssetDatabaseTest, AWalkOlderThanADeletionBringsNothingBack){
    // the walk (a Background job) saw a new file a later step took back, with the folder made for it:
    // applying it must not write the file's meta, which would make the folder again
    project.write("assets/keep.txt", "text\n");
    database.open(project.path(), "assets");
    project.write("assets/rbs/new/sheet.png", "png\n");
    const AssetDatabase::DiskScan scan = database.scanDisk();
    ASSERT_TRUE(scan.valid);
    std::error_code ec;
    std::filesystem::remove_all(project.file("assets/rbs"), ec);
    ASSERT_FALSE(ec);
    const std::vector<AssetDatabaseChange> changes = database.refresh(scan);
    EXPECT_EQ(count(changes, AssetDatabaseChange::Type::Added), 0u);
    EXPECT_TRUE(database.getGuid("assets/rbs/new/sheet.png").empty());
    EXPECT_FALSE(std::filesystem::exists(project.file("assets/rbs"))) << "the meta made the folder again";
}

TEST_F(AssetDatabaseTest, AShaderForkIsFoundByItsAssetsRelativeSpelling){
    project.write("assets/shaders/fork.frag", "#version 450\n");
    database.open(project.path(), "assets");
    const AssetDatabase::Resolution fork = database.resolveInAssetsDir("shaders/fork.frag");
    ASSERT_TRUE(fork.found());
    EXPECT_EQ(fork.path, "assets/shaders/fork.frag");
    EXPECT_FALSE(database.resolveInAssetsDir("shaders/none.frag").found());
    EXPECT_FALSE(database.resolveInAssetsDir("").found());
    // not through resolve(): a load would rewrite the reference to the project-relative spelling
    EXPECT_FALSE(database.resolve(Guid(), "shaders/fork.frag", AssetType::Shader, false).found());
}

TEST_F(AssetDatabaseTest, AReadOnlyOpenWritesNothing){
    // the CLI's check next to an open editor: the identities stay in memory, the same ones an
    // editor upgrading the project writes (R3)
    project.write("old.prefab", prefab(""));
    project.write("scenes/main.scene", "name: Main\n");
    project.write("textures/hero.png", "not decoded");
    database.setReadOnly(true);
    open();
    EXPECT_EQ(database.getGuid("old.prefab"), AssetDatabase::legacyGuid("old.prefab"));
    EXPECT_EQ(database.getGuid("scenes/main.scene"), AssetDatabase::legacyGuid("scenes/main.scene"));
    EXPECT_EQ(database.getGuid("textures/hero.png"), AssetDatabase::legacyGuid("textures/hero.png"));
    database.waitForBackgroundWork();
    EXPECT_EQ(project.read("old.prefab"), prefab(""));
    EXPECT_EQ(project.read("scenes/main.scene"), "name: Main\n");
    EXPECT_FALSE(project.exists("textures/hero.png.meta"));
    EXPECT_FALSE(project.exists(".jasperengine/assetdb/index.json"));
    database.close();

    database.setReadOnly(false);
    open();
    EXPECT_EQ(AssetDatabase::readGuid(project.file("old.prefab")), AssetDatabase::legacyGuid("old.prefab"));
    EXPECT_TRUE(project.exists("textures/hero.png.meta"));
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
    project.write("a.prefab", prefab(assetDatabaseGuidA));
    project.write("b.prefab", prefab(assetDatabaseGuidA));
    open();
    const std::string a = database.getGuid("a.prefab");
    const std::string b = database.getGuid("b.prefab");
    EXPECT_NE(a, b);
    EXPECT_TRUE(a == assetDatabaseGuidA || b == assetDatabaseGuidA);
    EXPECT_TRUE(AssetDatabase::isGuid(a) && AssetDatabase::isGuid(b));
}

TEST_F(AssetDatabaseTest, ADuplicatedGuidStaysWithTheFileTheCacheKnows){
    project.write("b.prefab", prefab(assetDatabaseGuidA));
    open();
    database.close();
    project.write("a.prefab", prefab(assetDatabaseGuidA));   // a copy made outside the editor
    open();
    EXPECT_EQ(database.getGuid("b.prefab"), assetDatabaseGuidA);
    EXPECT_NE(database.getGuid("a.prefab"), assetDatabaseGuidA);
}

TEST_F(AssetDatabaseTest, OpenWritesTheCache){
    project.write("a.prefab", prefab(assetDatabaseGuidA));
    open();
    EXPECT_TRUE(project.exists(".jasperengine/assetdb/index.json"));
    EXPECT_NE(project.read(".jasperengine/assetdb/index.json").find(assetDatabaseGuidA), std::string::npos);
}

#if defined(__linux__)
// A file name in another encoding (Latin-1 'é' on Linux, where names are bytes) can't go in the
// JSON cache: its entry is left out, the others are cached, and the open does not throw (it
// failed the project's load)
TEST_F(AssetDatabaseTest, ANonUtf8NameIsLeftOutOfTheCache){
    project.write("a.prefab", prefab(assetDatabaseGuidA));
    project.write("caf\xe9.prefab", prefab(assetDatabaseGuidB));
    ASSERT_NO_THROW(open());
    EXPECT_EQ(database.getGuid("caf\xe9.prefab"), assetDatabaseGuidB);
    const std::string cache = project.read(".jasperengine/assetdb/index.json");
    EXPECT_NE(cache.find(assetDatabaseGuidA), std::string::npos);
    EXPECT_EQ(cache.find(assetDatabaseGuidB), std::string::npos);
    database.close();
    ASSERT_NO_THROW(open());
    EXPECT_EQ(database.getGuid("caf\xe9.prefab"), assetDatabaseGuidB);
}
#endif

TEST_F(AssetDatabaseTest, CloseForgetsEverything){
    project.write("a.prefab", prefab(assetDatabaseGuidA));
    open();
    database.close();
    EXPECT_FALSE(database.isOpen());
    EXPECT_FALSE(database.contains(assetDatabaseGuidA));
    EXPECT_TRUE(database.refresh().empty());
}

TEST_F(AssetDatabaseTest, ReopeningKeepsTheGuids){
    project.write("a.prefab", prefab(""));
    project.write("b.prefab", prefab(assetDatabaseGuidB));
    open();
    const std::string a = database.getGuid("a.prefab");
    database.close();
    open();
    EXPECT_EQ(database.getGuid("a.prefab"), a);
    EXPECT_EQ(database.getGuid("b.prefab"), assetDatabaseGuidB);
}

TEST_F(AssetDatabaseTest, RefreshWithoutChangesReportsNothing){
    project.write("a.prefab", prefab(assetDatabaseGuidA));
    open();
    EXPECT_TRUE(database.refresh().empty());
}

TEST_F(AssetDatabaseTest, RefreshSeesAddedFiles){
    open();
    project.write("new.prefab", prefab(assetDatabaseGuidB));
    const std::vector<AssetDatabaseChange> changes = database.refresh();
    ASSERT_EQ(changes.size(), 1u);
    EXPECT_EQ(changes[0].type, AssetDatabaseChange::Type::Added);
    EXPECT_EQ(changes[0].guid, assetDatabaseGuidB);
    EXPECT_EQ(changes[0].path, std::filesystem::path("new.prefab"));
}

TEST_F(AssetDatabaseTest, RefreshSeesRemovedFiles){
    project.write("a.prefab", prefab(assetDatabaseGuidA));
    open();
    std::filesystem::remove(project.file("a.prefab"));
    const std::vector<AssetDatabaseChange> changes = database.refresh();
    ASSERT_EQ(changes.size(), 1u);
    EXPECT_EQ(changes[0].type, AssetDatabaseChange::Type::Removed);
    EXPECT_EQ(changes[0].guid, assetDatabaseGuidA);
    EXPECT_FALSE(database.contains(assetDatabaseGuidA));
}

TEST_F(AssetDatabaseTest, RefreshSeesMovedFiles){
    project.write("a.prefab", prefab(assetDatabaseGuidA));
    open();
    std::filesystem::rename(project.file("a.prefab"), project.file("moved/a.prefab"));
    const std::vector<AssetDatabaseChange> changes = database.refresh();
    ASSERT_EQ(changes.size(), 1u);
    EXPECT_EQ(changes[0].type, AssetDatabaseChange::Type::Moved);
    EXPECT_EQ(changes[0].path, std::filesystem::path("moved/a.prefab"));
    EXPECT_EQ(changes[0].oldPath, std::filesystem::path("a.prefab"));
    EXPECT_EQ(database.getPath(assetDatabaseGuidA), std::filesystem::path("moved/a.prefab"));
}

TEST_F(AssetDatabaseTest, RefreshSeesModifiedFiles){
    project.write("a.prefab", prefab(assetDatabaseGuidA));
    open();
    project.write("a.prefab", prefab(assetDatabaseGuidA, "A much longer name than before"));
    const std::vector<AssetDatabaseChange> changes = database.refresh();
    ASSERT_EQ(changes.size(), 1u);
    EXPECT_EQ(changes[0].type, AssetDatabaseChange::Type::Modified);
    EXPECT_EQ(changes[0].guid, assetDatabaseGuidA);
}

TEST_F(AssetDatabaseTest, RefreshPutsBackAGuidAnotherToolDropped){
    project.write("a.prefab", prefab(assetDatabaseGuidA));
    open();
    project.write("a.prefab", prefab("", "Rewritten by another tool, without its GUID"));
    const std::vector<AssetDatabaseChange> changes = database.refresh();
    ASSERT_EQ(changes.size(), 1u);
    EXPECT_EQ(changes[0].type, AssetDatabaseChange::Type::Modified);
    EXPECT_EQ(AssetDatabase::readGuid(project.file("a.prefab")), assetDatabaseGuidA);
}

// A folder deleted file by file, a meta before its file (a file manager, rm -rf), with the editor's
// walks between the steps: the meta is not written again in between, so the folder can go
TEST_F(AssetDatabaseTest, AFolderDeletedFileByFileMetaFirstLeavesNothingBehind){
    project.write("extra/icon.png", "png");
    project.write("extra/notes.prefab", prefab(assetDatabaseGuidB));
    open();
    ASSERT_TRUE(project.exists("extra/icon.png.meta"));
    const std::string icon = database.getGuid("extra/icon.png");
    ASSERT_FALSE(icon.empty());

    std::filesystem::remove(project.file("extra/icon.png.meta"));
    database.refresh();
    EXPECT_FALSE(project.exists("extra/icon.png.meta")) << "written again while its folder was being deleted";
    std::filesystem::remove(project.file("extra/icon.png"));
    database.refresh();
    std::filesystem::remove(project.file("extra/notes.prefab"));
    database.refresh();
    std::error_code ec;
    EXPECT_TRUE(std::filesystem::remove(project.path() / "extra", ec)) << ec.message() << ": the folder is not empty";
    EXPECT_FALSE(database.contains(icon));
    EXPECT_FALSE(database.isMetaRestoreDue());
}

// A meta deleted alone comes back with its GUID: from the next refresh due after the one that
// found it gone, and not while other files of its folder change (a delete going on there)
TEST_F(AssetDatabaseTest, AMetaDeletedAloneIsWrittenAgainOnceItsFolderIsQuiet){
    struct NoDelay {
        NoDelay(){ AssetDatabase::setMetaRestoreDelayForTesting(std::chrono::milliseconds(0)); }
        ~NoDelay(){ AssetDatabase::setMetaRestoreDelayForTesting(std::chrono::seconds(1)); }
    } noDelay;
    project.write("extra/icon.png", "png");
    project.write("extra/a.prefab", prefab(assetDatabaseGuidA));
    open();
    const std::string icon = database.getGuid("extra/icon.png");
    ASSERT_FALSE(icon.empty());

    std::filesystem::remove(project.file("extra/icon.png.meta"));
    EXPECT_TRUE(database.refresh().empty());
    EXPECT_FALSE(project.exists("extra/icon.png.meta")) << "written by the refresh that found it gone";
    EXPECT_EQ(database.getGuid("extra/icon.png"), icon) << "still the same asset meanwhile";
    EXPECT_TRUE(database.isMetaRestoreDue());

    std::filesystem::remove(project.file("extra/a.prefab"));
    EXPECT_EQ(count(database.refresh(), AssetDatabaseChange::Type::Removed), 1u);
    EXPECT_FALSE(project.exists("extra/icon.png.meta")) << "written while another file of its folder went";

    const std::vector<AssetDatabaseChange> changes = database.refresh();
    ASSERT_EQ(changes.size(), 1u);
    EXPECT_EQ(changes[0].type, AssetDatabaseChange::Type::Modified);
    ASSERT_TRUE(project.exists("extra/icon.png.meta"));
    EXPECT_EQ(AssetDatabase::readGuid(project.file("extra/icon.png.meta")), icon);
    EXPECT_FALSE(database.isMetaRestoreDue());
    EXPECT_TRUE(database.refresh().empty());
}

// Closed before it was due: written then, or the next open would give the file a new GUID
TEST_F(AssetDatabaseTest, CloseWritesADeletedMetaStillWaiting){
    project.write("icon.png", "png");
    open();
    const std::string icon = database.getGuid("icon.png");
    std::filesystem::remove(project.file("icon.png.meta"));
    database.refresh();
    ASSERT_FALSE(project.exists("icon.png.meta"));
    database.close();
    EXPECT_TRUE(project.exists("icon.png.meta"));
    open();
    EXPECT_EQ(database.getGuid("icon.png"), icon);
}

TEST_F(AssetDatabaseTest, RefreshGivesACopyANewGuid){
    project.write("a.prefab", prefab(assetDatabaseGuidA));
    open();
    std::filesystem::copy_file(project.file("a.prefab"), project.file("copy.prefab"));
    const std::vector<AssetDatabaseChange> changes = database.refresh();
    ASSERT_EQ(changes.size(), 1u);
    EXPECT_EQ(changes[0].type, AssetDatabaseChange::Type::Added);
    EXPECT_NE(changes[0].guid, assetDatabaseGuidA);
    EXPECT_EQ(database.getGuid("a.prefab"), assetDatabaseGuidA);
    EXPECT_EQ(AssetDatabase::readGuid(project.file("copy.prefab")), changes[0].guid);
}

TEST_F(AssetDatabaseTest, RefreshOfAFileReplacedByAnotherAsset){
    project.write("a.prefab", prefab(assetDatabaseGuidA));
    open();
    project.write("a.prefab", prefab(assetDatabaseGuidB, "Another asset entirely"));
    const std::vector<AssetDatabaseChange> changes = database.refresh();
    EXPECT_EQ(count(changes, AssetDatabaseChange::Type::Removed), 1u);
    EXPECT_EQ(count(changes, AssetDatabaseChange::Type::Added), 1u);
    EXPECT_EQ(database.getGuid("a.prefab"), assetDatabaseGuidB);
    EXPECT_FALSE(database.contains(assetDatabaseGuidA));
}

TEST_F(AssetDatabaseTest, MoveAssetsKeepsTheGuids){
    project.write("dir/a.prefab", prefab(assetDatabaseGuidA));
    project.write("dir/b.prefab", prefab(assetDatabaseGuidB));
    open();
    std::filesystem::rename(project.file("dir"), project.file("other"));
    const std::vector<AssetDatabaseChange> changes = database.moveAssets(project.path() / "dir", project.path() / "other");
    EXPECT_EQ(count(changes, AssetDatabaseChange::Type::Moved), 2u);
    EXPECT_EQ(database.getPath(assetDatabaseGuidA), std::filesystem::path("other/a.prefab"));
    EXPECT_EQ(database.getPath(assetDatabaseGuidB), std::filesystem::path("other/b.prefab"));
}

TEST_F(AssetDatabaseTest, RenamingAwayFromAnAssetTypeRemovesTheAsset){
    project.write("a.prefab", prefab(assetDatabaseGuidA));
    open();
    std::filesystem::rename(project.file("a.prefab"), project.file("a.bak"));
    const std::vector<AssetDatabaseChange> changes = database.moveAssets(project.path() / "a.prefab", project.path() / "a.bak");
    ASSERT_EQ(changes.size(), 1u);
    EXPECT_EQ(changes[0].type, AssetDatabaseChange::Type::Removed);
    EXPECT_FALSE(database.contains(assetDatabaseGuidA));
}

TEST_F(AssetDatabaseTest, RemoveAssetsTakesADirectory){
    project.write("dir/a.prefab", prefab(assetDatabaseGuidA));
    project.write("dir/sub/b.prefab", prefab(assetDatabaseGuidB));
    project.write("keep.prefab", prefab(""));
    open();
    const std::vector<AssetDatabaseChange> changes = database.removeAssets(project.path() / "dir");
    EXPECT_EQ(count(changes, AssetDatabaseChange::Type::Removed), 2u);
    EXPECT_FALSE(database.contains(assetDatabaseGuidA));
    EXPECT_FALSE(database.contains(assetDatabaseGuidB));
    EXPECT_FALSE(database.getGuid("keep.prefab").empty());
}

TEST_F(AssetDatabaseTest, ImportAssetsAddsNewFiles){
    open();
    project.write("imported/a.prefab", prefab(assetDatabaseGuidA));
    const std::vector<AssetDatabaseChange> changes = database.importAssets(project.path() / "imported");
    ASSERT_EQ(changes.size(), 1u);
    EXPECT_EQ(changes[0].type, AssetDatabaseChange::Type::Added);
    EXPECT_EQ(database.getGuid("imported/a.prefab"), assetDatabaseGuidA);
}

TEST_F(AssetDatabaseTest, ImportingAFileBackKeepsItsGuid){
    project.write("a.prefab", prefab(assetDatabaseGuidA));
    open();
    const std::string text = project.read("a.prefab");
    std::filesystem::remove(project.file("a.prefab"));
    database.removeAssets(project.path() / "a.prefab");
    project.write("a.prefab", text);
    database.importAssets(project.path() / "a.prefab");
    EXPECT_EQ(database.getGuid("a.prefab"), assetDatabaseGuidA);
}

TEST_F(AssetDatabaseTest, ImportingACopyGivesItANewGuidAndRedoRepeatsIt){
    project.write("a.prefab", prefab(assetDatabaseGuidA));
    open();
    std::filesystem::copy_file(project.file("a.prefab"), project.file("b.prefab"));
    std::map<std::string, std::string> copyGuids;
    const std::vector<AssetDatabaseChange> first = database.importAssets(project.path() / "b.prefab", true, &copyGuids);
    ASSERT_EQ(first.size(), 1u);
    const std::string copyGuid = database.getGuid("b.prefab");
    EXPECT_NE(copyGuid, assetDatabaseGuidA);
    EXPECT_EQ(copyGuids["b.prefab"], copyGuid);

    // undo (the copy deleted), then redo: the same GUID again
    std::filesystem::remove(project.file("b.prefab"));
    database.removeAssets(project.path() / "b.prefab");
    std::filesystem::copy_file(project.file("a.prefab"), project.file("b.prefab"));
    database.importAssets(project.path() / "b.prefab", true, &copyGuids);
    EXPECT_EQ(database.getGuid("b.prefab"), copyGuid);
}

TEST_F(AssetDatabaseTest, ImportingAFileWhoseTwinIsGoneTreatsItAsMoved){
    project.write("a.prefab", prefab(assetDatabaseGuidA));
    open();
    std::filesystem::rename(project.file("a.prefab"), project.file("b.prefab"));
    const std::vector<AssetDatabaseChange> changes = database.importAssets(project.path() / "b.prefab");
    ASSERT_EQ(changes.size(), 1u);
    EXPECT_EQ(changes[0].type, AssetDatabaseChange::Type::Moved);
    EXPECT_EQ(changes[0].oldPath, std::filesystem::path("a.prefab"));
}

TEST_F(AssetDatabaseTest, RegisterAssetTakesTheGuidOverFromItsOldPath){
    project.write("a.prefab", prefab(assetDatabaseGuidA));
    open();
    project.write("saved.prefab", prefab(assetDatabaseGuidA));
    database.registerAsset(project.path() / "saved.prefab", assetDatabaseGuidA);
    EXPECT_EQ(database.getPath(assetDatabaseGuidA), std::filesystem::path("saved.prefab"));
    EXPECT_EQ(database.getGuid("a.prefab"), "");
}

TEST_F(AssetDatabaseTest, ToRelativeStripsTheProject){
    open();
    EXPECT_EQ(database.toRelative(project.path() / "x" / ".." / "y.prefab"), std::filesystem::path("y.prefab"));
    EXPECT_EQ(database.toRelative("already/relative.prefab"), std::filesystem::path("already/relative.prefab"));
    EXPECT_EQ(database.toRelative(""), std::filesystem::path());
}

}  // namespace AssetDatabaseTest_cpp
