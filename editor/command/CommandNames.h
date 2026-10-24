// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "ecs/Entity.h"
#include "Catalog.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace jasperengine::editor{

    class Project;

    // The words of undo step names (Command::describe), Unity's: "<Verb> <subject>", the
    // subject a GameObject's name or "N Objects", a file's name or "N Files"
    namespace CommandNames{

        // Longest name a history keeps, in UTF-8 code points
        constexpr size_t MAX_LENGTH = 80;
        // " → " of renames ("Rename Player → Hero"), UTF-8 spelled out for every compiler
        constexpr const char* ARROW = " \xE2\x86\x92 ";

        // `name` cut to MAX_LENGTH code points, the cut marked with an ellipsis
        std::string clip(const std::string& name);

        // "Player" for one, "3 Objects" for more, "Nothing" for none
        std::string objects(const std::vector<std::string>& names);
        // The GameObject's name now ("GameObject" when it does not exist)
        std::string entity(Project* project, uint32_t sceneId, Entity entity);
        // The GameObjects' subject now: entity() of one, "N Objects" of more
        std::string entities(Project* project, uint32_t sceneId, const std::vector<Entity>& entities);
        // The scene's name ("Scene" when it is not loaded)
        std::string scene(Project* project, uint32_t sceneId);
        // "N Objects" (N > 1) or the one name
        std::string count(size_t count, const std::string& one, const char* plural = "Objects");

        // The inspector label of a Catalog property: its last path segment, title-cased
        // ("castShadows" -> "Cast Shadows", "submeshes[0].material.baseColorFactor" ->
        // "Base Color Factor", "scripts[1].speed" -> "Speed")
        std::string property(const std::string& propertyName);
        // The component's Unity name ("Light", "Rigidbody 2D")
        std::string component(ComponentType type);

        // "a.png" (a folder's name for "dir/"); for several paths "a.png" for one, "N Files" for more
        std::string file(const std::filesystem::path& path);
        std::string files(const std::vector<std::filesystem::path>& paths);

        // "Verb subject", or the verb alone when the subject is empty
        std::string verb(const std::string& verb, const std::string& subject);

    }

}
