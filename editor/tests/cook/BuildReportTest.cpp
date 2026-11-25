// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Build Report's file (docs/specs/build-size-tools.md R1, R5, R7, R39): every file listed in
// path order next to today's keys, old reports still read, the same build giving the same bytes,
// and two reports compared by source asset.

// Spec S99 R10, R11 (build size for agents): the web download estimate counts what a browser
// fetches, gzip -9 per file; the Download Budget's family per platform; W-BUILD-OVER-BUDGET's
// message names the budget, the actual size and the three largest categories; the budget is a
// Project Settings row stored in project.yaml (absent: off) and edited as one undo step.

#include <gtest/gtest.h>

#include "Project.h"
#include "command/type/ProjectSettingsValuesCmd.h"
#include "cook/BuildReport.h"
#include "cook/Reachability.h"
#include "support/EditorTestSupport.h"

#include "TempDir.h"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace BuildReportTest_cpp {

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

    void buildReportWrite(const fs::path& file, const std::string& bytes) {
        fs::create_directories(file.parent_path());
        std::ofstream(file, std::ios::binary) << bytes;
    }

    // text that compresses like code, longer than one read of the estimate (1 MiB)
    std::string buildReportCompressible(size_t size) {
        std::string text;
        uint32_t state = 12345;
        while (text.size() < size) {
            state = state * 1103515245u + 12345u;
            text += "function f" + std::to_string((state >> 16) % 97) + "() { return " + std::to_string((state >> 8) % 1000) + "; }\n";
        }
        text.resize(size);
        return text;
    }

    std::string buildReportReadText(const fs::path& path) {
        std::ifstream in(path, std::ios::binary);
        std::stringstream text;
        text << in.rdbuf();
        return text.str();
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
    for (size_t i = 1; i < json["files"].size(); i++) {
        EXPECT_LT(json["files"][i - 1]["path"].get<std::string>(), json["files"][i]["path"].get<std::string>()) << "path order";
    }
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
    for (const BuildReportFile& entry : newer.files)
        if (entry.path == "assets/je-cooked/astc/1f.ktx2") { EXPECT_EQ(entry.category, "textures") << "a file without category takes its path's"; }
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
    for (const BuildReportChange& change : partial.changes) { EXPECT_NE(change.change, "added") << change.key; }

    // two platforms
    BuildReport otherPlatform = head;
    otherPlatform.platform = "linux";
    EXPECT_TRUE(compareBuildReports(base, otherPlatform).platformDiffers);
}

TEST(BuildReportTest, TextRoundTrips) {
    // the writer writes text straight and the reader streams it: what one writes the other reads back
    BuildReport report;
    report.platform = "web";
    report.method = "prebuilt";
    report.buildProfile = "Demo \"quoted\"";
    report.id = "20261119-142233-prebuilt";
    report.created = "2026-11-19T14:22:33Z";
    report.editorVersion = "1.2.3";
    report.addFile(file("assets/textures/a\tb.png", "textures", 300));
    report.addFile(file("assets/je-cooked/models/aa.glb", "models", 200, "models/crate.glb"));
    BuildReportFile scene = file("scenes/3.jscn", "scenes", 50);
    scene.asset = "scenes/Main.scene";
    report.addFile(scene);
    BuildReportFile map = file("assets/je-asset-map.json", "other", 10);
    map.asset.clear();
    map.origin = "generated";
    report.addFile(map);
    BuildReportFile odd = file("assets/notes.wav", "other", 5);   // a category its path would not give
    report.addFile(odd);
    report.leftOut = {{"textures/big.psd", 900, "cooked_texture"}};
    report.budgets = {{"all", "textures", 1048576, 4000}};
    report.reachability = std::map<std::string, BuildReportReach>{
        {"project.yaml", {"", "", "root", ""}},
        {"scenes/Main.scene", {"project.yaml", "project/scenes[0].filepath", "scene_list", ""}},
        {"textures/a\tb.png", {"scenes/Main.scene", "entity:4/SpriteComponent.texture", "scene_component", "Root/\u00c9cran"}}};
    const std::string text = report.toText();
    ASSERT_FALSE(Json::parse(text, nullptr, false).is_discarded()) << text;
    BuildReport read;
    ASSERT_TRUE(BuildReport::parse(text, read));
    EXPECT_EQ(read.toText(), text);
    EXPECT_EQ(read.buildProfile, report.buildProfile);
    ASSERT_EQ(read.files.size(), 5u);
    for (const BuildReportFile& entry : read.files) {
        if (entry.path == "scenes/3.jscn") { EXPECT_EQ(entry.asset, "scenes/Main.scene"); }
        if (entry.path == "assets/je-asset-map.json") { EXPECT_EQ(entry.origin, "generated"); }
        if (entry.path == "assets/notes.wav") { EXPECT_EQ(entry.category, "other"); }
        if (entry.path == "assets/je-cooked/models/aa.glb") { EXPECT_EQ(entry.asset, "models/crate.glb"); }
    }
    EXPECT_EQ(read.reachability->at("textures/a\tb.png").object, "Root/\u00c9cran");
    EXPECT_EQ(read.leftOut.size(), 1u);
    EXPECT_EQ(read.budgets.size(), 1u);
    // a report without a file list, and text that is no report
    BuildReport old;
    ASSERT_TRUE(BuildReport::parse(editor::test::readText(editor::test::fixture("build-report/v1-largest-only.json")), old));
    EXPECT_FALSE(old.hasFiles);
    // for a comparison: the files and totals, nothing of reachability, left-out sources or budgets
    BuildReport compared;
    ASSERT_TRUE(BuildReport::parse(text, compared, true));
    EXPECT_EQ(compared.files.size(), 5u);
    EXPECT_EQ(compared.totalBytes, report.totalBytes);
    EXPECT_FALSE(compared.reachability.has_value());
    EXPECT_TRUE(compared.leftOut.empty());
    EXPECT_TRUE(compared.budgets.empty());
    EXPECT_TRUE(BuildReport::parse("{\"version\": 1, \"x\": [\"a\\\"]\", {\"y\": [1, 2.5e3, -4, null, true]}]}", read))
        << "an unknown value of any shape is skipped";
    EXPECT_FALSE(BuildReport::parse("{\"version\": 2}", read));
    EXPECT_FALSE(BuildReport::parse("[1, 2]", read));
    EXPECT_FALSE(BuildReport::parse("{\"version\": 1", read));
}

