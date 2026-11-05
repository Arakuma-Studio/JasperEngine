// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef RENDERSYSTEMSHARED_H
#define RENDERSYSTEMSHARED_H

// Helpers RenderSystem.cpp and RenderSystemShadows.cpp both use (engine-internal)

#include "component/InstancedMeshComponent.h"
#include "component/MeshComponent.h"
#include "math/AABB.h"
#include "Scene.h"

#include <algorithm>
#include <cmath>

namespace jasperengine::rendershared {

    // whether the instances' distance fade can cut on the CPU what it shrinks to nothing: the
    // built-in triangle shaders shrink them in every pass (includes/instance_fade.glsl: colour,
    // depth, the SSR G-buffer); a fork may draw them otherwise
    inline bool instanceFadeCuts(const InstancedMeshComponent& instmesh, const MeshComponent& mesh, const Scene& scene){
        if (!scene.getDefaultMeshShader().empty() || !instmesh.distanceFade || !mesh.customShader.empty() || !mesh.customDepthShader.empty()){
            return false;
        }
        for (unsigned int s = 0; s < mesh.numSubmeshes; s++){
            const auto primitive = mesh.submeshes[s].primitiveType;
            if (primitive != PrimitiveType::TRIANGLES && primitive != PrimitiveType::TRIANGLE_STRIP){
                return false;
            }
        }
        return true;
    }

    // fade distance past which instances can be dropped, -1 for none
    inline float instanceFadeCullEnd(const InstancedMeshComponent& instmesh, const MeshComponent& mesh, const Scene& scene){
        if (!(instmesh.fadeEnd > instmesh.fadeStart) || !std::isfinite(instmesh.fadeStart) ||
            !std::isfinite(instmesh.fadeEnd) || instmesh.fadeEnd < 0.0f || !instanceFadeCuts(instmesh, mesh, scene)){
            return -1.0f;
        }
        return instmesh.fadeEnd;
    }

    // near fade distance inside which instances can be dropped (the near fade's start), 0 for none
    inline float instanceFadeCullNear(const InstancedMeshComponent& instmesh, const MeshComponent& mesh, const Scene& scene){
        if (!(instmesh.nearFadeEnd > instmesh.nearFadeStart) || !std::isfinite(instmesh.nearFadeStart) ||
            !std::isfinite(instmesh.nearFadeEnd) || !(instmesh.nearFadeStart > 0.0f) || !instanceFadeCuts(instmesh, mesh, scene)){
            return 0.0f;
        }
        return instmesh.nearFadeStart;
    }

    // the eye movement a list cut at these distances still covers: past the far end and inside the
    // near start the fade has shrunk instances to nothing, so a margin there changes no pixel
    inline float instanceFadeSlackFar(const InstancedMeshComponent& instmesh, float cullEnd){
        return cullEnd >= 0.0f ? std::max(0.25f, 0.04f * (instmesh.fadeEnd - instmesh.fadeStart)) : 0.0f;
    }

    inline float instanceFadeSlackNear(float cullNear){
        return cullNear > 0.0f ? std::min(0.25f, 0.1f * cullNear) : 0.0f;
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
