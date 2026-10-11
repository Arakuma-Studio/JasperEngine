// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Generator.h"
#include "component/ScriptComponent.h"
#include "script/ScriptProperty.h"
#include "shader/ShaderManifest.h"
#include "shader/ShaderData.h"

#include "json.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

struct lua_State;

namespace jasperengine::editor {

    class Project;

    // The checks of a project's Lua scripts, forked shaders and C++ scripts, shared by the
    // `check` CLI subcommand (cli/CommandLine.cpp) and the MCP tools check_script,
    // check_shader, compile_scripts and sync_files, so a shell, a CI job or an agent hook gets
    // what an agent gets through MCP. docs/mcp/checks.md describes the CLI.
    class ProjectChecks {
    public:
        // One problem, compiler style. tool: "lua", "shader" or "cpp". path: project-relative
        // with '/' (absolute when outside the project, "" when the problem has no file). line
        // and col are 1-based, 0 when unknown. severity: "error" or "warning".
        struct Finding {
            std::string tool;
            std::string path;
            int line = 0;
            int col = 0;
            std::string severity;
            std::string message;
        };

        static nlohmann::json toJson(const Finding& finding);
        static nlohmann::json toJson(const std::vector<Finding>& findings);
        // "path:line:col: severity: message", leaving out the unknown parts ("tool: severity:
        // message" without a path); the message on one line
        static std::string toText(const Finding& finding);
        // by tool (lua, shader, cpp), then path, line, column
        static void sortFindings(std::vector<Finding>& findings);
        static size_t countSeverity(const std::vector<Finding>& findings, const std::string& severity);

        // ---- files

        // Project-relative generic path; the absolute path when outside the project
        static std::string projectRelative(const std::filesystem::path& projectRoot, const std::filesystem::path& absolute);
        // Directories project scans skip: hidden ones (.git, .jasperengine and its build) and "build"
        static bool isSkippedDir(const std::filesystem::path& dir);
        // True when a project-relative path goes through a skipped directory
        static bool isInSkippedDir(const std::filesystem::path& relative);
        static bool readFile(const std::filesystem::path& path, std::string& content);
        // Every file of the project with one of the extensions (lowercase, with the dot),
        // skipping the skipped directories; sorted, absolute
        static std::vector<std::filesystem::path> findProjectFiles(const std::filesystem::path& projectRoot,
                                                                   const std::vector<std::string>& extensions);
        // The project's Lua directory from its project.yaml (luaDir, default the project root)
        static std::filesystem::path luaRootFromProjectFile(const std::filesystem::path& projectRoot);

        // ---- Lua

        // Syntax only: compiles the chunk in a fresh lua_State without running it. line = 0 when ok.
        struct LuaSyntax {
            bool ok = true;
            int line = 0;
            std::string message;
        };
        static LuaSyntax checkLuaSyntax(const std::string& code, const std::string& chunkName);

        struct LuaScriptCheck {
            bool syntaxOk = true;
            std::vector<Finding> findings;           // errors
            std::vector<ScriptProperty> properties;  // when it loaded
        };
        // A Lua script: its syntax, then, when it parses and L is set, its properties read as
        // the editor reads them (ProjectUtils::runLuaScriptProperties: this RUNS the script's top
        // level in L). A script whose top level fails ("runtime error at load: ...") or whose
        // property defaults can't be read is an error. path names the chunk and the findings.
        static LuaScriptCheck checkLuaScript(const std::string& code, const std::string& path, lua_State* L);

        // An isolated lua_State for checkLuaScript outside the editor (the CLI): no io, os
        // (but time/clock/date), debug, dofile/loadfile, binary chunks or native modules;
        // print does nothing; require loads the project's Lua modules from luaRoot like the
        // engine's loader; the engine's globals (engineGlobals) read as inert stand-ins
        // (callable, indexable) so top-level engine calls run; memory and run time are capped.
        // Close it with closeIsolatedLuaState.
        static lua_State* newIsolatedLuaState(const std::filesystem::path& luaRoot, const std::vector<std::string>& engineGlobals);
        static void closeIsolatedLuaState(lua_State* L);
        // The global names of the engine's Lua state (LuaBinding, after Engine::systemInit)
        static std::vector<std::string> engineLuaGlobals();

        // ---- C++ scripts

        struct CppHeaderInfo {
            std::optional<std::string> className;   // the single script class, if there is one
            std::optional<bool> directScript;       // derives from the script base itself (else a subclass)
            std::vector<ScriptProperty> properties; // DPROPERTY fields
        };
        static CppHeaderInfo checkCppHeader(const std::string& code, const std::string& name);

