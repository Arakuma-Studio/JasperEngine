// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// What a project can reach of the optional engine modules (docs/specs/S34-player-size.md R5, D4,
// D5): the export leaves the others out of the player it ships (a prebuilt variant, or the module
// switches of a Compile build). A module is needed when anything that ships can create or call
// its objects:
//
// - a component of a scene or prefab whose system is the module's (a UI Document, a Rigidbody), or
//   a component field that makes the engine create one (a Spline Extrude's collision);
// - a Lua script naming one of the module's entry points: its classes, enums and globals, its
//   members of classes outside it, and every member whose type is one of its classes, read from
//   the Lua API description (the `modules` of tools/lua_api, the bindings' own #if conditions);
//   a member's name in a string counts too (go["getRigidbody"]), a class's does not (a global is
//   reached by its name only through a dynamic access); comments do not;
// - a script property typed with one of its classes;
// - a dynamic access that can reach any global without naming it (_G, _ENV, load, debug,
//   package, a computed require ...): every module;
// - C++ scripts (their code can use anything): every module.
//
// The video module (the HTML UI's <video>) is needed with the HTML UI when a document has a
// <video> element, a script names video or the media API. The runtime shader compiler is needed
// when a script can ask for a shader variant the export did not cook: it names a shader-relevant
// entry point (ShaderNeutralApi), a script property has a shader-relevant type, or a script makes
// a dynamic access. Navigation follows Project Settings > Navigation > Include in Builds; the sound
// layer, a clip whose import settings change what it plays.

#include "mcp/McpJson.h"
#include "ecs/Entity.h"

#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace jasperengine {
    class EntityRegistry;
}

namespace jasperengine::editor {

    class Project;

    struct ModuleEvidence {
        std::string module;
        std::string where;   // "scene Main", "prefab Enemy", "lua/hud.lua:12", "ui/menu.html"
        std::string what;    // "UI Document on Menu", "UIDocument", "_G (a dynamic global access)"
    };

    struct ModuleUsage {
        std::set<std::string> modules;          // needed
        std::vector<ModuleEvidence> evidence;   // at most a few per module, the first first

        bool needs(const std::string& module) const { return modules.count(module) > 0; }
        // the needed modules in player::allModules() order
        std::vector<std::string> list() const;
        // "lua/hud.lua:12: UIDocument" (empty when not needed)
        std::string firstEvidence(const std::string& module) const;
        std::vector<ModuleEvidence> evidenceOf(const std::string& module) const;
    };

    // a scene's or a prefab's entities
    struct ModuleContent {
        std::string label;                 // "scene Main", "prefab Enemy"
        EntityRegistry* registry = nullptr;
        std::vector<Entity> entities;
    };

    struct ModuleSource {
        std::string path;                  // project-relative
        std::string text;
    };

    struct ModuleScan {
        std::vector<ModuleContent> contents;
        std::vector<ModuleSource> luaScripts;
        std::vector<ModuleSource> documents;    // .html .htm .rml .xhtml
        std::vector<std::string> cppScripts;    // C++ script sources (project-relative)
        bool debugOverlayInBuilds = false;      // Project Settings > Debug: the overlay is an HTML UI document
        bool navigationInBuilds = true;         // Project Settings > Navigation > Include in Builds
        bool audioImportTable = false;          // the export writes je-audio-imports.json (a clip with import settings)
        bool stripEngineCode = true;            // Project Settings > Player > Strip Engine Code
        // what this scan made to read prefabs (their files' registries, instance previews)
        std::vector<std::shared_ptr<void>> owned;
    };

    class ModuleUsageDetector {
    public:
        // what a project ships: its scenes and prefabs as the export loaded them, every .lua of
        // the project, every HTML UI document, its C++ scripts and settings
        static ModuleScan collect(Project* project);
        static ModuleUsage analyze(const ModuleScan& scan);
        // the same with another Lua API description (tests)
        static ModuleUsage analyze(const ModuleScan& scan, const mcp::Json& luaApi);

        // the Lua names that reach a module (lua-api.json -> module -> names)
        static std::map<std::string, std::set<std::string>> moduleEntryPoints(const mcp::Json& luaApi);
        // the Lua names that can reach a shader input the export did not see
        static std::set<std::string> shaderRelevantNames(const mcp::Json& luaApi);
        // the names that reach any global without spelling it
        static const std::set<std::string>& dynamicAccessNames();
    };

}
