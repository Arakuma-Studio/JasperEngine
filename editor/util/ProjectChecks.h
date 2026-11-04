// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Generator.h"
#include "htmlui/compat/CompatOptions.h"
#include "htmlui/compat/Diagnostics.h"
#include "script/ScriptProperty.h"
#include "shader/ShaderManifest.h"
#include "shader/ShaderData.h"
#include "shader/ShaderBuildTypes.h"

#include "json.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

struct lua_State;

namespace jasperengine::htmlui {
    struct EffectManifest;
}

namespace jasperengine::editor {

    class Project;

    // The checks of a project's Lua scripts, forked shaders, C++ scripts and HTML/CSS UI files,
    // shared by the `check` CLI subcommand (cli/CommandLine.cpp) and the MCP tools
    // check_script, check_shader, compile_scripts and sync_files (and the automatic pick-up), so
    // a shell, a CI job or an agent hook gets what an agent gets through MCP.
    // docs/mcp/checks.md describes the CLI.
    class ProjectChecks {
    public:
        // One problem, compiler style. tool: "lua", "shader", "cpp" or "ui". path: project-relative
        // with '/' (absolute when outside the project, "" when the problem has no file). line
        // and col are 1-based, 0 when unknown. severity: "error" or "warning".
        struct Finding {
            std::string tool;
            std::string path;
            int line = 0;
            int col = 0;
            std::string severity;
            std::string message;
            std::string code;        // ui: the compat diagnostic code ("HTMLUI-CSS-0002"); "" for the other tools
            std::string suggestion;  // ui: the fix; "" when none
        };

        // {tool, path, line, col, severity, message}, plus code and suggestion when not empty
        static nlohmann::json toJson(const Finding& finding);
        static nlohmann::json toJson(const std::vector<Finding>& findings);
        // "path:line:col: severity: message (fix: suggestion) [code]", leaving out the unknown
        // parts ("tool: severity: message" without a path); the message on one line
        static std::string toText(const Finding& finding);
        // by tool (lua, shader, cpp, ui), then path, line, column
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
        // level in L, an isolated state like newIsolatedLuaState's). A script whose top level
        // fails ("runtime error at load: ...") or whose property defaults can't be read is an
        // error. path names the chunk and the findings.
        static LuaScriptCheck checkLuaScript(const std::string& code, const std::string& path, lua_State* L);

        // An isolated lua_State for checkLuaScript and the editor's reading of script properties
        // (ProjectUtils::readLuaScriptProperties), any thread: no io, os
        // (but time/clock/date), debug, dofile/loadfile, binary chunks or native modules;
        // print does nothing; require loads the project's Lua modules from luaRoot like the
        // engine's loader; the engine's globals (engineGlobals) read as inert stand-ins
        // (callable, indexable) so top-level engine calls run; memory and run time are capped.
        // Close it with closeIsolatedLuaState.
        static lua_State* newIsolatedLuaState(const std::filesystem::path& luaRoot, const std::vector<std::string>& engineGlobals);
        static void closeIsolatedLuaState(lua_State* L);
        // The global names of the engine's Lua state as the engine made it (LuaBinding::
        // getEngineGlobalNames, after Engine::systemInit): any thread
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

        // ---- HTML/CSS UI: the compatibility layer's diagnostics (engine/core/htmlui/compat), the
        // same code that runs when a document loads