        // Building the C++ scripts as Play and compile_scripts do: Project::prepareScriptBuild
        // (generated sources, CMake configuration), the toolchain check, a Generator build.
        struct CppBuildPlan {
            std::vector<SceneScriptSource> scripts;
            CMakeKit kit;
            unsigned int jobs = 0;
        };
        enum class CppPrepare { Ready, NothingToBuild, Failed, MissingTools };
        // Main thread. Failed (generating the build files) and MissingTools: error says why.
        static CppPrepare prepareCppBuild(Project* project, CppBuildPlan& plan, std::string& error);
        // Starts generator.build for the project (asynchronous)
        static void startCppBuild(Project* project, Generator& generator, const CppBuildPlan& plan);
        // Blocks until the build ends; every line of build output (the Out entries other than
        // info/success logged after firstLogId) goes to onLine and is returned
        static std::vector<std::string> waitCppBuild(Generator& generator, uint64_t firstLogId,
                                                     const std::function<void(const std::string&)>& onLine = {});

        struct CppDiagnostic {
            std::string file;     // project-relative when inside the project
            int line = 0;
            int column = 0;
            std::string severity; // error or warning
            std::string message;
        };
        struct CppBuildReport {
            bool success = false;
            std::vector<CppDiagnostic> errors;    // deduplicated, at most 60
            std::vector<CppDiagnostic> warnings;  // at most 30
            std::vector<std::string> logTail;     // last 30 lines
            std::vector<Finding> findings;        // errors and warnings; a failure without a file:line is one
        };
        // gcc/clang "file:line:col: error: message" and MSVC "file(line,col): error C1234: message"
        // lines of the output; notes are dropped
        static CppBuildReport summarizeCppBuild(const std::filesystem::path& projectRoot, bool success,
                                                const std::vector<std::string>& output);

        // ---- shaders

        // A shader fork (a customShader value) and the ShaderType it is used as (-1: unknown)
        struct ShaderUse {
            std::string base;
            int shaderType = -1;
        };
        // The ShaderType a component's customShader is used as (false: no custom shaders)
        static bool shaderTypeForComponent(ComponentType component, ShaderType& out);
        // Every fork the loaded scenes, prefab stages and scene settings use
        static std::vector<ShaderUse> shaderForksInUse(Project* project);
        // The forks project files belong to: a .vert/.frag's fork (or its pair, when nothing
        // uses it yet); for a .glsl, every fork in use (any can include it)
        static std::vector<ShaderUse> shaderForksUsingFiles(Project* project, const std::vector<std::string>& relativeFiles);
        // Every fork in use plus every .vert/.frag pair of the project no scene uses yet
        static std::vector<ShaderUse> shaderForksInProject(Project* project);
        // The type an unused fork most likely is, from its fragment shader
        static int guessShaderType(Project* project, const std::string& base);
        // mesh, ui, sky, points, lines, depth, post_process (or unknown)
        static std::string shaderTypeName(int shaderType);
        // glslang lines "ERROR: <source>:<line>: <message>" as [{severity, source, line, message}]
        static nlohmann::json parseShaderLog(const std::string& log);

        struct ShaderCheck {
            ShaderUse use;
            bool ok = true;
            nlohmann::json errors = nlohmann::json::array();  // parseShaderLog
            std::string log;
            std::vector<Finding> findings;
        };
        // Main thread, once before a round of checks: drops cached builds and source snapshots
        // so the checks (and the scenes) compile the files on disk
        static void invalidateShaderCaches(Project* project);
        // Main thread: resolves an unknown type and registers the fork; returns its ShaderKey
        static uint64_t prepareShaderCheck(Project* project, ShaderUse& use);
        // Any thread: compiles the fork with the editor's shader builder for the editor's
        // backend (ShaderBuilder::buildShaderForExport), the files read from the project
        static ShaderCheck runShaderCheck(Project* project, const ShaderUse& use, uint64_t shaderKey);
        // {shader, type, ok, errors, log} (log and errors only on failure)
        static nlohmann::json toJson(const ShaderCheck& check);
        // The problems of a shader's manifest (a library reference or a fork with one): its
        // syntax, and its @property members and @texture uniforms checked against what the
        // compiled variants declare (onlyEnabledFeatures: skip the rows behind features the
        // reference's suffix does not enable). Empty for a shader without a manifest.
        static std::vector<ShaderManifestDiagnostic> validateShaderManifest(const std::string& reference,
                                                                           const std::vector<const ShaderData*>& variants,
                                                                           bool onlyEnabledFeatures);
        // Materials of the loaded scenes and prefab stages whose shader (or extra pass) reads the
        // scene depth or colour (@require scenedepth / scenecolor) while the material resolves to
        // the opaque queue: errors (tool "shader"), at the scene or prefab file
        static std::vector<Finding> sceneInputFindings(Project* project);
        // The project file a compiler message source (an entry point or include key) of a fork is
        // ("engine/shaderlib/<key>" for an engine library include)
        static std::string shaderSourcePath(Project* project, const std::string& base, const std::string& source);
    };

}
