// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The example library embedded in the editor (editor/util/HtmlUiExamples, phase-4 spec 3.7.3,
// 3.7.4): template instantiation and the generated project library,
// writeTo (the project copy: unchanged files left alone, files no longer embedded removed,
// unicode paths, an unwritable target), and instantiate's renames, checked by compiling every
// instantiated template through the compat layer.

#include <gtest/gtest.h>

#include "util/FileUtils.h"
#include "util/HtmlUiExamples.h"

#include "htmlui/compat/CompatApi.h"

#include <chrono>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

#include <unistd.h>

using jasperengine::editor::FileUtils;
using jasperengine::editor::HtmlUiExamples;
namespace hc = jasperengine::htmlui::compat;
namespace fs = std::filesystem;

namespace {

    std::string readAll(const fs::path& path){
        std::ifstream in(path, std::ios::binary);
        std::ostringstream text;
        text << in.rdbuf();
        return text.str();
    }

    void writeAll(const fs::path& path, const std::string& content){
        fs::create_directories(path.parent_path());
        std::ofstream out(path, std::ios::binary);
        out << content;
    }

    // A fresh temporary directory (with a non-ASCII name), removed afterwards
    struct TempDir {
        fs::path path;
        TempDir(){
            static int counter = 0;
            path = fs::temp_directory_path() / FileUtils::pathFromUtf8("html-ui-exámples-☃-" + std::to_string(::getpid()) + "-" + std::to_string(++counter));
            fs::remove_all(path);
            fs::create_directories(path);
        }
        ~TempDir(){
            std::error_code error;
            fs::remove_all(path, error);
        }
    };

    std::map<std::string, std::string> instanceFiles(const HtmlUiExamples::Instance& instance){
        std::map<std::string, std::string> files;
        for (const auto& [path, content] : instance.files)
            files[path] = content;
        return files;
    }

    // the compat layer over an instance's files (project-relative, the assets directory "assets/")
    hc::CheckReport checkInstance(const HtmlUiExamples::Instance& instance, const std::string& htmlRel){
        auto files = std::make_shared<std::map<std::string, std::string>>(instanceFiles(instance));
        hc::CheckOptions options;
        options.verifyWithRmlUi = false;
        options.files.read = [files](const std::string& assetPath, std::string& content){
            auto it = files->find("assets/" + assetPath);
            if (it == files->end())
                return false;
            content = it->second;
            return true;
        };
        options.files.exists = [files](const std::string& assetPath){ return files->count("assets/" + assetPath) > 0; };
        return hc::checkDocument(htmlRel.substr(std::string("assets/").size()), options);
    }

    std::string problemsOf(const hc::CheckReport& report){
        std::string problems;
        for (const hc::Diagnostic& diagnostic : report.diagnostics){
            if (diagnostic.severity != hc::Severity::Info)
                problems += hc::toText(diagnostic) + "\n";
        }
        return problems;
    }

}

TEST(HtmlUiExamples, WriteToMakesAndKeepsTheProjectCopy){
    TempDir temp;
    const fs::path dir = temp.path / ".jasperengine" / "engine-api" / "html-ui-examples";
    std::string error;
    ASSERT_TRUE(HtmlUiExamples::writeTo(dir, error)) << error;
    for (const HtmlUiExamples::File& file : HtmlUiExamples::files())
        EXPECT_EQ(readAll(dir / file.path), std::string(file.data)) << file.path;

    // unchanged files are not written again (no new modification time)
    const fs::path css = dir / "hud" / "hud.css";
    fs::last_write_time(css, fs::file_time_type::clock::now() - std::chrono::hours(24));
    const auto before = fs::last_write_time(css);
    ASSERT_TRUE(HtmlUiExamples::writeTo(dir, error)) << error;
    // ticks: a failing time_point would print through libc++'s chrono formatter (macOS 13.3+ only)
    EXPECT_EQ(fs::last_write_time(css).time_since_epoch().count(), before.time_since_epoch().count());

    // a newer editor: an edited file is restored, a file and a folder it no longer has go
    writeAll(css, "/* edited */");
    writeAll(dir / "old_example" / "old.html", "<p>old</p>");
    writeAll(dir / "hud" / "assets" / "gone.svg", "<svg/>");
    ASSERT_TRUE(HtmlUiExamples::writeTo(dir, error)) << error;
    EXPECT_EQ(readAll(css), std::string(HtmlUiExamples::find("hud/hud.css")->data));
    EXPECT_FALSE(fs::exists(dir / "old_example"));
    EXPECT_FALSE(fs::exists(dir / "hud" / "assets" / "gone.svg"));
    EXPECT_TRUE(fs::exists(dir / "hud" / "assets" / "compass.svg"));
}

