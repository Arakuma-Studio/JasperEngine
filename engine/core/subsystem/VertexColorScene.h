// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// docs/specs/V1-vertex-colors.md R1, R2, R9-R16, D12: the vertex color targets of a scene's entities,
// as the render path, Lua (VertexColors), the editor facade and MCP see them. Main thread.
// An entity's targets are the primitives of its ModelComponent (glTF node + primitive; with Merge
// Static Meshes, its merged submeshes) or the submeshes of its own MeshComponent.

#include "Export.h"
#include "ecs/Entity.h"
#include "math/Matrix4.h"
#include "vertexcolor/VertexColorMesh.h"
#include "vertexcolor/VertexColorTypes.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine {
    class Scene;
    struct Material;
}

namespace jasperengine::vertexcolor {

    struct SceneTarget {
        TargetKey key;
        std::string name;               // "Body (node 3, primitive 0)", "Submesh 0"
        Entity meshEntity = NULL_ENTITY; // the entity whose MeshComponent draws it (a Model's mesh-node child, or the entity)
        uint32_t submesh = 0;           // its submesh index in that MeshComponent
        uint32_t vertexCount = 0;
        uint32_t triangleCount = 0;
        bool hasAssetColors = false;
        std::string material;
        bool faceCulling = true;        // its submesh culls back faces
    };

    // R1: why the entity cannot hold vertex colors ("" when it can): no Model/Mesh, or a mesh another
    // system generates (names the generator)
    JASPERENGINE_API std::string paintRefusal(Scene* scene, Entity entity);

    // the paintable targets of an entity, in key order; empty when refused or not loaded yet
    JASPERENGINE_API std::vector<SceneTarget> sceneTargets(Scene* scene, Entity entity);

    // the target's geometry in local bind pose, built (MeshView::build done). Cached per mesh load
    // (the same pointer until the mesh reloads); null when not loaded. Uses CPU geometry
    // (ModelCpuData::ensure for models, R83).
    JASPERENGINE_API std::shared_ptr<const MeshView> sceneMeshView(Scene* scene, Entity entity, const TargetKey& key);

    // the material the target draws with (its submesh's), so callers need not include the mesh
    // component; null when the submesh is gone
    JASPERENGINE_API const Material* sceneTargetMaterial(Scene* scene, const SceneTarget& target);

    // local -> world of the target in its current transform (bind pose geometry, D12)
    JASPERENGINE_API Matrix4 sceneTargetModel(Scene* scene, Entity entity, const TargetKey& key);

    // the component's live data changed (editor edits, Lua setValues/applyOps): bumps revision so the
    // render path re-uploads the touched targets next frame (R14)
    JASPERENGINE_API void markVertexColorsChanged(Scene* scene, Entity entity);

    // R12, R124: the split vertices the render path bound for the target (0: none or not bound yet)
    JASPERENGINE_API uint32_t boundSplitVertices(Scene* scene, Entity entity, const TargetKey& key);

    // R10, R92: the target's stored fingerprint against the loaded mesh. topologyOnly: what runtime
    // and exports check; false also compares the coarse position hash (editor)
    JASPERENGINE_API bool targetMatchesMesh(Scene* scene, Entity entity, const TargetKey& key, bool topologyOnly);

}
