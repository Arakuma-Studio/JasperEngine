// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef RENDERSYSTEMSHARED_H
#define RENDERSYSTEMSHARED_H

// Helpers RenderSystem.cpp and RenderSystemShadows.cpp both use (engine-internal)

#include "component/InstancedMeshComponent.h"
#include "component/MeshComponent.h"
#include "math/AABB.h"
#include "Scene.h"

#include <cmath>

namespace jasperengine::rendershared {

    // fade distance past which instances can be dropped: only the built-in triangle
    // shaders shrink them to nothing, and the SSR G-buffer has no fade yet
    inline float instanceFadeCullEnd(const InstancedMeshComponent& instmesh, const MeshComponent& mesh, const Scene& scene){
        if (scene.isSSREnabled() || !scene.getDefaultMeshShader().empty() ||
            !instmesh.distanceFade || !mesh.customShader.empty() || !mesh.customDepthShader.empty() ||
            !(instmesh.fadeEnd > instmesh.fadeStart) || !std::isfinite(instmesh.fadeStart) ||
            !std::isfinite(instmesh.fadeEnd) || instmesh.fadeEnd < 0.0f){
            return -1.0f;
        }
        for (unsigned int s = 0; s < mesh.numSubmeshes; s++){
            const auto primitive = mesh.submeshes[s].primitiveType;
            if (primitive != PrimitiveType::TRIANGLES && primitive != PrimitiveType::TRIANGLE_STRIP){
                return -1.0f;
            }
        }
        return instmesh.fadeEnd;
    }

    // shadow-map texels of error a caster may lose, well inside the PCF blur
    inline constexpr float SHADOW_LOD_TEXELS = 2.0f;
    // shadow maps cast from about one detail level coarser than the camera's rule picks (spec S54
    // R7, the default Shadow LOD Bias 1 keeps it)
    inline constexpr float SHADOW_LOD_COARSER = 2.0f;

    // an instanced mesh with culling off keeps its whole batch, bounds or not
    inline bool cullsBatch(const MeshComponent& mesh, const InstancedMeshComponent* instmesh){
        return mesh.worldAABB != AABB::ZERO && (!instmesh || instmesh->cullInstances);
    }

    // a VAT batch's states parallel to its render list, null for any other batch (or one whose
    // list is shorter: no state is read past it)
    inline const InstanceVatRenderData* instanceVatData(const InstancedMeshComponent& instmesh){
        if (instmesh.renderVat.empty() || instmesh.renderVat.size() < instmesh.numVisible)
            return nullptr;
        return instmesh.renderVat.data();
    }

}

#endif // RENDERSYSTEMSHARED_H
