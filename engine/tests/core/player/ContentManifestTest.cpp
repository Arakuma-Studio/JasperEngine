// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// content-manifest.json (docs/specs/S30-shipping.md R21): what the editor writes the player and
// the payload checks read back whole, and the checks a player makes at start.

#include <gtest/gtest.h>

#include "player/ContentManifest.h"
#include "serialization/CookedScene.h"

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
