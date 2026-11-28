// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Patch And Run's development content folder (docs/specs/build-and-run-on-device.md R7, AC6): a
// development player runs from je-dev-content when its manifest is this player's, warns once and
// keeps its packaged content for another player's, never looks with an explicit --content, and a
// release player compiles the check to nothing.

#include <gtest/gtest.h>

#include "player/ContentManifest.h"
#include "player/DevelopmentContent.h"

#include <map>
#include <string>
#include <vector>

namespace development_content_test {

using namespace jasperengine;
using namespace jasperengine::player;


    const std::string kFolder = "/data/user/0/com.example.game/files/je-dev-content";
    const std::string kPlayer = "android-universal-gles3-development";
    const std::string kPlayerSha = std::string(64, 'a');

    // an app data folder in memory, counting what the check touches
    struct FakeData {
        std::map<std::string, std::string> files;
        int stats = 0;
        int reads = 0;
        int packagedReads = 0;
        std::vector<std::string> warnings;

        DevelopmentContentAccess access() {
            DevelopmentContentAccess a;
            a.exists = [this](const std::string& path) {
                stats++;
                return files.count(path) > 0;
            };
            a.read = [this](const std::string& path, std::string& text) {
                reads++;
                const auto it = files.find(path);
                if (it == files.end()) return false;
                text = it->second;
                return true;
            };
            a.warn = [this](const std::string& message) { warnings.push_back(message); };
            return a;
        }
        std::function<std::string()> packagedSha() {
            return [this]() {
                packagedReads++;
                return kPlayerSha;
            };
        }
    };

    std::string manifestFor(const std::string& playerId, const std::string& playerSha, int patch) {
        ContentManifest m;
        m.engineVersion = "v1.0.0";
        m.playerId = playerId;
        m.playerSha256 = playerSha;
        m.method = "prebuilt";
        m.patch = patch;
        m.patchedAt = "2026-11-20T10:00:00Z";
        return m.toJson().dump(2);
    }

}

namespace development_content_test {

using namespace development_content_test;

TEST(DevelopmentContent, UsedWhenManifestMatchesThePlayer) {
    FakeData data;
    data.files[kFolder + "/content-manifest.json"] = manifestFor(kPlayer, kPlayerSha, 3);
    const auto found = findDevelopmentContentFor<true>(kFolder, false, kPlayer, data.packagedSha(), data.access());
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->root, kFolder);
    EXPECT_EQ(found->patch, 3);
    EXPECT_TRUE(data.warnings.empty());

    // no folder (no patch ever pushed): one stat, nothing read, packaged content
    FakeData empty;
    EXPECT_FALSE(findDevelopmentContentFor<true>(kFolder, false, kPlayer, empty.packagedSha(), empty.access()).has_value());
    EXPECT_EQ(empty.stats, 1);
    EXPECT_EQ(empty.reads, 0);
    EXPECT_EQ(empty.packagedReads, 0);
    EXPECT_TRUE(empty.warnings.empty());
}

TEST(DevelopmentContent, IgnoredForAnotherPlayer) {
    // another runtime id, then the same id of another player binary, then a broken manifest
    const std::vector<std::string> manifests = {manifestFor("android-universal-vulkan-development", kPlayerSha, 2),
                                                manifestFor(kPlayer, std::string(64, 'b'), 2), "{not json"};
    for (const std::string& manifest : manifests) {
        FakeData data;
        data.files[kFolder + "/content-manifest.json"] = manifest;
        EXPECT_FALSE(findDevelopmentContentFor<true>(kFolder, false, kPlayer, data.packagedSha(), data.access()).has_value()) << manifest;
        ASSERT_EQ(data.warnings.size(), 1u) << manifest;
        EXPECT_EQ(data.warnings[0], "development content in je-dev-content is for another player; using the packaged content");
    }
}

TEST(DevelopmentContent, ExplicitContentOptionWins) {
    FakeData data;
    data.files[kFolder + "/content-manifest.json"] = manifestFor(kPlayer, kPlayerSha, 1);
    EXPECT_FALSE(findDevelopmentContentFor<true>(kFolder, true, kPlayer, data.packagedSha(), data.access()).has_value());
    EXPECT_EQ(data.stats, 0);
    EXPECT_EQ(data.reads, 0);
    EXPECT_TRUE(data.warnings.empty());
}

// A release player (JASPERENGINE_DEVELOPMENT_BUILD 0) instantiates the gate with false: a constant
// nothing that touches no file, even beside a matching folder
TEST(DevelopmentContent, ReleaseNeverLooks) {
    FakeData data;
    data.files[kFolder + "/content-manifest.json"] = manifestFor(kPlayer, kPlayerSha, 1);
    EXPECT_FALSE(findDevelopmentContentFor<false>(kFolder, false, kPlayer, data.packagedSha(), data.access()).has_value());
    EXPECT_EQ(data.stats, 0);
    EXPECT_EQ(data.reads, 0);
    EXPECT_EQ(data.packagedReads, 0);
    // the player's own gate follows the build's flag
#if defined(JASPERENGINE_DEVELOPMENT_BUILD) && JASPERENGINE_DEVELOPMENT_BUILD
    static_assert(DEVELOPMENT_CONTENT_CHECKED);
#else
    static_assert(!DEVELOPMENT_CONTENT_CHECKED);
#endif
}

TEST(DevelopmentContent, ManifestPatchFieldsRoundTripAndDefault) {
    ContentManifest m;
    m.patch = 4;
    m.patchedAt = "2026-11-20T10:00:00Z";
    ContentManifest back;
    std::string error;
    ASSERT_TRUE(ContentManifest::parse(m.toJson().dump(), back, error)) << error;
    EXPECT_EQ(back.patch, 4);
    EXPECT_EQ(back.patchedAt, "2026-11-20T10:00:00Z");
    // packaged content: absent keys, 0 and none
    ContentManifest packaged;
    EXPECT_FALSE(packaged.toJson().contains("patch"));
    EXPECT_FALSE(packaged.toJson().contains("patchedAt"));
    ASSERT_TRUE(ContentManifest::parse(packaged.toJson().dump(), back, error)) << error;
    EXPECT_EQ(back.patch, 0);
    EXPECT_EQ(back.patchedAt, "");
}

}  // namespace development_content_test