TEST(HtmlUiExamples, WriteToReportsAnUnwritableTarget){
    TempDir temp;
    // a file where the folder has to be: nothing can be written under it (whatever the user's rights)
    writeAll(temp.path / "engine-api", "not a folder");
    std::string error;
    EXPECT_FALSE(HtmlUiExamples::writeTo(temp.path / "engine-api" / "html-ui-examples", error));
    EXPECT_NE(error.find("html-ui-examples"), std::string::npos) << error;

    // a folder where a file has to be: that file fails, the others are written
    const fs::path dir = temp.path / "copy";
    fs::create_directories(dir / "hud" / "hud.css");
    error.clear();
    EXPECT_FALSE(HtmlUiExamples::writeTo(dir, error));
    EXPECT_NE(error.find("hud.css"), std::string::npos) << error;
    EXPECT_TRUE(fs::is_regular_file(dir / "main_menu" / "main_menu.html"));
}

TEST(HtmlUiExamples, InstantiateRenamesTheTemplate){
    HtmlUiExamples::Instance instance;
    std::string error;
    ASSERT_TRUE(HtmlUiExamples::instantiate("main_menu", "assets/ui/title_screen.html", "scripts", instance, error)) << error;
    const auto files = instanceFiles(instance);
    std::vector<std::string> paths;
    for (const auto& [path, content] : instance.files)
        paths.push_back(path);
    EXPECT_EQ(paths, (std::vector<std::string>{"assets/ui/title_screen.html", "assets/ui/title_screen.css",
                                               "assets/ui/title_screen_assets/logo.svg", "scripts/TitleScreenUI.lua"}));
    EXPECT_EQ(instance.luaPath, "scripts/TitleScreenUI.lua");
    EXPECT_EQ(instance.luaClass, "TitleScreenUI");

    const std::string& html = files.at("assets/ui/title_screen.html");
    EXPECT_NE(html.find("href=\"title_screen.css\""), std::string::npos);
    EXPECT_EQ(html.find("main_menu.css"), std::string::npos);
    EXPECT_NE(html.find("src=\"title_screen_assets/logo.svg\""), std::string::npos);
    EXPECT_EQ(html.find("\"assets/"), std::string::npos);

    const std::string& lua = files.at("scripts/TitleScreenUI.lua");
    EXPECT_NE(lua.find("---@class TitleScreenUI : ScriptInstance"), std::string::npos);
    EXPECT_NE(lua.find("return TitleScreenUI"), std::string::npos);
    EXPECT_EQ(lua.find("MainMenuUI"), std::string::npos);
    EXPECT_EQ(files.at("assets/ui/title_screen_assets/logo.svg"), std::string(HtmlUiExamples::find("main_menu/assets/logo.svg")->data));

    // the renamed files still compile clean: the stylesheet and the pictures are found
    const hc::CheckReport report = checkInstance(instance, "assets/ui/title_screen.html");
    EXPECT_EQ(problemsOf(report), "");
}

TEST(HtmlUiExamples, InstantiatedTemplatesCompileClean){
    for (const std::string& name : HtmlUiExamples::names()){
        SCOPED_TRACE(name);
        HtmlUiExamples::Instance instance;
        std::string error;
        // a stem unlike the template's, in a folder of its own
        const std::string html = "assets/screens/my " + name + ".htm";
        ASSERT_TRUE(HtmlUiExamples::instantiate(name, html, "lua/ui", instance, error)) << error;
        EXPECT_EQ(problemsOf(checkInstance(instance, html)), "");
        // every asset reference was renamed: no file is outside the document's folder or the Lua folder
        for (const auto& [path, content] : instance.files){
            const bool inScreens = path.compare(0, 15, "assets/screens/") == 0;
            const bool inLua = path == instance.luaPath;
            EXPECT_TRUE(inScreens || inLua) << path;
            if (inScreens && path != html && path.find("_assets/") != std::string::npos){
                EXPECT_EQ(path.compare(0, 15 + 3 + name.size() + 8, "assets/screens/my " + name + "_assets/"), 0) << path;
            }
        }
    }
}

TEST(HtmlUiExamples, InstantiateBlankTakesTheTitle){
    HtmlUiExamples::Instance instance;
    std::string error;
    ASSERT_TRUE(HtmlUiExamples::instantiate("blank", "assets/ui/game_over.html", "", instance, error)) << error;
    const auto files = instanceFiles(instance);
    const std::string& html = files.at("assets/ui/game_over.html");
    EXPECT_NE(html.find("<title>Game Over</title>"), std::string::npos);
    EXPECT_NE(html.find("<h1>Game Over</h1>"), std::string::npos);
    EXPECT_NE(html.find("href=\"game_over.css\""), std::string::npos);
    EXPECT_EQ(instance.luaPath, "GameOverUI.lua");
    EXPECT_NE(files.at("GameOverUI.lua").find("---@class GameOverUI : ScriptInstance"), std::string::npos);
    EXPECT_TRUE(files.count("assets/ui/game_over.css"));
    EXPECT_EQ(files.size(), 3u);
}

