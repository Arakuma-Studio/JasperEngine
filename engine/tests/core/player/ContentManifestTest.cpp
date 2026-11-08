// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// content-manifest.json (docs/specs/S30-shipping.md R21): what the editor writes the player and
// the payload checks read back whole, and the checks a player makes at start.

#include <gtest/gtest.h>

#include "LogCapture.h"

#include "player/ContentManifest.h"
#include "player/PlayerModules.h"
#include "serialization/CookedScene.h"

#include <algorithm>
#include <set>

using namespace jasperengine;
using namespace jasperengine::player;

namespace content_manifest_test {

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
}

// A player runs content of a newer minor (its additions are skipped, one warning per version) and
// of an older one; another major is refused, naming the fix
TEST(ContentManifest, ReadsANewerMinorAndRefusesAnotherMajor) {
    present() = {"scenes/3.jscn", "scenes/4.jscn", "prefabs/2.jpfb", "assets/shaders/MESH_x_glsl410.sdat", "content.jpak"};
    const ContentAccess access{&exists, &noSha};
    ContentManifest m = sample();
    const std::string newer = std::to_string(cook::FORMAT_MAJOR) + "." + std::to_string(cook::FORMAT_MINOR + 1);
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
    EXPECT_EQ(missingModules(old, lite), (std::vector<std::string>{"htmlui", "video", "runtimeshaders"}));
    // and a Compile build (no player id) is not asked
    problems.clear();
    EXPECT_TRUE(checkContent(m, "", access, false, problems, &lite));
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
