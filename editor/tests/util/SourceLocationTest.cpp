// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Console click-to-source (S23 R27, R37): every file location in real log lines of every
// format the editor prints, the build-tree mapping, and the insert-time budget.

#include "util/SourceLocation.h"
#include "TempDir.h"

#include "support/EditorTestSupport.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

using namespace jasperengine::editor;

namespace {

    struct Expected {
        std::string path;
        int line;
        int column;
    };

    struct Case {
        const char* text;
        std::vector<Expected> locations;
    };

    std::string describe(const std::vector<SourceLocation>& found) {
        std::string out;
        for (const SourceLocation& l : found) out += "{" + l.path + ":" + std::to_string(l.line) + ":" + std::to_string(l.column) + "} ";
        return out;
    }

    const std::vector<Case>& table() {
        static const std::vector<Case> cases = {
            // Lua: both chunk name forms, lua://, tracebacks
            {"scripts/player.lua:12: attempt to index a nil value (field 'body')", {{"scripts/player.lua", 12, 0}}},
            {"Lua Error: scripts/player.lua:12: attempt to call a nil value (method 'jump')", {{"scripts/player.lua", 12, 0}}},
            {"[string \"scripts/player.lua\"]:7: unexpected symbol near 'end'", {{"scripts/player.lua", 7, 0}}},
            {"Failed to load Lua file 'enemy.lua': [string \"enemy.lua\"]:3: '=' expected near 'x'", {{"enemy.lua", 3, 0}}},
            {"lua://lib/util.lua:44: bad argument #1 to 'insert'", {{"lib/util.lua", 44, 0}}},
            {"\tscripts/player.lua:30: in method 'update'", {{"scripts/player.lua", 30, 0}}},
            {"\t[C]: in function 'error'", {}},
            {"stack traceback:\n\t[C]: in function 'error'\n\tscripts/a.lua:5: in upvalue 'fail'\n\tscripts/b.lua:9: in function <scripts/b.lua:8>",
             {{"scripts/a.lua", 5, 0}, {"scripts/b.lua", 9, 0}, {"scripts/b.lua", 8, 0}}},
            {"Lua onReload() failed for 'scripts/hooks.lua': scripts/hooks.lua:3: boom\nstack traceback:\n\t[C]: in function 'error'\n\tscripts/hooks.lua:3: in method 'onReload'",
             {{"scripts/hooks.lua", 3, 0}}},
            {"Script property error: lua/enemies/boss.lua:101: index out of range", {{"lua/enemies/boss.lua", 101, 0}}},
            // GCC and Clang
            {"/home/me/game/scripts/Mover.cpp:42:17: error: 'speed' was not declared in this scope", {{"/home/me/game/scripts/Mover.cpp", 42, 17}}},
            {"scripts/Mover.cpp:42:17: warning: unused variable 'x' [-Wunused-variable]", {{"scripts/Mover.cpp", 42, 17}}},
            {"In file included from /home/me/game/scripts/Mover.cpp:1:", {{"/home/me/game/scripts/Mover.cpp", 1, 0}}},
            {"In file included from scripts/Mover.h:3,\n                 from scripts/Mover.cpp:1:", {{"scripts/Mover.h", 3, 0}, {"scripts/Mover.cpp", 1, 0}}},
            {"scripts/Mover.cpp:12:5: note: candidate: 'void Mover::move(float)'", {{"scripts/Mover.cpp", 12, 5}}},
            {"/usr/include/c++/13/bits/stl_vector.h:1125:7: note: declared here", {{"/usr/include/c++/13/bits/stl_vector.h", 1125, 7}}},
            {"scripts/Mover.cpp:8:1: fatal error: missing.h: No such file or directory", {{"scripts/Mover.cpp", 8, 1}}},
            {"/Users/me/My Game/scripts/Mover.cpp:3:10: error: expected ';'", {{"/Users/me/My Game/scripts/Mover.cpp", 3, 10}}},
            {"clang: error: linker command failed with exit code 1 (use -v to see invocation)", {}},
            {"/usr/bin/ld: Mover.cpp.o: in function `Mover::Mover()':", {}},
            // MSVC
            {"C:\\Users\\me\\game\\scripts\\Mover.cpp(42,17): error C2065: 'speed': undeclared identifier", {{"C:\\Users\\me\\game\\scripts\\Mover.cpp", 42, 17}}},
            {"C:\\Users\\me\\game\\scripts\\Mover.cpp(42): warning C4244: 'argument': conversion from 'double' to 'float'", {{"C:\\Users\\me\\game\\scripts\\Mover.cpp", 42, 0}}},
            {"C:\\Users\\Jo Doe\\My Game\\scripts\\Mover.cpp(7,1): error C1004: unexpected end-of-file found", {{"C:\\Users\\Jo Doe\\My Game\\scripts\\Mover.cpp", 7, 1}}},
            {"  scripts\\Mover.cpp(10,5): error C3861: 'jump': identifier not found [C:\\game\\build\\game.vcxproj]", {{"scripts\\Mover.cpp", 10, 5}}},
            {"Mover.obj : error LNK2019: unresolved external symbol", {}},
            // CMake
            {"CMake Error at CMakeLists.txt:12 (add_library):", {{"CMakeLists.txt", 12, 0}}},
            {"CMake Warning (dev) at /home/me/game/ProjectBuild.cmake:3 (target_link_libraries):", {{"/home/me/game/ProjectBuild.cmake", 3, 0}}},
            {"CMake Error at cmake/Deps.cmake:40 (message):\n  Call Stack (most recent call first):\n    CMakeLists.txt:7 (include)",
             {{"cmake/Deps.cmake", 40, 0}, {"CMakeLists.txt", 7, 0}}},
            // glslang and shaders
            {"ERROR: shaders/water.frag:12: 'foo' : undeclared identifier", {{"shaders/water.frag", 12, 0}}},
            {"ERROR: 0:12: 'foo' : undeclared identifier", {}},
            {"[Hot Reload] shaders/sky.glsl:4: compile failed (kept the previous version)", {{"shaders/sky.glsl", 4, 0}}},
            {"WARNING: assets/effects/glow.frag:30: precision qualifier ignored", {{"assets/effects/glow.frag", 30, 0}}},
            // HTML UI
            {"ui/menu.html:14:9: unknown element <buton>", {{"ui/menu.html", 14, 9}}},
            {"[HTML UI] ui/style.css:3:1: unknown property 'colr'", {{"ui/style.css", 3, 1}}},
            {"ui/hud.rml:2:5: warning: the 'data-model' attribute names no model", {{"ui/hud.rml", 2, 5}}},
            // S03 asset issues
            {"scenes/Level1.scene:120: unknown component 'Rigidbody4D' (kept as is)", {{"scenes/Level1.scene", 120, 0}}},
            {"materials/rock.material:8: value out of range", {{"materials/rock.material", 8, 0}}},
            {"prefabs/Enemy.prefab:33: missing asset 9f3c... (path textures/enemy.png)", {{"prefabs/Enemy.prefab", 33, 0}}},
            // not locations
            {"Play started at 12:30:45", {}},
            {"Loaded 37 files in 12.5 ms", {}},
            {"https://example.com:8080/page", {}},
            {"Vector3(1,2,3) is not a number", {}},
            {"Built 4/12 (33%)", {}},
            {"Reloaded scripts/player.lua (12 instances, 1 property added)", {}},
        };
        return cases;
    }

}

