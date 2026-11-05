// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef SCRIPTATTACH_H
#define SCRIPTATTACH_H

#include "ecs/Entity.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace jasperengine {
    enum class ScriptType;
    struct ScriptEntry;
}

namespace jasperengine::editor {

    class Project;
    struct SceneProject;

    // A script file of the project that can go on a GameObject
    struct ScriptCandidate {
        ScriptType type{};             // set by findProjectScripts
        std::string name;               // Lua: the file name; C++: the class
        std::filesystem::path file;     // absolute: the .lua, or the C++ header
        std::filesystem::path source;   // C++: the .cpp beside the header
        bool subclass = false;          // C++ class deriving an engine object class, not ScriptBase
        std::string baseClass;          // the class a subclass derives (subclass only)
    };

    // Attaching script files to GameObjects, one path for the Inspector (Add Script, drops), the
    // Hierarchy (drops), the New Script dialog and MCP attach_script
    class ScriptAttach {
    public:
        // The entry for a script file (absolute .lua, .h/.hpp or .cpp; a C++ pair side by side).
        // A Lua script's name is its file name (className differing adds a warning); a C++ class is
        // found in the header when className is empty.
        static bool makeEntry(Project* project, const std::filesystem::path& file, const std::string& className,
                              ScriptEntry& entry, std::vector<std::string>& warnings, std::string& error);
        // Attaches entry to the entities (a Script component added where missing) as one undo step on
        // the scene; index < 0 appends. The entities that already have the file are left out (in
        // `skipped`); false, with nothing done, when none is left or the editor refuses the change.
        // indexOut: where it went on the first entity. commandSceneId: the undo history it goes in
        // (0: the scene's own; the Inspector's is the selected scene's, as its other edits)
        static bool attach(Project* project, SceneProject* sceneProject, const std::vector<Entity>& entities, const ScriptEntry& entry,
                           long long index, std::vector<Entity>& skipped, std::string& error, size_t* indexOut = nullptr,
                           uint32_t commandSceneId = 0);
        // makeEntry + attach for a human's gesture (a drop, a pick): problems go to the Console
        static bool attachFile(Project* project, SceneProject* sceneProject, const std::vector<Entity>& entities,
                               const std::filesystem::path& file, uint32_t commandSceneId = 0);
        // The project's scripts: Lua modules under the Lua directory, C++ script classes in the
        // script directories (subclasses flagged), sorted by name
        static std::vector<ScriptCandidate> findProjectScripts(Project* project);
        // A file a script can be made from (a drop): .lua, a C++ header or source
        static bool isScriptFile(const std::filesystem::path& file);
        // the C++ subclass base `entity` can take (an engine object class), "" when any
        static bool fitsEntity(const ScriptCandidate& candidate, SceneProject* sceneProject, Entity entity);
    };

}

#endif // SCRIPTATTACH_H
