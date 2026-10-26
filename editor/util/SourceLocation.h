// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// The path-start scan and the ":L:C" / "(L,C)" readers follow OpenEngine's SourceLocation
// (Apps/Editor/Source/ScriptEditor/SourceLocation.cpp): Copyright (c) 2026 Gamecrafters Guild
// Limited and contributors, MIT. Extended to every location of a message and more formats.

#pragma once

// File locations in log text: what the Console turns into links (click-to-source) and the
// MCP build and console tools report. One scanner for every format the editor prints:
//
//   Lua      p.lua:12:  [string "p.lua"]:12:  lua://p.lua:12:  tracebacks "\tp.lua:12: in ..." ([C] skipped)
//   GCC/Clang  f.cpp:12:5:  "In file included from f.h:3"
//   MSVC     f.cpp(12,5):  f.cpp(12):
//   CMake    CMake Error at CMakeLists.txt:12 (add_library)
//   glslang  ERROR: f.frag:12:
//   HTML UI  f.html:12:5      S03 asset issues  f.scene:12:
//
// No regex: a message is scanned once when it is logged.

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace jasperengine::editor {

    struct SourceLocation {
        std::string path;        // as written (lua:// and [string "..."] removed)
        int line = 0;
        int column = 0;          // 0 when the text has none
        std::string kind;        // lua, cpp, cmake, shader, html, asset, file
        size_t begin = 0;        // the location's characters in the text
        size_t end = 0;
    };

    std::vector<SourceLocation> findSourceLocations(std::string_view text);

    // Where locations are looked up: the first existing match wins, in this order
    struct SourceRoots {
        std::filesystem::path project;   // the project root
        std::filesystem::path lua;       // Project Settings > Scripting > Lua directory
        std::filesystem::path assets;
        std::filesystem::path engine;    // the engine sources this editor was built from (read-only)
    };

    struct ResolvedSource {
        std::filesystem::path absolute;
        std::string projectPath;         // relative to the project root, '/' separated; empty outside it
        bool inProject = false;
        bool exists = false;
    };

    // Absolute paths as they are; then the project root, the Lua and assets directories, a
    // build-tree copy's project file (projectFileFor), the engine sources
    ResolvedSource resolveSourceLocation(const std::string& path, const SourceRoots& roots);

    // The project file a build-tree path was copied from: the longest trailing part of the
    // path that exists in the project ("" when none)
    std::string projectFileFor(const std::filesystem::path& projectPath, const std::string& file);

    // "path:line:column" (column left out when 0)
    std::string formatSourceLocation(const std::string& path, int line, int column);

}