TEST(SourceLocation, Table) {
    ASSERT_GE(table().size(), 40u);
    for (const Case& test : table()) {
        const std::vector<SourceLocation> found = findSourceLocations(test.text);
        ASSERT_EQ(found.size(), test.locations.size()) << test.text << "\nfound: " << describe(found);
        for (size_t i = 0; i < found.size(); i++) {
            EXPECT_EQ(found[i].path, test.locations[i].path) << test.text;
            EXPECT_EQ(found[i].line, test.locations[i].line) << test.text;
            EXPECT_EQ(found[i].column, test.locations[i].column) << test.text;
            EXPECT_LT(found[i].begin, found[i].end);
            EXPECT_LE(found[i].end, std::string(test.text).size());
        }
    }
}

TEST(SourceLocation, Kinds) {
    EXPECT_EQ(findSourceLocations("a.lua:1:")[0].kind, "lua");
    EXPECT_EQ(findSourceLocations("a.cpp:1:2: error")[0].kind, "cpp");
    EXPECT_EQ(findSourceLocations("CMake Error at CMakeLists.txt:3 (x)")[0].kind, "cmake");
    EXPECT_EQ(findSourceLocations("ERROR: a.frag:2: x")[0].kind, "shader");
    EXPECT_EQ(findSourceLocations("a.html:1:1 x")[0].kind, "html");
    EXPECT_EQ(findSourceLocations("a.scene:4: x")[0].kind, "asset");
}

