// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// .mesh assets from the editor's side (docs/specs/S98-editor-tooling-gaps.md F1, Deviations): Save
// Mesh As Asset (the Mesh Renderer's menu, set_mesh_geometry's save_as) and the scene save that
// moves large geometry out of the scene file.

#include "asset/AssetRef.h"
#include "ecs/Entity.h"
#include "math/Vector3.h"

#include <cstddef>
#include <filesystem>
#include <string>

namespace jasperengine {
    class EntityRegistry;
    struct MeshComponent;
}

namespace jasperengine::editor {

    class Project;
    class Command;
    struct SceneProject;

    namespace MeshAssetFiles {

        // A scene save moves a mesh's own geometry of more bytes than this into a .mesh: a quad, a
        // box or a plane (under 2 KB) stays readable in the scene, a sphere (34 KB) does not
        constexpr size_t EXTRACT_ABOVE_BYTES = 4096;

        // Whether the GameObject's Mesh Renderer may take its geometry from a .mesh: not a model's
        // nor a generator's (sprite, tilemap, polygon, terrain, spline). The reason when not.
        bool canShowMeshAsset(EntityRegistry* registry, Entity entity, std::string* reason = nullptr);
        // canShowMeshAsset, with geometry to save
        bool ownsGeometry(EntityRegistry* registry, Entity entity, std::string* reason = nullptr);

        // A project- or assets-relative ".mesh" path as the assets root names it; empty, with the
        // reason, for another extension or a place outside the assets root
        std::string assetsRelativePath(const Project& project, const std::string& given, std::string* error);

        // Writes the geometry to the assets-relative .mesh (atomically, an unchanged file
        // untouched), gives it a GUID (its .meta, S04) and, with `refreshShown`, has every open scene
        // showing that file read it again; `ref` names it. False, with the reason, when it cannot be
        // written.
        bool write(Project& project, const MeshComponent& mesh, const std::string& assetsRelative, AssetRef& ref, std::string* error,
                   bool refreshShown = true);

        // Save Mesh As Asset: writes `mesh` (the GameObject's Mesh Renderer, or one with the geometry
        // made for it) to `given` (a .mesh path, see assetsRelativePath) and returns the undoable
        // step that shows it on the GameObject, for the caller to run; `written` is the
        // assets-relative path. Null, with the reason, when the GameObject's geometry is not its own
        // or the file cannot be written. The file stays when the step is undone.
        Command* saveAs(Project& project, uint32_t sceneId, Entity entity, MeshComponent mesh, const std::string& given, std::string& written,
                        std::string* error);

        // A .mesh dropped in the Hierarchy or the Scene view: a GameObject named after it whose Mesh
        // Renderer shows it, at `position` under `parent`, one undo step "Create <name>" (Unity's mesh
        // drop). NULL_ENTITY, with the reason, when there is no such scene or file.
        Entity createObject(Project& project, uint32_t sceneId, const std::string& path, const Vector3& position, Entity parent, std::string* error);

        // The scene's save (to `sceneFile`): each GameObject of its own that holds more than
        // EXTRACT_ABOVE_BYTES of geometry shows a .mesh from then on, written to "<scene folder>/<scene
        // name>/<GameObject>.mesh" (a scene outside the assets root: "<assets>/Meshes/<scene name>/").
        // A file there with the same bytes is shown again; another one is never written over.
        // Prefab instances keep their prefab's mesh. A failure is reported and leaves the mesh inline.
        // The meshes show their files through one undo step of the scene, "Save Meshes As Assets".
        void extractScene(Project& project, SceneProject& sceneProject, const std::filesystem::path& sceneFile);

    }

}