        // .html, .htm, .css (any case)
        static bool isUiFile(const std::string& path);
        // The project's assets directory from its project.yaml (assetsDir, default the project root)
        static std::filesystem::path assetsRootFromProjectFile(const std::filesystem::path& projectRoot);
        // Sandboxed project loader for compat: paths are asset-relative and resolve under the
        // project's assets directory; absolute paths and '..' escapes are refused (compat reports
        // those files as missing)
        static jasperengine::htmlui::compat::FileAccess projectFiles(const std::filesystem::path& projectRoot);
        // A compat diagnostic as a finding (tool "ui"): its asset-relative file shown
        // project-relative, severity error/warning/info, code and suggestion
        static Finding uiFinding(const std::filesystem::path& projectRoot, const std::filesystem::path& assetsRoot,
                                 const jasperengine::htmlui::compat::Diagnostic& diagnostic);
        struct UiCheck {
            std::vector<Finding> findings;   // tool "ui", severity "error"/"warning" (Info dropped), code, suggestion
            size_t documents = 0;            // .html/.htm converted
            size_t stylesheets = 0;          // .css checked on their own
            std::vector<std::string> files;  // project-relative: every file checked or read by the checks
        };
        // files empty: the whole project, meaning every .html/.htm of the assets directory outside
        // skipped directories except the web export's custom shell (project.yaml
        // web.customHtmlShell), then every .css none of those documents depends on. files given
        // (absolute): an .html/.htm is converted as a document, a .css is checked on its own
        // (files outside the assets directory are skipped). A diagnostic located in a linked
        // stylesheet is reported at that stylesheet's path; identical findings (a stylesheet
        // linked by several documents) are reported once. Pure parsing: no engine start.
        static UiCheck checkUiFiles(const std::filesystem::path& projectRoot, const std::vector<std::filesystem::path>& files);
        // `jasperengine-editor ui-support`: the compat support tables as JSON
        // {version, editor_version, features: [{kind, name, support, note, suggestion, code, ...}],
        //  diagnostics: [{code, name, severity, summary}]} (support computed with the fork
        // features present)
        static nlohmann::json uiSupportJson();

        // ---- texture synthesis recipes

        // PNGs written by the texture synthesis carry their recipe (texsynth/SynthesisRecipe.h):
        // the sources' SHA-1s at synthesis time. A source changed or deleted since makes the
        // texture stale: a warning (tool "texture") at the PNG, code synthesis-source-changed or
        // synthesis-source-missing, with how to regenerate it; a garbled recipe (CRC, not JSON,
        // wrong types) is a warning synthesis-recipe-invalid. Fast on large projects: only the
        // chunks before IDAT are read, sources are hashed only for PNGs with a recipe, and both
        // are cached by mtime and size in .jasperengine/synthesis-check-cache.json (when that
        // directory exists).
        struct SynthesisCheck {
            std::vector<Finding> findings;
            size_t recipes = 0;   // PNGs with a recipe that were checked
            size_t pngs = 0;      // PNGs looked at
        };
        // files empty: every PNG of the project outside skipped directories; a companion or
        // uncertainty output whose primary output exists is covered by the primary's findings.
        // files given (absolute): those PNGs, plus every project PNG whose recipe names one of the
        // files as a source (an edited example reports the textures made from it).
        static SynthesisCheck checkSynthesisRecipes(const std::filesystem::path& projectRoot, const std::vector<std::filesystem::path>& files);

        // ---- HTML UI effects (phase 5)

        // An effect as `ui-effects --json` and MCP list_ui_effects give it: every manifest field
        // (name kind category summary source cost backdrop pointer time suggestFor params builtin
        // path, prepass/margin/geometry when set), an example declaration and the thumbnail path
        static nlohmann::json uiEffectJson(const jasperengine::htmlui::EffectManifest& manifest);
        // "background-image: effect(stripes, angle 45deg);" (its first simple parameter)
        static std::string uiEffectExample(const jasperengine::htmlui::EffectManifest& manifest);

        // ---- shaders

