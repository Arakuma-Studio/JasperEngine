// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// A Build Profile's content exclusions (docs/specs/build-profiles.md R10, R11, AC6): Exclude Labels
// against the S25 labels (without case), Exclude Paths as folder prefixes or files, and the paths a
// profile may name.

#include <gtest/gtest.h>

#include "buildprofile/BuildProfile.h"
#include "support/EditorTestSupport.h"

#include <chrono>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::editor;

TEST(BuildContentFilter, ExcludesByLabelAndPath) {
    BuildProfile profile;
    profile.name = "Demo";
    profile.excludeLabels = {"Full-Only"};
    profile.excludePaths = {"music/full/", "levels/bonus.scene", "cheats"};
    const std::map<std::string, std::vector<std::string>> labels = {
        {"art/full/boss.png", {"Hero", "full-only"}},
        {"art/hero.png", {"Hero"}},
    };
    const BuildContentFilter filter(&profile, labels);
    std::string why;
    EXPECT_TRUE(filter.excludes("art/full/boss.png", &why));
    EXPECT_EQ(why, "label Full-Only");
    EXPECT_FALSE(filter.excludes("art/hero.png"));
    EXPECT_TRUE(filter.excludes("music/full/theme.ogg", &why));
    EXPECT_EQ(why, "path music/full/");
    EXPECT_TRUE(filter.excludes("music/full/deep/inner.ogg"));
    EXPECT_TRUE(filter.excludes("music/full"));
    EXPECT_FALSE(filter.excludes("music/fullness.ogg"));
    EXPECT_FALSE(filter.excludes("music/theme.ogg"));
    EXPECT_TRUE(filter.excludes("levels/bonus.scene", &why));
    EXPECT_EQ(why, "path levels/bonus.scene");
    EXPECT_FALSE(filter.excludes("levels/bonus.scene.meta2"));
    // a path without the slash names the file or the folder
    EXPECT_TRUE(filter.excludes("cheats/godmode.lua"));
    EXPECT_TRUE(filter.excludes("cheats"));
    EXPECT_FALSE(filter.excludes("cheatsheet.txt"));
    // Windows separators and a leading ./
    EXPECT_TRUE(filter.excludes("music\\full\\theme.ogg"));
    EXPECT_TRUE(filter.excludes("./music/full/theme.ogg"));

    // no profile, no exclusions: nothing is left out
    const BuildContentFilter none;
    EXPECT_TRUE(none.empty());
    EXPECT_FALSE(none.excludes("music/full/theme.ogg"));
    const BuildContentFilter nothing(nullptr, labels);
    EXPECT_TRUE(nothing.empty());

    // the walk is per path: 10,000 files cost one component walk each
    std::vector<std::string> paths;
    for (int i = 0; i < 10000; i++) paths.push_back("art/set" + std::to_string(i % 50) + "/deep/file" + std::to_string(i) + ".png");
    size_t excluded = 0;
    const auto start = std::chrono::steady_clock::now();
    for (const std::string& path : paths) excluded += filter.excludes(path);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    EXPECT_EQ(excluded, 0u);
    std::printf("[ bench ] content filter over 10,000 paths: %.2f ms\n", ms);
    if (const double slack = editor::test::perfSlack(); slack > 0.0) {
        EXPECT_LT(ms, 20.0 * slack);
    }
}

TEST(BuildContentFilter, RejectsEscapingPaths) {
    EXPECT_EQ(BuildContentFilter::pathRefusal("music/full/"), "");
    EXPECT_EQ(BuildContentFilter::pathRefusal("levels/bonus.scene"), "");
    EXPECT_NE(BuildContentFilter::pathRefusal("../other/"), "");
    EXPECT_NE(BuildContentFilter::pathRefusal("music/../../other/").find("leaves the project"), std::string::npos);
    EXPECT_NE(BuildContentFilter::pathRefusal("/etc/").find("project-relative"), std::string::npos);
    EXPECT_NE(BuildContentFilter::pathRefusal("EditorSettings/BuildProfiles/").find("holds no assets"), std::string::npos);
    EXPECT_NE(BuildContentFilter::pathRefusal(".jasperengine/").find("holds no assets"), std::string::npos);
    EXPECT_NE(BuildContentFilter::pathRefusal("build/").find("holds no assets"), std::string::npos);
    EXPECT_NE(BuildContentFilter::pathRefusal(""), "");
}
