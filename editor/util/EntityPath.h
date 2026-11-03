// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "registry/EntityRegistry.h"

#include <map>
#include <string>
#include <vector>

namespace jasperengine::editor {

    struct SceneProject;

    // A GameObject's hierarchy path, the address scene-dump and the MCP server share:
    // the names from the scene root joined by '/', each followed by "#N" when it is the
    // Nth same-named sibling after the first (in Transform order), e.g. "Player/Gun",
    // "Enemy#2". Stable across saves and reloads, unlike entity ids of new GameObjects.
    class EntityPath {
    public:
        // Paths already worked out, by entity: a caller building many paths of one
        // registry passes the same cache (valid while the hierarchy does not change)
        using Cache = std::map<Entity, std::string>;

        // The path of entity in registry
        static std::string of(EntityRegistry* registry, Entity entity, Cache* cache = nullptr);
        static std::string of(const SceneProject& sceneProject, Entity entity, Cache* cache = nullptr);

        // The entity of `entities` (the scene's GameObjects, SceneProject::entities, or
        // EntityRegistry::getEntityList() for a registry without a SceneProject) whose path
        // is exactly `path`, NULL_ENTITY when none
        static Entity find(EntityRegistry* registry, const std::vector<Entity>& entities, const std::string& path);
        static Entity find(const SceneProject& sceneProject, const std::string& path);

        // Every entity of `entities` named `name` (a plain GameObject name, at any depth)
        static std::vector<Entity> findByName(EntityRegistry* registry, const std::vector<Entity>& entities, const std::string& name);

        // What a reference typed by a user or an agent means: the exact path when one
        // matches, otherwise every GameObject named `ref` anywhere in the hierarchy. Empty:
        // not found; one: found; more: ambiguous (list them by of() so the caller can pick).
        static std::vector<Entity> resolve(EntityRegistry* registry, const std::vector<Entity>& entities, const std::string& ref);
        static std::vector<Entity> resolve(const SceneProject& sceneProject, const std::string& ref);

        // ---- Paths relative to an animated root: the clip binding grammar (animation clips spec
        // 2.2, 9.R39). One grammar with of() / find() above, implemented by the engine's
        // anim::relativePath / anim::resolvePath (ClipBinding), so a clip path is exactly the
        // scene path below the root: of(entity) == of(root) + "/" + relative(root, entity). ----

        // The path of `entity` below `root` ("" for the root itself); false when it is not the
        // root or under it, or a name on the way can't be addressed (isAddressableName)
        static bool relative(EntityRegistry* registry, Entity root, Entity entity, std::string& out);
        // The entity at `path` below `root`, NULL_ENTITY when none (or a malformed path)
        static Entity findRelative(EntityRegistry* registry, Entity root, const std::string& path);
        // False for names a path can't address: empty, containing '/' or ending in "#<digits>"
        // (such a name reads back as a sibling index). Add Property and keying warn about them.
        static bool isAddressableName(const std::string& name);
        // The warning for such a name (empty when it is addressable)
        static std::string unaddressableReason(const std::string& name);
    };

}
