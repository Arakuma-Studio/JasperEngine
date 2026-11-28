// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// content-manifest.json (docs/specs/S30-shipping.md R21): what the editor writes the player and
// the payload checks read back whole, and the checks a player makes at start.

#include <gtest/gtest.h>

#include "LogCapture.h"

#include "player/ContentManifest.h"
#include "player/PlayerConfig.h"
#include "player/PlayerModules.h"
#include "serialization/CookedScene.h"

#include <algorithm>
#include <set>

namespace content_manifest_test {

using namespace jasperengine;
using namespace jasperengine::player;


    ContentManifest sample() {
        ContentManifest m;
        m.engineVersion = "v1.0.0";
        m.contentFormatVersion = cook::contentFormatVersion();
        m.playerId = "linux-x64-glcore-release";
        m.playerSha256 = std::string(64, 'a');
        m.method = "prebuilt";
        m.backend = "glcore";
        m.startScene = 3;
        m.scenes = {"scenes/3.jscn", "scenes/4.jscn"};
        m.prefabs = {"prefabs/2.jpfb"};
        m.shaders = {"MESH_x_glsl410.sdat"};
        m.files["game.json"] = {std::string(64, 'b'), 120};
        m.files["licenses/lua-LICENSE"] = {std::string(64, 'c'), 1077};
        m.packs.push_back({"content.jpak", std::string(64, 'd'), 812});
        m.notices = {"lua", "sokol"};
        m.modules = {"physics3d", "navigation"};
        m.hasModules = true;
        m.symbols = "/home/me/Game/.jasperengine/symbols/linux";
        return m;
    }

    // what the content of a run holds (ContentAccess takes plain functions)
    std::set<std::string>& present() {
        static std::set<std::string> files;
        return files;
    }
    bool exists(const std::string& path) { return present().count(path) > 0; }
    bool noSha(const std::string&, std::string&, uint64_t&) { return false; }

}