TEST(BuildReport, WebDownloadIsWhatTheBrowserFetches) {
    for (const char* fetched : {"index.html", "player-loader.js", "jasperengine-player-st.js", "jasperengine-player-st.wasm", "content.jpak",
                                "favicon.png", "plugins/poki.js", "Game.js", "Game.wasm", "Game.data", "plugins/license-check.js"})
        EXPECT_TRUE(BuildReport::isWebDownload(fetched)) << fetched;
    for (const char* skipped : {"build-report.json", "content-manifest.json", "_headers", "licenses/lua-LICENSE", "THIRD-PARTY-NOTICES.txt",
                                "sokol-LICENSE", "LICENSE.txt", "threads/Game.wasm", "symbols/Game.debug", "jasperengine-player-mt.js",
                                "jasperengine-player-mt.wasm", "jasperengine-player-mt.worker.js", "coi-serviceworker.js"})
        EXPECT_FALSE(BuildReport::isWebDownload(skipped)) << skipped;
}

TEST(BuildReport, WebDownloadGzipIsGzipNineOfTheFetchedFiles) {
    jasperengine::test::TempDir dir;
    const std::string wasm = buildReportCompressible(1024 * 1024 + 17);
    const std::string page = "<!doctype html><html><body><canvas></canvas><script src=\"player-loader.js\"></script></body></html>\n";
    buildReportWrite(dir.path() / "jasperengine-player-st.wasm", wasm);
    buildReportWrite(dir.path() / "index.html", page);
    buildReportWrite(dir.path() / "empty.js", "");
    const uint64_t estimate = BuildReport::webDownloadGzipBytes(dir.path());
    // Python's gzip.compress(data, 9) (zlib) of the same three files: 121999 + 95 + 20 bytes. The
    // estimate's deflate is miniz's level 9, streamed in 1 MiB reads: close, not byte-identical
    EXPECT_NEAR((double)estimate, 122114.0, 122114.0 * 0.02);

    // none of these reach the browser
    buildReportWrite(dir.path() / "licenses" / "lua-LICENSE", buildReportCompressible(50000));
    buildReportWrite(dir.path() / "jasperengine-player-mt.wasm", wasm);
    buildReportWrite(dir.path() / BuildReport::kFileName, "{}");
    EXPECT_EQ(BuildReport::webDownloadGzipBytes(dir.path()), estimate);
    EXPECT_EQ(BuildReport::webDownloadGzipBytes(dir.path() / "missing"), 0u);
}

TEST(BuildReport, BudgetFamilyFollowsThePlatform) {
    ModelFamily family = ModelFamily::Desktop;
    ASSERT_TRUE(BuildReport::budgetFamilyOf("web", family));
    EXPECT_EQ(family, ModelFamily::Web);
    for (const char* mobile : {"android", "ios"}) {
        ASSERT_TRUE(BuildReport::budgetFamilyOf(mobile, family)) << mobile;
        EXPECT_EQ(family, ModelFamily::Mobile) << mobile;
    }
    for (const char* desktop : {"windows", "linux", "macos", "desktop"}) {
        ASSERT_TRUE(BuildReport::budgetFamilyOf(desktop, family)) << desktop;
        EXPECT_EQ(family, ModelFamily::Desktop) << desktop;
    }
    EXPECT_FALSE(BuildReport::budgetFamilyOf("source", family)) << "a source export downloads nothing";
    EXPECT_EQ(BuildReport::budgetBytesOf(0.001), 1049u) << "MB are MiB, as the report prints them";
    EXPECT_EQ(BuildReport::budgetBytesOf(10.0), 10u * 1048576u);
    EXPECT_EQ(BuildReport::budgetBytesOf(0.0), 0u);
    EXPECT_EQ(BuildReport::budgetBytesOf(-1.0), 0u);
}