        // A shader fork (a customShader value) and the ShaderType it is used as (-1: unknown)
        struct ShaderUse {
            std::string base;
            int shaderType = -1;
            uint64_t properties = 0;   // UI effects: 1 = the filter form ('Flt')
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
        // A project UI effect (phase-5 spec 3.10): a .frag whose manifest (the comment lines after
        // #version 450) starts with "// @effect". Its uses are its forms (a paint effect both, a
        // filter effect the filter form; one use when the manifest does not parse, so the check
        // reports it), named by the effect's custom shader name: the asset-relative path
        // (project-relative outside the assets directory), as @effect registers it.
        static bool isUiEffectFile(const std::filesystem::path& absolute);
        static std::vector<ShaderUse> uiEffectUses(Project* project, const std::string& relativeFile);
        // every UI effect .frag of the project (project-relative, sorted)
        static std::vector<std::string> uiEffectFiles(Project* project);
        // the file a UI effect's custom shader name stands for (absolute)
        static std::filesystem::path uiEffectFile(Project* project, const std::string& base);
        // The type an unused fork most likely is, from its fragment shader
        static int guessShaderType(Project* project, const std::string& base);
        // mesh, ui, sky, points, lines, depth, post_process, ui_effect (or unknown)
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
        static ShaderKey prepareShaderCheck(Project* project, ShaderUse& use);
        // Any thread: compiles the fork with the editor's shader builder for the editor's
        // backend (ShaderBuilder::buildShaderForExport), the files read from the project. A UI
        // effect's manifest is checked first (HTMLUI-FX-0014 effect-manifest at its line, no
        // compile then); a compile error is HTMLUI-FX-0015 effect-shader-compile at the .frag
        // line, and the compiled variant is checked against the manifest (its parameter block)
        static ShaderCheck runShaderCheck(Project* project, const ShaderUse& use, ShaderKey shaderKey, ShaderData* built = nullptr);
        // {shader, type, ok, errors, log} (log and errors only on failure); a UI effect adds
        // effect, form (paint/filter) and, on failure, code
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
        // the opaque queue: errors (tool "shader"), at the scene or prefab file. With forks, only
        // the materials drawing with one of them (their shader or extra pass): a check of some
        // files reports the materials of the forks it checked
        static std::vector<Finding> sceneInputFindings(Project* project, const std::vector<ShaderUse>* forks = nullptr);
        // The project file a compiler message source (an entry point or include key) of a fork is
        // ("engine/shaderlib/<key>" for an engine library include)
        static std::string shaderSourcePath(Project* project, const std::string& base, const std::string& source);

        // ---- Stochastic Tiling (materials, terrain layers, library shaders' STOCHASTIC feature)

        // Warnings (tool "shader", at the scene or prefab file) for the loaded scenes and prefab
        // stages: code "stochastic-tilted-normal-map" where Rotation is above 0 and the normal map
        // the cells turn has an average normal at least NormalMapTilt::WARNING_DEGREES off +Z
        // (built-in Lit materials, terrain layers, library shader materials with the feature);
        // code "stochastic-no-effect" where a material asks Lit's Stochastic Tiling but it does
        // not apply (no texture, a texture rect, a terrain, a shader with a manifest)
        static std::vector<Finding> stochasticTilingFindings(Project* project);

        // ---- Water (water-reflections spec 6.3, 6.8)

        // Warnings (tool "shader", at the scene or prefab file) for the loaded scenes and prefab
        // stages: code "water-fork-without-role" where a material or post-process pass draws
        // with a fork that declares library:water's parameters (buoyancy, waveHeight,
        // shallowColor) or library:underwater's (waterLevel, followWater) but no @role, so the
        // engine does not treat it as water; code "planar-reflection-not-flat" where a mesh reads
        // a planar reflection but its world bounds are taller than 1% of their larger horizontal
        // extent plus 0.05 (it mirrors about one level)
        static std::vector<Finding> waterFindings(Project* project);

        // Line and Trail Renderers of the loaded scenes (lines and trails spec 6.8): code
        // "line-texture-no-uv" where a hairline (no width) draws with a shader that samples a
        // texture (a hairline has no texture coordinates), "trail-zero-time" where a trail's
        // Time is 0 (it keeps no point: nothing is drawn)
        static std::vector<Finding> lineTrailFindings(Project* project);

        // Particle Systems of the loaded scenes (docs/specs/S20-particles.md, Upstream absorption):
        // "particles-max-exceeded-budget" (warning: an effect whose Max Particles is past a mobile
        // build's world particle budget, which thins its emission there), "particles-effect-invalid"
        // (error: an effect that does not compile, so the system draws nothing)
        static std::vector<Finding> particleFindings(Project* project);
    };

}
