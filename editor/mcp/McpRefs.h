// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "McpJson.h"

#include "Catalog.h"
#include "Project.h"

#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace jasperengine::editor::mcp {

    // Scene / GameObject / component / asset references in tool arguments (DESIGN.md,
    // Addressing). Every resolver fills `error` with a message that tells the agent how
    // to fix the reference (listing the valid choices) and returns null / false.
    class McpRefs {
    public:
        // Scene: null / missing → the selected scene; integer (or digit string) → scene id
        // (Play copies of child scenes included); string → scene name, or a project-relative
        // .scene path. Includes open prefab stages. An object {"id"} / {"name"} / {"path"}
        // (what sceneJson returns) works too.
        static SceneProject* resolveScene(Project* project, const Json& ref, std::string& error);

        // GameObject in `scene`: integer (or digit string) → entity id; string → a path of
        // '/'-joined names from the scene root, "Name#N" being the Nth earlier sibling with
        // that name (the scene-dump scheme). A bare name matching one GameObject anywhere
        // resolves too; several matches are an error listing their paths. Objects:
        // {"id": n} (what entityJson returns: id wins), {"path": "A/B"}, {"name": "B"}
        // (any depth; ambiguity is an error listing the candidates).
        static Entity resolveEntity(SceneProject* scene, const Json& ref, std::string& error);
        // One ref or an array of refs (duplicates dropped, order kept). Empty array → error.
        static bool resolveEntities(SceneProject* scene, const Json& refs, std::vector<Entity>& out, std::string& error);
        // `entities` (array) or else `entity` (one ref) of a tool's arguments
        static bool resolveEntityArgs(SceneProject* scene, const Json& args, std::vector<Entity>& out, std::string& error);

        // The path resolveEntity accepts for this GameObject
        static std::string entityPath(SceneProject* scene, Entity entity);
        // Every GameObject's path in one pass (EntityPath::of for each is quadratic)
        static std::unordered_map<Entity, std::string> entityPaths(SceneProject* scene);
        // {id, name, path}
        static Json entityJson(SceneProject* scene, Entity entity);
        // {id, name, path (project-relative .scene, "" if never saved), type, opened, modified, play_state}
        static Json sceneJson(Project* project, const SceneProject& scene);
        static std::string sceneTypeName(SceneType type);
        // "Scene 3 \"Level\" is not open: open_scene {\"scene\": \"scenes/Level.scene\"} first ..."
        static std::string notOpenError(Project* project, const SceneProject& scene);
        // "3D" / "2D" / "UI" (any case, also "3d_scene"...) → SceneType
        static bool parseSceneType(const std::string& name, SceneType& out);
        static std::string playStateName(ScenePlayState state);

        // Play mode: the scene the running game uses for `scene` (its Play copy for a child
        // scene that runs as a copy), else `scene` itself. Tools reading runtime state use it.
        static SceneProject* runtimeScene(Project* project, SceneProject* scene);

        // Component type by Catalog name (transform, mesh, rigidbody2d, ...; a "Component"
        // suffix and any case are accepted). Unknown → error listing the valid names.
        static bool resolveComponent(const Json& ref, ComponentType& out, std::string& error);
        // Valid component names (Catalog::getComponentName without the suffix, lower case)
        static std::vector<std::string> componentTypeNames();
        // The name tools use for a component type: lower case, no suffix ("rigidbody2d")
        static std::string componentName(ComponentType type);
        // Whether the GameObject has the component
        static bool hasComponent(SceneProject* scene, Entity entity, ComponentType type);

        // Project-relative path → absolute, sandboxed to the project (no '..' escaping it,
        // no absolute paths). mustExist adds an existence check. Lexical only: reads follow
        // symbolic links (an assets folder deliberately linked in from a shared library reads
        // fine); every tool that writes, creates, deletes, moves or renames uses
        // resolveProjectWritePath instead.
        static bool resolveProjectPath(Project* project, const std::string& relative,
                                       std::filesystem::path& absolute, std::string& error, bool mustExist = false);

        // What a write acts on: the final name followed too when it is a link (a dangling one
        // included: a plain write would create its target), or (Entry) only the folders on the
        // way, the entry itself acted on (a delete or a move's source: a link there is removed or
        // moved, never followed)
        enum class WriteUse { Write, Entry };
        // resolveProjectPath for a path a tool writes, creates, deletes, moves or renames: also
        // refused when, once symbolic links (and Windows junctions) are resolved
        // (util/SafeWrite.h, resolvedPath), it lies outside the project directory (itself
        // resolved: a project opened through a link works) or a link on the way loops. `absolute`
        // is the path as named (under the project's spelling). The error names the path, where
        // the link leads and how to fix it.
        static bool resolveProjectWritePath(Project* project, const std::string& relative, std::filesystem::path& absolute,
                                            std::string& error, bool mustExist = false, WriteUse use = WriteUse::Write);
        // The same check for an absolute path a tool computed itself (a default output beside its
        // source, a generated file): "" when it stays inside the project, else the error
        static std::string writeEscape(Project* project, const std::filesystem::path& absolute, WriteUse use = WriteUse::Write);
        // The project's trusted roots: its directory, its configured assets directory and its
        // configured Lua scripts directory (as loaded from project.yaml; each resolved once). Containment = the link-resolved target
        // lies inside one of them resolved; links below them are what is checked.
        static std::vector<std::filesystem::path> trustedRoots(Project* project);
        // The trusted root a beneath-root write of `absolute` goes under (the one holding it once
        // resolved; the project directory when none does: the write then refuses the escape)
        static std::filesystem::path rootFor(Project* project, const std::filesystem::path& absolute, WriteUse use = WriteUse::Write);
        // writeEscape against the project directory alone: for a path that becomes a trusted root
        // (an asset root setting), so an agent can't make a folder outside the project trusted
        static std::string projectOnlyEscape(Project* project, const std::filesystem::path& absolute);
        // writeEscape of every editor-owned folder a build, Play or a script compile writes or
        // sweeps (the internal folder, its build tree and hot-reload staging, the engine API
        // snapshot and Lua stubs, the generated sources, the logs, the external build, the
        // imported models, the thumbnails, the terrain maps, .vscode): their writers are CMake,
        // the compilers and editor code running later, so they are checked up front. "" or the refusal.
        static std::string internalFoldersEscape(Project* project);
        // A move, rename or delete of `absolute` (or a move to it, `destination`) that would touch
        // a trusted root: the assets or Lua folder itself or a folder holding one (moving it away
        // and a link in its place must not re-point a root). "" or the refusal.
        static std::string trustedRootTouched(Project* project, const std::filesystem::path& absolute, bool destination);
        // A place no tool writes, moves, copies or creates something at: under a hidden folder
        // (.jasperengine, .git, .trash ...), a file named project.yaml (any folder), the project's
        // CMakeLists.txt. Checked on the path as named and on where it resolves (a link inside the
        // project to its .jasperengine or its root is no way in); names compared as the file system
        // may (letter case, Windows aliases: safewrite::comparableName). "" or the refusal.
        static std::string protectedPlace(Project* project, const std::filesystem::path& absolute, WriteUse use = WriteUse::Write);
        // The first symbolic link (or junction) inside the folder `dir` (any depth, links not
        // followed, so loops are harmless) that leads out of the trusted roots: its writeEscape;
        // also a refusal when the folder holds more than `maxEntries` entries (too many to check).
        // "" none.
        static std::string linkOutInside(Project* project, const std::filesystem::path& dir, size_t maxEntries = 200000);
    };

}