TEST(HtmlUiExamples, InstantiateNamesAndCollisions){
    std::string error;
    // the same document twice gives the same files (instantiate writes nothing: the caller keeps existing files)
    HtmlUiExamples::Instance first, second;
    ASSERT_TRUE(HtmlUiExamples::instantiate("hud", "assets/ui/hud.html", "scripts", first, error));
    ASSERT_TRUE(HtmlUiExamples::instantiate("hud", "assets/ui/hud.html", "scripts", second, error));
    EXPECT_EQ(first.files, second.files);
    EXPECT_EQ(first.luaClass, "HudUI");

    // two documents from one template in one folder: their stylesheets, pictures and scripts do not collide
    HtmlUiExamples::Instance a, b;
    ASSERT_TRUE(HtmlUiExamples::instantiate("inventory", "assets/ui/bag.html", "scripts", a, error));
    ASSERT_TRUE(HtmlUiExamples::instantiate("inventory", "assets/ui/chest.html", "scripts", b, error));
    std::set<std::string> pathsA, pathsB;
    for (const auto& file : a.files) pathsA.insert(file.first);
    for (const auto& file : b.files) pathsB.insert(file.first);
    for (const std::string& path : pathsA)
        EXPECT_EQ(pathsB.count(path), 0u) << path;

    // another template at an example's name: the files follow the document, not the template
    HtmlUiExamples::Instance renamed;
    ASSERT_TRUE(HtmlUiExamples::instantiate("shop", "assets/ui/main_menu.html", "scripts", renamed, error));
    EXPECT_EQ(renamed.luaClass, "MainMenuUI");
    EXPECT_TRUE(instanceFiles(renamed).count("assets/ui/main_menu_assets/items/sword.svg"));
    EXPECT_EQ(problemsOf(checkInstance(renamed, "assets/ui/main_menu.html")), "");

    // a unicode stem and folder: the paths keep it, the Lua class is a Lua name
    HtmlUiExamples::Instance unicode;
    ASSERT_TRUE(HtmlUiExamples::instantiate("dialogue", "assets/ui/diálogos/menú_principal.html", "scripts", unicode, error));
    const auto files = instanceFiles(unicode);
    EXPECT_TRUE(files.count("assets/ui/diálogos/menú_principal.css"));
    EXPECT_TRUE(files.count("assets/ui/diálogos/menú_principal_assets/portrait.svg"));
    EXPECT_EQ(unicode.luaPath, "scripts/MenPrincipalUI.lua");
    EXPECT_NE(files.at("assets/ui/diálogos/menú_principal.html").find("src=\"menú_principal_assets/portrait.svg\""), std::string::npos);
    EXPECT_EQ(problemsOf(checkInstance(unicode, "assets/ui/diálogos/menú_principal.html")), "");
}

TEST(HtmlUiExamples, InstantiateRejectsBadArguments){
    HtmlUiExamples::Instance instance;
    std::string error;
    EXPECT_FALSE(HtmlUiExamples::instantiate("credits", "assets/ui/c.html", "scripts", instance, error));
    EXPECT_NE(error.find("unknown template 'credits'"), std::string::npos) << error;
    EXPECT_NE(error.find("main_menu"), std::string::npos) << error;
    EXPECT_FALSE(HtmlUiExamples::instantiate("hud", "assets/ui/hud.css", "scripts", instance, error));
    EXPECT_NE(error.find(".html"), std::string::npos) << error;
    EXPECT_FALSE(HtmlUiExamples::instantiate("hud", "../outside/hud.html", "scripts", instance, error));
    EXPECT_FALSE(HtmlUiExamples::instantiate("hud", "assets/ui/hud.html", "../scripts", instance, error));
    EXPECT_FALSE(HtmlUiExamples::instantiate("hud", "/abs/hud.html", "scripts", instance, error));
    EXPECT_FALSE(HtmlUiExamples::instantiate("hud", "assets/ui/.html", "scripts", instance, error));
    EXPECT_TRUE(instance.files.empty());
    // separators and a leading ./ are normalized
    ASSERT_TRUE(HtmlUiExamples::instantiate("toasts", "./assets\\ui\\toasts.HTML", "scripts/", instance, error)) << error;
    EXPECT_EQ(instance.files.front().first, "assets/ui/toasts.HTML");
    EXPECT_EQ(instance.luaPath, "scripts/ToastsUI.lua");
}
