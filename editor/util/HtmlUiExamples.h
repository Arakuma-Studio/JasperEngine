// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "json.hpp"

#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace jasperengine::editor {

    // The HTML/CSS UI example library (docs/mcp/guide/html-ui-examples, phase-4 spec 3.7):
    // Game UI and motion-graphics examples plus the blank template, embedded at build time
    // (CMakeLists.txt generates html_ui_examples.h). Written into every project's
    // .jasperengine/engine-api/html-ui-examples/ (Generator::writeEditorTooling), served as
    // MCP resources and instantiated by create_ui_document. No ImGui or project state.
    class HtmlUiExamples {
    public:
        struct File {
            std::string path;        // relative to the examples root ("main_menu/main_menu.css")
            std::string_view data;
        };
        static const std::vector<File>& files();
        // The embedded file at `path` (relative to the examples root); nullptr when there is none
        static const File* find(const std::string& path);
        // examples.json (an empty object if it does not parse, which a test rules out)
        static const nlohmann::json& manifest();
        // The template names in manifest order, the examples first and blank last
        static std::vector<std::string> names();
        static bool has(const std::string& name);

        // Writes the whole tree under dir: only files whose content differs are written
        // (FileUtils::writeFile), files and folders no longer embedded are removed.
        static bool writeTo(const std::filesystem::path& dir, std::string& error);

        struct Instance {
            std::vector<std::pair<std::string, std::string>> files;   // project-relative path -> content
            std::string luaPath;     // "" when the template has no Lua
            std::string luaClass;
        };
        // The files of template `name` for a document at htmlRel (project-relative, .html or
        // .htm), with its Lua in luaDirRel (project-relative, "" = the project root). Pure: it
        // writes nothing. Renames (spec 3.7.4), with stem = the html file name without its
        // extension and dir = its folder:
        //   <n>/<n>.html      -> htmlRel           href="<n>.css" -> href="<stem>.css"; "assets/ and 'assets/ -> <stem>_assets/
        //   <n>/<n>.css       -> dir/<stem>.css    url(assets/, url("assets/, url('assets/ -> <stem>_assets/
        //   <n>/assets/**     -> dir/<stem>_assets/**
        //   <n>/<Class>UI.lua -> luaDir/<Pascal(stem)>UI.lua   whole-word <Class>UI renamed; "assets/ -> "<stem>_assets/
        // blank also takes the stem in title case as its <title> and heading.
        static bool instantiate(const std::string& name, const std::string& htmlRel, const std::string& luaDirRel,
                                Instance& out, std::string& error);

        // "main_menu" -> "MainMenu": the words of the stem (split at '_', '-', spaces and every
        // character a Lua name cannot hold) capitalized and joined; "Document" when nothing is
        // left, "Ui" in front when it would start with a digit. Always a valid Lua name.
        static std::string pascalName(const std::string& stem);
        // "main_menu" -> "Main Menu" (split at '_', '-' and spaces; other characters kept)
        static std::string titleName(const std::string& stem);
    };

}
