// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Catalog.h"
#include "ecs/Entity.h"
#include "yaml-cpp/yaml.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace jasperengine {
    class Scene;
    class EntityRegistry;
}

namespace jasperengine::editor {

    class Project;

    // A prefab instance inside copied GameObjects: its members by (source local, registry)
    // entity, so references into the instance can follow the members a paste recreates.
    struct ClipboardPrefabInstance {
        std::filesystem::path path;
        Entity root = NULL_ENTITY;
        std::vector<std::pair<Entity, Entity>> members;
    };

    // Copied GameObjects: a snapshot, never live references, so a paste still works after
    // the sources were changed or deleted, in another scene or in another editor instance.
    struct GameObjectsClip {
        std::string project;            // Project path the snapshot was taken in
        uint32_t sceneId = 0;           // Scene the GameObjects were copied from
        std::string sceneName;
        YAML::Node roots;               // Sequence of Stream::encodeEntity nodes, hierarchy order
        // The first selectedRoots roots are the copied GameObjects; the rest are the actions
        // animating them and entities they own, pasted along but not selected
        size_t selectedRoots = 0;
        std::vector<ClipboardPrefabInstance> prefabInstances; // Outer instances before nested ones
        // Names of the entities outside the copy that copied components reference. A paste in
        // the source scene keeps such a reference only while that entity still has the name.
        std::unordered_map<Entity, std::string> externalNames;
    };

    // One copied component (Inspector "Copy Component")
    struct ComponentClip {
        std::string project;
        uint32_t sceneId = 0;
        ComponentType type = ComponentType::Transform;
        Entity entity = NULL_ENTITY;    // GameObject it was copied from
        YAML::Node value;               // Full component encode (mesh buffers included)
        std::unordered_map<Entity, std::string> externalNames;
    };

    // How entity references in pasted data are resolved (see EntityClipboard::remapReferences)
    struct ClipboardRemap {
        const std::unordered_map<Entity, Entity>* map = nullptr; // Copied entity -> its paste
        const std::unordered_map<Entity, std::string>* externalNames = nullptr;
        uint32_t sourceSceneId = 0;
        uint32_t targetSceneId = 0;
        bool sameScene = false;         // Same project and scene as the copy
    };

    // Unity-style Cut / Copy / Paste of GameObjects and components through the system
    // clipboard. The text is YAML behind a "# JasperEngine clipboard" header line, so two
    // editor instances of the same project can exchange it and any other text is ignored.
    // The last copy is also kept parsed in memory, and the last text read from the system
    // clipboard is parsed once, so pasting a large copy again does not decode YAML text.
    class EntityClipboard {
    public:
        enum class Kind { None, GameObjects, Component };

        // Where pasted GameObjects go
        enum class Placement {
            Root,       // End of the scene root
            After,      // Next sibling of the anchor, after its children
            Into        // Last child of the anchor
        };

        static constexpr const char* HEADER = "# JasperEngine clipboard";

        // --- GameObjects ---

        // Copies the entities with their whole subtrees (a child selected with its parent is
        // copied once through the parent), prefab instance links and overrides, the actions
        // animating them and the entities their components own. False with a Console warning
        // when nothing can be copied.
        static bool copyGameObjects(Project* project, uint32_t sceneId, const std::vector<Entity>& entities);
        // Copy, then delete the copied GameObjects in one undo step. A paste in the same scene
        // reclaims their ids when still free, so references to them come back: a move.
        static bool cutGameObjects(Project* project, uint32_t sceneId, const std::vector<Entity>& entities);
        // Pastes relative to the selection of the scene: after the last selected GameObject
        // (asChild false; scene root when nothing is selected) or as its last child. One undo
        // step; the pasted GameObjects become the selection.
        static bool pasteGameObjects(Project* project, uint32_t sceneId, bool asChild);
        // Placement for pasteGameObjects, public for the Hierarchy context menu
        static bool pasteGameObjects(Project* project, uint32_t sceneId, Placement placement, Entity anchor);

        // The GameObjects the selection of a scene copies: authored, ordered as the Hierarchy
        // shows them. Empty when nothing is selected there.
        static std::vector<Entity> getCopyableSelection(Project* project, uint32_t sceneId);

        // --- Components ---

        static bool copyComponent(Project* project, uint32_t sceneId, Entity entity, ComponentType type);

        // --- Clipboard state (cheap, for menus drawn every frame) ---

        // What the system clipboard held when it was last probed, restricted to this project
        static Kind getKind(const Project* project);
        static bool hasGameObjects(const Project* project) { return getKind(project) == Kind::GameObjects; }
        static bool hasComponent(const Project* project, ComponentType* type = nullptr);

        // Waits for the text of the last copy to be written: its nodes are then only the
        // main thread's (a Cut's delete snapshot shares them, and decoding may add to them)
        static void finishPendingWrite();

        // Reads the system clipboard now. Called when a menu offering Paste opens and when
        // the editor regains focus; never every frame, a foreign clipboard owner can block.
        static void refresh();
        // Once per frame: notices ImGui writing the clipboard and the editor regaining focus
        static void update();

        // The parsed component clipboard, read now; null when it holds no component from this project
        static std::shared_ptr<const ComponentClip> readComponent(const Project* project);
        // The copied component as last probed, without reading the clipboard (menus drawn every frame)
        static std::shared_ptr<const ComponentClip> peekComponent(const Project* project);

        // --- Reference remapping shared by the paste commands ---

        // Rewrites the Entity/EntityReference properties (and render-to-texture camera links)
        // of one component, then binds those links to their cameras: copied entities
        // become their pastes; references to other entities of the source scene stay when
        // pasting there and that entity still exists under its copied name, and are cleared
        // in another scene; references into a third scene are kept. `skip` names properties
        // left alone. Returns the update flags of the properties that changed.
        static uint64_t remapReferences(Scene* scene, Entity entity, ComponentType type, const ClipboardRemap& remap,
                                        const std::vector<std::string>& skip = {});

        // Binds the render-to-texture links of one component to their camera's current
        // framebuffer (what CameraTextureLink::resolve does for a whole scene). Returns the
        // update flags of the textures that changed.
        static uint64_t bindCameraTextures(EntityRegistry* registry, Entity entity, ComponentType type);

        // Project identity stored in the clipboard
        static std::string projectKey(const Project* project);

        // Full component encode used by Copy Component and for undo (mesh buffers included)
        static YAML::Node encodeComponent(EntityRegistry* registry, Entity entity, ComponentType type);
        // Writes a full component encode over the live component, exactly: a field the encoder
        // leaves out at its default is cleared, as a scene load would have it. The component
        // keeps its runtime state and is refreshed like an edited one.
        static void applyComponentValue(EntityRegistry* registry, Entity entity, ComponentType type, YAML::Node value);
        // The encode of a default-constructed component, what Add Component gives
        static YAML::Node encodeDefaultComponent(ComponentType type);

    private:
        // copyGameObjects, also handing back the encode of each copied root that is not a
        // lone prefab member: the same Stream::encodeEntity a delete snapshots it with
        static bool copyGameObjects(Project* project, uint32_t sceneId, const std::vector<Entity>& entities,
                                    std::unordered_map<Entity, YAML::Node>* encodedRoots);
    };

}
