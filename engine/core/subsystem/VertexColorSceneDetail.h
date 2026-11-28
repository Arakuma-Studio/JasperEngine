// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// docs/specs/V1-vertex-colors.md R2, R11, R12: what VertexColorScene, the render path
// (VertexColorSystem) and Lua (VertexColors) share about a target's mesh: where it is drawn and
// how its CPU geometry is read. Main thread.

#include "Export.h"
#include "ecs/Entity.h"
#include "buffer/Attribute.h"
#include "render/Render.h"
#include "subsystem/VertexColorScene.h"

#include <cstdint>
#include <vector>

namespace jasperengine {
    class Buffer;
    struct VertexColorsComponent;
    struct MeshComponent;
    struct Submesh;
}

namespace jasperengine::vertexcolor::detail {

    // one vertex input of a submesh: the per-submesh attribute, else the mesh's interleaved one
    struct AttributeSource {
        const Buffer* buffer = nullptr;
        Attribute attribute;
        explicit operator bool() const { return buffer != nullptr; }
    };

    JASPERENGINE_API AttributeSource findAttribute(const MeshComponent& mesh, const Submesh& submesh, AttributeType type);

    // the GPU vertices the submesh's inputs address
    JASPERENGINE_API uint32_t submeshVertexCount(const MeshComponent& mesh, const Submesh& submesh);

    // the submesh's triangle list (its indices, or 0..n-1 unindexed); false when it is not a
    // triangle list or an index is out of range
    JASPERENGINE_API bool readSubmeshIndices(const MeshComponent& mesh, const Submesh& submesh, uint32_t vertexCount, std::vector<uint32_t>& out);

    // `elements` floats of an attribute at a vertex, normalized and half values decoded; false
    // past the buffer's end or for a type it cannot read
    JASPERENGINE_API bool readAttribute(const AttributeSource& source, uint32_t vertex, unsigned int elements, float* out);

    // what identifies the geometry of a submesh as loaded now: changes when its buffers, their
    // bytes' source or its index range change (a model reload, a primitive rebuilt); not when a
    // game frees the CPU copy of the bytes (ModelCpuData)
    JASPERENGINE_API uint64_t geometryStamp(const MeshComponent& mesh, unsigned int submesh);

    // the entity's targets with the mesh each draws in, in key order (as sceneTargets)
    JASPERENGINE_API std::vector<SceneTarget> resolveTargets(Scene* scene, Entity entity);
    // one target (false: the entity has none of that key now)
    JASPERENGINE_API bool resolveTarget(Scene* scene, Entity entity, const TargetKey& key, SceneTarget& out);

    // the component's .vcolors data in `loaded` (kept when it already shows its file: the editor's
    // live values); read through the engine's files (resource packs, Android assets, the web's
    // preloaded files). False when it cannot be read (logged; loaded empty). `read`: it was read now
    JASPERENGINE_API bool loadComponentData(Scene* scene, Entity entity, VertexColorsComponent& colors, bool* read = nullptr);

    // the component's data to write (R125): read first if not yet, made when there is none, copied
    // when another holder shares it (a copy of the GameObject, the editor's scene during Play)
    JASPERENGINE_API Data& writableData(Scene* scene, Entity entity, VertexColorsComponent& colors);

    // the MeshView of a submesh read from its CPU geometry, built; false with `error`
    JASPERENGINE_API bool buildMeshView(const MeshComponent& mesh, unsigned int submesh, MeshView& out, std::string& error);

}