namespace content_manifest_test {

using namespace content_manifest_test;

TEST(ContentManifest, RoundTripsEveryField) {
    const ContentManifest m = sample();
    ContentManifest back;
    std::string error;
    ASSERT_TRUE(ContentManifest::parse(m.toJson().dump(2), back, error)) << error;
    EXPECT_EQ(back.toJson(), m.toJson());
    ASSERT_EQ(back.files.size(), 2u);
    EXPECT_EQ(back.files["licenses/lua-LICENSE"].size, 1077u);
    ASSERT_EQ(back.packs.size(), 1u);
    EXPECT_EQ(back.packs[0].entries, 812u);
    EXPECT_EQ(back.symbols, m.symbols);
    EXPECT_TRUE(back.hasModules);
    EXPECT_EQ(back.modules, m.modules);
}

TEST(ContentManifest, RefusesNewerSchemaAndBadJson) {
    ContentManifest back;
    std::string error;
    EXPECT_FALSE(ContentManifest::parse("{\"schemaVersion\": 99}", back, error));
    EXPECT_NE(error.find("schema 99"), std::string::npos) << error;
    EXPECT_FALSE(ContentManifest::parse("{", back, error));
    EXPECT_FALSE(ContentManifest::parse("{}", back, error));
}

TEST(ContentManifest, ChecksAtStart) {
    const ContentManifest m = sample();
    present() = {"scenes/3.jscn", "scenes/4.jscn", "prefabs/2.jpfb", "assets/shaders/MESH_x_glsl410.sdat", "content.jpak"};
    const ContentAccess access{&exists, &noSha};
    std::vector<std::string> problems;
    EXPECT_TRUE(checkContent(m, "linux-x64-glcore-release", access, false, problems));
    problems.clear();
    EXPECT_FALSE(checkContent(m, "linux-x64-vulkan-release", access, false, problems));
    ASSERT_FALSE(problems.empty());
    EXPECT_NE(problems[0].find("build it again for this player"), std::string::npos) << problems[0];
    present().erase("scenes/3.jscn");
    problems.clear();
    EXPECT_FALSE(checkContent(m, "linux-x64-glcore-release", access, false, problems));
}

TEST(ContentManifest, ReadsContentFormat) {
    EXPECT_TRUE(cook::playerReadsContentFormat(cook::contentFormatVersion()));
    EXPECT_FALSE(cook::playerReadsContentFormat(std::to_string(cook::FORMAT_MAJOR + 1) + ".0"));
    EXPECT_FALSE(cook::playerReadsContentFormat("1.x"));
    EXPECT_TRUE(cook::readsContentFormat("1.5", "1.5"));
    EXPECT_TRUE(cook::readsContentFormat("1.5", "1.0"));
    EXPECT_TRUE(cook::readsContentFormat("1.12", "1.9"));
    EXPECT_FALSE(cook::readsContentFormat("1.5", "1.6"));
    EXPECT_FALSE(cook::readsContentFormat("1.9", "1.12"));
    EXPECT_FALSE(cook::readsContentFormat("1.5", "2.0"));
    EXPECT_FALSE(cook::readsContentFormat("2.0", "1.5"));
    for (const char* bad : {"", "1", "1.", ".5", "1.x", "v1.5", "1.5.0", "-1.5"}) EXPECT_FALSE(cook::readsContentFormat("1.5", bad)) << bad;
    // Build Profiles: game.json's buildProfile and defines need a runtime of BUILD_PROFILE_MINOR (one
    // of the cooked files' minor would ignore the defines and run a demo as the full game); a build
    // without a profile needs only the cooked files' minor, so older runtimes still serve it
    const std::string cooked = std::to_string(cook::FORMAT_MAJOR) + "." + std::to_string(cook::FORMAT_MINOR);
    EXPECT_EQ(cook::buildContentFormatVersion(false), cooked);
    EXPECT_EQ(cook::buildContentFormatVersion(true),
              std::to_string(cook::FORMAT_MAJOR) + "." + std::to_string(std::max(cook::FORMAT_MINOR, cook::BUILD_PROFILE_MINOR)));
    EXPECT_TRUE(cook::readsContentFormat(cook::buildContentFormatVersion(true), cooked)) << "a profile's build carries the cooked files too";
    EXPECT_TRUE(cook::readsContentFormat(cook::contentFormatVersion(), cook::buildContentFormatVersion(true)));
    EXPECT_TRUE(cook::readsContentFormat(cooked, cook::buildContentFormatVersion(false)));
    // (a cooked layout past BUILD_PROFILE_MINOR, S95's 1.12/1.13, reads a profile's build too; a
    // runtime before BUILD_PROFILE_MINOR never does: PlayerRuntimes::find's buildProfile)
    EXPECT_EQ(cook::readsContentFormat(cooked, cook::buildContentFormatVersion(true)), cook::FORMAT_MINOR >= cook::BUILD_PROFILE_MINOR);
    EXPECT_FALSE(cook::readsContentFormat(std::to_string(cook::FORMAT_MAJOR) + "." + std::to_string(cook::BUILD_PROFILE_MINOR - 1),
                                          cook::buildContentFormatVersion(true)));
}

// game.json's side of the same (R17): the profile and its defines, absent without a profile
TEST(PlayerConfig, BuildProfileAndDefinesRoundTrip) {
    PlayerConfig game;
    game.name = "Box";
    EXPECT_FALSE(game.toJson().contains("buildProfile"));
    EXPECT_FALSE(game.toJson().contains("defines"));
    game.buildProfile = "Demo";
    game.defines = {"DEMO", "NO_ADS"};
    PlayerConfig back;
    std::string error;
    ASSERT_TRUE(PlayerConfig::parse(game.toJson().dump(), back, error)) << error;
    EXPECT_EQ(back.buildProfile, "Demo");
    EXPECT_EQ(back.defines, game.defines);
}

TEST(ContentManifest, BuildProfileRoundTrips) {
    ContentManifest m = sample();
    // without a profile the key is absent: a build of no profile writes what it wrote before
    EXPECT_FALSE(m.toJson().contains("buildProfile"));
    m.buildProfile = "Steam Demo";
    ContentManifest back;
    std::string error;
    ASSERT_TRUE(ContentManifest::parse(m.toJson().dump(2), back, error)) << error;
    EXPECT_EQ(back.buildProfile, "Steam Demo");
    EXPECT_EQ(back.toJson(), m.toJson());
    // an older manifest has none
    ContentManifest old;
    ASSERT_TRUE(ContentManifest::parse(sample().toJson().dump(), old, error)) << error;
    EXPECT_EQ(old.buildProfile, "");
}

// A player runs content of a newer minor (its additions are skipped, one warning per version) and
// of an older one; another major is refused, naming the fix
TEST(ContentManifest, ReadsANewerMinorAndRefusesAnotherMajor) {
    present() = {"scenes/3.jscn", "scenes/4.jscn", "prefabs/2.jpfb", "assets/shaders/MESH_x_glsl410.sdat", "content.jpak"};
    const ContentAccess access{&exists, &noSha};
    ContentManifest m = sample();
    const std::string newer = std::to_string(cook::FORMAT_MAJOR) + "." + std::to_string(cook::CONTENT_MINOR + 1);
    m.contentFormatVersion = newer;
    std::vector<std::string> problems;
    EXPECT_TRUE(checkContent(m, "linux-x64-glcore-release", access, false, problems)) << problems.front();
    EXPECT_TRUE(checkContent(m, "linux-x64-glcore-release", access, false, problems)) << problems.front();
    const std::string warning = "content " + newer + " was made by a newer editor; features newer than " + cook::contentFormatVersion() +
                                " are skipped";
    EXPECT_EQ(jasperengine::test::LogCapture::sinceTestStart().count(D_LOG_WARN, warning), 1u) << "one warning per version";

    m.contentFormatVersion = std::to_string(cook::FORMAT_MAJOR) + ".0";
    EXPECT_TRUE(checkContent(m, "linux-x64-glcore-release", access, false, problems)) << problems.front();

    const std::string otherMajor = std::to_string(cook::FORMAT_MAJOR + 1) + ".0";
    m.contentFormatVersion = otherMajor;
    EXPECT_FALSE(checkContent(m, "linux-x64-glcore-release", access, false, problems));
    ASSERT_EQ(problems.size(), 1u);
    EXPECT_NE(problems[0].find("content format " + otherMajor + ", this player reads " + cook::contentFormatVersion() +
                               ": build the game with the editor of this player's version"), std::string::npos) << problems[0];
}

// S34 R3: a player variant runs only content whose modules it has; content of an older editor
// (no modules) needs every module
TEST(ContentManifest, RefusesContentNeedingAModuleThePlayerLacks) {
    ContentManifest m = sample();
    present() = {"scenes/3.jscn", "scenes/4.jscn", "prefabs/2.jpfb", "assets/shaders/MESH_x_glsl410.sdat", "content.jpak"};
    const ContentAccess access{&exists, &noSha};
    const std::vector<std::string> lite = {"physics3d", "physics2d", "navigation", "soundlayer"};
    std::vector<std::string> problems;
    EXPECT_TRUE(checkContent(m, "linux-x64-glcore-release", access, false, problems, &lite)) << problems.front();

    m.modules = {"htmlui", "physics3d"};
    problems.clear();
    EXPECT_FALSE(checkContent(m, "linux-x64-glcore-release", access, false, problems, &lite));
    ASSERT_EQ(problems.size(), 1u);
    EXPECT_NE(problems[0].find("the HTML UI (htmlui)"), std::string::npos) << problems[0];
    EXPECT_EQ(missingModules(m, lite), std::vector<std::string>{"htmlui"});

    // no modules key: everything
    ContentManifest old;
    std::string error;
    nlohmann::json json = sample().toJson();
    json.erase("modules");
    ASSERT_TRUE(ContentManifest::fromJson(json, old, error)) << error;
    EXPECT_FALSE(old.hasModules);
    EXPECT_EQ(missingModules(old, allModules()), std::vector<std::string>{});
    // T2 added noise, T6 voxel, T4 terrainworld, T1 runtime terrain edits, S81 titleart, dialogue-graphs.md
    // dialogue and input scripts, which this lite list leaves out (N11, T4 R55, S81 R51)
    EXPECT_EQ(missingModules(old, lite), (std::vector<std::string>{"htmlui", "video", "runtimeshaders", "noise", "voxel", "terrainworld",
                                                                    "terrainedit", "titleart", "dialogue", "inputscripts"}));
    // and a Compile build (no player id) is not asked
    problems.clear();
    EXPECT_TRUE(checkContent(m, "", access, false, problems, &lite));
}

// T6 R47, AC38: lite3d leaves Voxel Volumes out, so content with one does not start on it (S34 R3)
TEST(ContentManifest, ContentNeedingVoxelRefusesLite3D) {
    ContentManifest m = sample();
    m.modules = {"physics3d", "navigation", "voxel"};
    present() = {"scenes/3.jscn", "scenes/4.jscn", "prefabs/2.jpfb", "assets/shaders/MESH_x_glsl410.sdat", "content.jpak"};
    const ContentAccess access{&exists, &noSha};
    const std::vector<std::string> lite3d = {"physics3d", "navigation"};
    std::vector<std::string> problems;
    EXPECT_FALSE(checkContent(m, "linux-x64-glcore-release-lite3d", access, false, problems, &lite3d));
    ASSERT_FALSE(problems.empty());
    bool named = false;
    for (const std::string& problem : problems) named = named || problem.find("Voxel Volumes (voxel)") != std::string::npos;
    EXPECT_TRUE(named);
    EXPECT_EQ(missingModules(m, lite3d), std::vector<std::string>{"voxel"});
}

// T4 R55, AC36: lite3d leaves Terrain Worlds out, so content with one does not start on it (S34 R3)
TEST(ContentManifest, ContentNeedingTerrainWorldRefusesLite3D) {
    ContentManifest m = sample();
    m.modules = {"physics3d", "navigation", "terrainworld"};
    present() = {"scenes/3.jscn", "scenes/4.jscn", "prefabs/2.jpfb", "assets/shaders/MESH_x_glsl410.sdat", "content.jpak"};
    const ContentAccess access{&exists, &noSha};
    const std::vector<std::string> lite3d = {"physics3d", "navigation"};
    std::vector<std::string> problems;
    EXPECT_FALSE(checkContent(m, "linux-x64-glcore-release-lite3d", access, false, problems, &lite3d));
    bool named = false;
    for (const std::string& problem : problems) named = named || problem.find("Terrain World (terrainworld)") != std::string::npos;
    EXPECT_TRUE(named);
    EXPECT_EQ(missingModules(m, lite3d), std::vector<std::string>{"terrainworld"});
}

// the newest last: T6's Voxel Volumes, T4's Terrain World, then T1's runtime terrain edits
TEST(PlayerModules, KnowsVoxelTerrainWorldAndTerrainEdits) {
    // the terrain programme's three, then Title Art's (S81 R51), dialogue's (dialogue-graphs.md R15) and
    // input scripts' (input-recorder-and-input-scripts.md Deviations), which merged after them
    ASSERT_GE(allModules().size(), 6u);
    EXPECT_EQ(allModules()[allModules().size() - 6], "voxel");
    EXPECT_EQ(allModules()[allModules().size() - 5], "terrainworld");
    EXPECT_EQ(allModules()[allModules().size() - 4], "terrainedit");
    EXPECT_EQ(allModules()[allModules().size() - 3], "titleart");
    EXPECT_EQ(allModules()[allModules().size() - 2], "dialogue");
    EXPECT_EQ(allModules().back(), "inputscripts");
    EXPECT_EQ(moduleDisplayName("dialogue"), "dialogue graphs and the Dialogue Runner");
    EXPECT_TRUE(isModuleBuilt("inputscripts")) << "the tests' engine has every module";
    EXPECT_EQ(moduleDisplayName("voxel"), "Voxel Volumes");
    EXPECT_EQ(moduleDisplayName("terrainworld"), "Terrain World");
    EXPECT_EQ(moduleDisplayName("terrainedit"), "runtime terrain edits");
#if JASPERENGINE_VOXEL
    EXPECT_TRUE(isModuleBuilt("voxel"));
#else
    EXPECT_FALSE(isModuleBuilt("voxel"));
#endif
#if JASPERENGINE_TERRAIN_WORLD
    EXPECT_TRUE(isModuleBuilt("terrainworld"));
#else
    EXPECT_FALSE(isModuleBuilt("terrainworld"));
#endif
#if JASPERENGINE_TERRAIN_EDIT
    EXPECT_TRUE(isModuleBuilt("terrainedit"));
#else
    EXPECT_FALSE(isModuleBuilt("terrainedit"));
#endif
}

TEST(PlayerModules, BuiltModulesFollowTheBuildDefinitions) {
    const std::vector<std::string> built = builtModules();
    for (const std::string& module : built)
        EXPECT_NE(std::find(allModules().begin(), allModules().end(), module), allModules().end()) << module;
    // the unit tests' engine has the HTML UI and its video (engine/CMakeLists.txt refuses tests without them)
    EXPECT_TRUE(isModuleBuilt("htmlui"));
    EXPECT_TRUE(isModuleBuilt("video"));
    EXPECT_FALSE(isModuleBuilt("nosuchmodule"));
    EXPECT_EQ(moduleDisplayName("physics3d"), "3D physics");
}

// S98 REQ-10: a Development Build's left-out assets round-trip; a release build's manifest has no key
TEST(ContentManifest, StrippedListRoundTripsAndIsAbsentWhenEmpty) {
    ContentManifest m = sample();
    EXPECT_FALSE(m.toJson().contains("stripped"));
    m.stripped["textures/b.png"] = "unreferenced";
    m.stripped["dev/debug.png"] = "excluded: assets/dev/**";
    ContentManifest back;
    std::string error;
    ASSERT_TRUE(ContentManifest::parse(m.toJson().dump(2), back, error)) << error;
    EXPECT_EQ(back.stripped, m.stripped);
}

}  // namespace content_manifest_test
