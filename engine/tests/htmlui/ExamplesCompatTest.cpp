// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The HTML/CSS UI example library (docs/mcp/guide/html-ui-examples, phase-4 spec 3.7, 5.1):
// every example and the blank template compile with zero warnings and zero errors at every
// viewport its manifest entry lists, RmlUi accepting the produced RCSS (verifyWithRmlUi), and
// read only files of the library. The files are read in place through a FileAccess on the
// source tree, as `check --ui` reads a project.

#include "htmlui/compat/CompatApi.h"

#include "TestSystem.h"

#include "json.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace hc = jasperengine::htmlui::compat;
namespace fs = std::filesystem;

namespace {

    fs::path examplesRoot(){
        return fs::path(JASPER_HTMLUI_TEST_SOURCE_DIR) / ".." / ".." / ".." / "docs" / "mcp" / "guide" / "html-ui-examples";
    }

    bool readFile(const fs::path& path, std::string& content){
        std::ifstream in(path, std::ios::binary);
        if (!in)
            return false;
        std::ostringstream text;
        text << in.rdbuf();
        content = text.str();
        return true;
    }

    // asset paths are relative to the library root ("hud/hud.css"), never outside it
    fs::path fileOf(const std::string& assetPath){
        const fs::path relative = fs::path(assetPath).lexically_normal();
        if (relative.empty() || relative.is_absolute() || *relative.begin() == "..")
            return {};
        return examplesRoot() / relative;
    }

    hc::FileAccess libraryFiles(){
        hc::FileAccess files;
        files.read = [](const std::string& assetPath, std::string& content){
            const fs::path file = fileOf(assetPath);
            std::error_code error;
            return !file.empty() && fs::is_regular_file(file, error) && readFile(file, content);
        };
        files.exists = [](const std::string& assetPath){
            const fs::path file = fileOf(assetPath);
            std::error_code error;
            return !file.empty() && fs::is_regular_file(file, error);
        };
        return files;
    }

    nlohmann::json manifest(){
        std::string text;
        if (!readFile(examplesRoot() / "examples.json", text))
            return nlohmann::json();
        return nlohmann::json::parse(text, nullptr, false);
    }

}

TEST(ExamplesCompat, ManifestListsTheNineExamplesAndBlank){
    const nlohmann::json json = manifest();
    ASSERT_TRUE(json.is_object()) << "examples.json is missing or does not parse";
    ASSERT_TRUE(json["examples"].is_array());
    std::vector<std::string> names;
    for (const nlohmann::json& entry : json["examples"])
        names.push_back(entry.value("name", ""));
    EXPECT_EQ(names, (std::vector<std::string>{"main_menu", "pause_menu", "hud", "inventory", "settings", "dialogue", "shop",
                                               "toasts", "loading", "video_menu", "blank"}));
}

TEST(ExamplesCompat, EveryExampleIsCleanAtEveryViewport){
    const nlohmann::json json = manifest();
    ASSERT_TRUE(json.is_object() && json["examples"].is_array());
    // RmlUi (verifyWithRmlUi) opens the pictures through the engine's files: the library is the asset root
    struct AssetRoot {
        AssetRoot(){ jasperengine::test::TestSystem::get().setAssetPath(examplesRoot().lexically_normal().string()); }
        ~AssetRoot(){ jasperengine::test::TestSystem::get().setAssetPath(""); }
    } assetRoot;
    int checked = 0;
    for (const nlohmann::json& entry : json["examples"]){
        const std::string name = entry.value("name", "");
        const std::string html = name + "/" + entry.value("html", "");
        SCOPED_TRACE(html);
        ASSERT_TRUE(fs::is_regular_file(fileOf(html)));

        std::vector<std::pair<int, int>> viewports;
        for (const nlohmann::json& size : entry["viewports"])
            viewports.emplace_back(size[0].get<int>(), size[1].get<int>());
        ASSERT_FALSE(viewports.empty());

        for (const auto& [width, height] : viewports){
            SCOPED_TRACE(std::to_string(width) + "x" + std::to_string(height));
            hc::CheckOptions options;
            options.files = libraryFiles();
            options.verifyWithRmlUi = true;
            options.compat.viewport = hc::CompatSize{width, height};
            const hc::CheckReport report = hc::checkDocument(html, options);

            std::string problems;
            for (const hc::Diagnostic& diagnostic : report.diagnostics){
                if (diagnostic.severity != hc::Severity::Info)
                    problems += hc::toText(diagnostic) + "\n";
            }
            EXPECT_EQ(problems, "") << "warnings or errors in " << html;
            EXPECT_TRUE(report.remoteRefs.empty()) << "an example references a remote URL";

            // it read its stylesheet and nothing outside its own folder
            const std::string css = name + "/" + entry.value("css", "");
            EXPECT_NE(std::find(report.files.begin(), report.files.end(), css), report.files.end()) << css << " was not read";
            for (const std::string& file : report.files)
                EXPECT_EQ(file.compare(0, name.size() + 1, name + "/"), 0) << html << " read " << file;
        }

        // every asset the manifest lists exists
        for (const nlohmann::json& asset : entry["assets"])
            EXPECT_TRUE(fs::is_regular_file(fileOf(name + "/" + asset.get<std::string>()))) << asset;
        checked++;
    }
    EXPECT_EQ(checked, 10);
}