TEST(SourceLocation, BuildTreePathsMapToProjectFiles) {
    jasperengine::test::TempDir dir;
    const auto project = dir.path() / "game";
    std::filesystem::create_directories(project / "scripts");
    std::filesystem::create_directories(project / "lua");
    std::ofstream(project / "scripts" / "Mover.cpp") << "// mover\n";
    std::ofstream(project / "lua" / "player.lua") << "return {}\n";

    SourceRoots roots;
    roots.project = project;
    roots.lua = project / "lua";
    roots.assets = project / "assets";

    // a copy in another build tree: the project's own file
    const ResolvedSource copied = resolveSourceLocation("/build/somewhere/else/game/scripts/Mover.cpp", roots);
    EXPECT_TRUE(copied.exists);
    EXPECT_TRUE(copied.inProject);
    EXPECT_EQ(copied.projectPath, "scripts/Mover.cpp");

    // a Lua chunk path is relative to the Lua directory
    const ResolvedSource lua = resolveSourceLocation("player.lua", roots);
    EXPECT_TRUE(lua.exists);
    EXPECT_EQ(lua.projectPath, "lua/player.lua");

    const ResolvedSource relative = resolveSourceLocation("scripts/Mover.cpp", roots);
    EXPECT_TRUE(relative.exists);
    EXPECT_EQ(relative.projectPath, "scripts/Mover.cpp");

    const ResolvedSource missing = resolveSourceLocation("scripts/Gone.cpp", roots);
    EXPECT_FALSE(missing.exists);

    EXPECT_EQ(projectFileFor(project, "/x/y/game/.jasperengine/export/scripts/Mover.cpp"), "scripts/Mover.cpp");
    EXPECT_EQ(formatSourceLocation("a.lua", 3, 0), "a.lua:3");
    EXPECT_EQ(formatSourceLocation("a.cpp", 3, 7), "a.cpp:3:7");
}

TEST(SourceLocation, InsertTimeBudget) {
    std::vector<std::string> lines;
    for (const Case& test : table()) lines.push_back(test.text);
    for (int i = 0; i < 200; i++) lines.push_back("[Hot Reload] Reloaded assets/hero_" + std::to_string(i) + ".png (texture, 34 ms)");
    const int rounds = 200;
    size_t found = 0;
    const auto start = std::chrono::steady_clock::now();
    for (int r = 0; r < rounds; r++) {
        for (const std::string& line : lines) found += findSourceLocations(line).size();
    }
    const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count() / (rounds * lines.size());
    std::printf("[bench] source locations %.3f us per line (budget 2)\n", us);
    EXPECT_GT(found, 0u);
    // the budget is the benchmark run's (JASPER_TEST_PERF_SLACK on the benchmark server,
    // docs/testing.md): a shared builder's load, or a sanitizer, alone can miss it
    if (const double slack = jasperengine::editor::test::perfSlack(); slack > 0.0) {
        EXPECT_LE(us, 2.0 * slack) << "R37 (2 us per line)";
    }
}
