// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Build Report's file (docs/specs/build-size-tools.md R1, R5, R7, R39): every file listed in
// path order next to today's keys, old reports still read, the same build giving the same bytes,
// and two reports compared by source asset.

#include <gtest/gtest.h>

#include "cook/BuildReport.h"
#include "cook/Reachability.h"
#include "support/EditorTestSupport.h"

#include "TempDir.h"

#include <cstdio>
#include <filesystem>
#include <string>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace fs = std::filesystem;

namespace {

    BuildReport loadFixture(const std::string& name) {
        BuildReport report;
        std::string error;
        EXPECT_TRUE(BuildReport::load(editor::test::fixture("build-report/" + name), report, error)) << error;
        return report;
    }

    // a project-like tree: 40 files of known sizes under assets/ and lua/
    void writeTree(const jasperengine::test::TempDir& dir) {
        for (int i = 0; i < 40; i++) {
            char name[64];
            std::snprintf(name, sizeof(name), "assets/textures/t%02d.png", 39 - i);
            dir.write(name, std::string((size_t)(100 + i * 10), 'x'));
        }
    }

    BuildReportFile file(const std::string& path, const std::string& category, uint64_t bytes, const std::string& source = {}) {
        BuildReportFile out;
        out.path = path;
        out.category = category;
        out.bytes = bytes;
        out.source = source;
        out.asset = source.empty() ? path.substr(path.find('/') + 1) : source;
        return out;
    }

    std::string withoutIdentity(const BuildReport& report) {
        Json json = Json::parse(report.toText());
        json.erase("id");
        json.erase("created");
        json.erase("editor_version");
        return json.dump();
    }

}

TEST(BuildReportTest, FullListAndOptionalKeys) {
    jasperengine::test::TempDir dir;
    writeTree(dir);
    BuildReport report;
    report.platform = "web";
    report.method = "prebuilt";
    report.addTree(dir.path() / "assets", {}, {}, "assets/");
    const Json json = Json::parse(report.toText());
    EXPECT_EQ(json["version"], 1);
    ASSERT_TRUE(json.contains("files"));
    ASSERT_EQ(json["files"].size(), 40u);
    for (size_t i = 1; i < json["files"].size(); i++)
        EXPECT_LT(json["files"][i - 1]["path"].get<std::string>(), json["files"][i]["path"].get<std::string>()) << "path order";
    EXPECT_EQ(json["largest"].size(), BuildReport::kLargest);
    EXPECT_EQ(json["largest"][0]["path"], "assets/textures/t00.png") << "the largest first";
    EXPECT_TRUE(json["reachability"].is_null()) << "no walk ran";

    // a report the base tree wrote: no file list, read as it was
    const BuildReport old = loadFixture("v1-largest-only.json");
    EXPECT_FALSE(old.hasFiles);
    EXPECT_TRUE(old.files.empty());
    EXPECT_FALSE(old.reachability.has_value());
    EXPECT_EQ(old.largest.size(), 6u);
    EXPECT_EQ(old.largest[0].asset, "textures/wall.png") << "a cooked file ships its source";

    // a report with the new keys (and one a later editor added): today's keys read the same
    const BuildReport newer = loadFixture("v1-with-new-keys.json");
    EXPECT_EQ(newer.totalBytes, old.totalBytes);
    EXPECT_EQ(newer.categories, old.categories);
    ASSERT_EQ(newer.largest.size(), old.largest.size());
    for (size_t i = 0; i < old.largest.size(); i++) {
        EXPECT_EQ(newer.largest[i].path, old.largest[i].path);
        EXPECT_EQ(newer.largest[i].bytes, old.largest[i].bytes);
        EXPECT_EQ(newer.largest[i].source, old.largest[i].source);
    }
    EXPECT_TRUE(newer.hasFiles);
    EXPECT_EQ(newer.files.size(), 6u);
    EXPECT_EQ(newer.id, "20261119-142233-source");
    EXPECT_EQ(newer.leftOutBytes(), 8000u);
    ASSERT_TRUE(newer.reachability.has_value());
    EXPECT_EQ(newer.referencedBy("scenes/1.jscn").reason, "reached");
    EXPECT_EQ(newer.referencedBy("assets/je-asset-map.json").reason, "generated");
    EXPECT_EQ(newer.referencedBy("assets/sounds/hit.wav").reason, "unreferenced");
    ASSERT_EQ(newer.budgets.size(), 1u);
    EXPECT_EQ(newer.budgets[0].limitBytes, 1048576u);
}