TEST(BuildReport, OverBudgetMessageNamesBudgetActualAndLargestCategories) {
    BuildReport web;
    web.platform = "web";
    web.totalBytes = 20000;
    web.downloadBytesGzip = 5000;
    web.categories = {{"textures", 3000}, {"engine", 9000}, {"models", 1000}, {"scripts", 10}, {"audio", 0}};
    EXPECT_EQ(web.budgetedBytes(), 5000u) << "the web is held to its download";
    const std::string message = web.overBudgetMessage(BuildReport::budgetBytesOf(0.001));
    EXPECT_NE(message.find("downloads 4.9 KB (gzip estimate)"), std::string::npos) << message;
    EXPECT_NE(message.find("Download Budget of 1.0 KB (player.download_budget_mb.web)"), std::string::npos) << message;
    EXPECT_NE(message.find("largest categories: engine 8.8 KB, textures 2.9 KB, models 1000 B"), std::string::npos) << message;
    EXPECT_EQ(message.find("scripts"), std::string::npos) << "three categories only: " << message;
    EXPECT_EQ(web.overBudgetMessage(5000), "") << "at the budget is within it";
    EXPECT_EQ(web.overBudgetMessage(0), "") << "0 is off";

    BuildReport desktop = web;
    desktop.platform = "linux";
    desktop.downloadBytesGzip = 0;
    EXPECT_EQ(desktop.budgetedBytes(), 20000u) << "a desktop build is held to its bytes";
    EXPECT_NE(desktop.overBudgetMessage(10000).find("the linux build weighs 19.5 KB, over its Download Budget of 9.8 KB "
                                                  "(player.download_budget_mb.desktop)"), std::string::npos) << desktop.overBudgetMessage(10000);
    BuildReport source = desktop;
    source.platform = "source";
    EXPECT_EQ(source.overBudgetMessage(1), "");

    BuildReport loaded;
    ASSERT_TRUE(BuildReport::parse(web.toText(), loaded)) << "the report's text keeps the estimate";
    EXPECT_EQ(loaded.downloadBytesGzip, 5000u);
}

TEST(BuildReport, DownloadBudgetIsAnUndoableSettingInProjectYaml) {
    jasperengine::test::TempDir dir;
    const fs::path root = dir.path();
    fs::create_directories(root / "assets");
    std::ofstream(root / "project.yaml") << "name: budget\nassetsDir: assets\n";

    Project project;
    project.setSkipProjectLock(true);
    ASSERT_TRUE(project.loadProject(root, false));
    for (size_t f = 0; f < kModelFamilyCount; f++) EXPECT_EQ(project.getDownloadBudgetMB((ModelFamily)f), 0.0) << "an old project has none";

    auto* command = new ProjectSettingsValuesCmd(&project, {{"player.download_budget_mb.web", 12.5}, {"player.download_budget_mb.mobile", 150}},
                                                 "Change Download Budget");
    command->setNoMerge();
    ASSERT_TRUE(project.getProjectCommandHistory()->addCommand(command));
    EXPECT_EQ(project.getDownloadBudgetMB(ModelFamily::Web), 12.5);
    EXPECT_EQ(project.getDownloadBudgetMB(ModelFamily::Mobile), 150.0);
    const YAML::Node saved = YAML::Load(buildReportReadText(root / "project.yaml"));
    ASSERT_TRUE(saved["export"] && saved["export"]["downloadBudgetMB"]) << buildReportReadText(root / "project.yaml");
    EXPECT_EQ(saved["export"]["downloadBudgetMB"]["web"].as<double>(), 12.5);
    EXPECT_FALSE(saved["export"]["downloadBudgetMB"]["desktop"]) << "a family without a budget writes no key";

    ASSERT_TRUE(project.getProjectCommandHistory()->undo());
    EXPECT_EQ(project.getDownloadBudgetMB(ModelFamily::Web), 0.0);
    EXPECT_EQ(project.getDownloadBudgetMB(ModelFamily::Mobile), 0.0);
    const YAML::Node undone = YAML::Load(buildReportReadText(root / "project.yaml"));
    EXPECT_FALSE(undone["export"] && undone["export"]["downloadBudgetMB"]) << "off again: the key is gone";

    ProjectSettingsValuesCmd negative(&project, {{"player.download_budget_mb.desktop", -1}}, "Change Download Budget");
    EXPECT_FALSE(negative.execute()) << "a budget is 0 (off) or more";

    // a hand-edited file: a negative or non-numeric value is off
    std::ofstream(root / "project.yaml") << "name: budget\nassetsDir: assets\nexport:\n  downloadBudgetMB: {web: 20, mobile: -3, desktop: big}\n";
    Project reloaded;
    reloaded.setSkipProjectLock(true);
    ASSERT_TRUE(reloaded.loadProject(root, false));
    EXPECT_EQ(reloaded.getDownloadBudgetMB(ModelFamily::Web), 20.0);
    EXPECT_EQ(reloaded.getDownloadBudgetMB(ModelFamily::Mobile), 0.0);
    EXPECT_EQ(reloaded.getDownloadBudgetMB(ModelFamily::Desktop), 0.0);
}

}  // namespace BuildReportTest_cpp