TEST(BuildReportTest, SameProjectSameReport) {
    jasperengine::test::TempDir dir;
    writeTree(dir);
    dir.write("lua/main.lua", "require('ui.hud')\n");
    dir.write("lua/ui/hud.lua", "return {}\n");
    const std::vector<ReachEdge> edges = {
        {"project.yaml", "scenes/main.scene", "project/scenes[0]", "SceneList", ""},
        {"scenes/main.scene", "textures/t03.png", "entity:4/MeshComponent.submeshes[0].material.baseColorTexture", "SceneComponent", "Root/Wall"},
        {"main.lua", "ui/hud.lua", "text:1", "LuaRequire", ""},
    };
    auto build = [&](const std::string& id) {
        BuildReport report;
        report.platform = "linux";
        report.method = "prebuilt";
        report.id = id;
        report.created = id;
        report.editorVersion = id;
        report.addTree(dir.path() / "assets", {}, {}, "assets/");
        report.addTree(dir.path() / "lua", {}, {}, "lua/");
        for (BuildReportFile& entry : report.files) entry.asset = entry.path.substr(entry.path.find('/') + 1);
        report.leftOut = {{"textures/big.psd", 900, "cooked_texture"}};
        report.reachability = computeReachability(edges, {"project.yaml", "main.lua", "ui/hud.lua"});
        report.budgets = {{"all", "textures", 1048576, 4000}};
        return report;
    };
    const BuildReport first = build("a");
    const BuildReport second = build("b");
    EXPECT_EQ(withoutIdentity(first), withoutIdentity(second));
    EXPECT_NE(first.toText(), second.toText()) << "only the identity differs";
}

TEST(BuildReportTest, CompareBySource) {
    BuildReport base;
    base.platform = "web";
    base.hasFiles = true;
    base.addFile(file("assets/je-cooked/models/aaaa.glb", "models", 1000, "models/crate.glb"));
    base.addFile(file("assets/sounds/hit.wav", "audio", 500));
    base.addFile(file("lua/main.lua", "scripts", 300));
    base.addFile(file("assets/same.txt", "other", 10));
    BuildReport head;
    head.platform = "web";
    head.addFile(file("assets/je-cooked/models/bbbb.glb", "models", 1600, "models/crate.glb"));
    head.addFile(file("assets/textures/new.png", "textures", 200));
    head.addFile(file("lua/main.lua", "scripts", 250));
    head.addFile(file("assets/same.txt", "other", 10));

    const BuildReportDiff diff = compareBuildReports(base, head);
    EXPECT_FALSE(diff.partial);
    EXPECT_FALSE(diff.platformDiffers);
    ASSERT_EQ(diff.changes.size(), 4u);
    EXPECT_EQ(diff.unchanged, 1u);
    // |delta| descending: the model grew 600, the sound went (-500), the texture came (+200), the script shrank (-50)
    EXPECT_EQ(diff.changes[0].key, "models/crate.glb");
    EXPECT_EQ(diff.changes[0].change, "grown") << "one row keyed by the source, not removed + added";
    EXPECT_EQ(diff.changes[0].delta, 600);
    EXPECT_EQ(diff.changes[0].path, "assets/je-cooked/models/bbbb.glb");
    EXPECT_EQ(diff.changes[1].change, "removed");
    EXPECT_EQ(diff.changes[1].delta, -500);
    EXPECT_EQ(diff.changes[2].change, "added");
    EXPECT_EQ(diff.changes[2].key, "assets/textures/new.png");
    EXPECT_EQ(diff.changes[3].change, "shrunk");
    EXPECT_EQ(diff.grown, 1u);
    EXPECT_EQ(diff.added, 1u);
    EXPECT_EQ(diff.removed, 1u);
    EXPECT_EQ(diff.shrunk, 1u);
    EXPECT_EQ(diff.categories.at("models").first, 1000u);
    EXPECT_EQ(diff.categories.at("models").second, 1600u);
    EXPECT_EQ(diff.totalBefore, 1810u);
    EXPECT_EQ(diff.totalAfter, 2060u);

    // a base from before the file list: its largest only, marked partial
    const BuildReport old = loadFixture("v1-largest-only.json");
    BuildReport web = head;
    web.platform = "source";
    const BuildReportDiff partial = compareBuildReports(old, web);
    EXPECT_TRUE(partial.partial);
    for (const BuildReportChange& change : partial.changes) EXPECT_NE(change.change, "added") << change.key;

    // two platforms
    BuildReport otherPlatform = head;
    otherPlatform.platform = "linux";
    EXPECT_TRUE(compareBuildReports(base, otherPlatform).platformDiffers);
}
