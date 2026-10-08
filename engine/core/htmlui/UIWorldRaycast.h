// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef UIWORLDRAYCAST_H
#define UIWORLDRAYCAST_H

#include "Export.h"
#include "Entity.h"
#include "math/Ray.h"
#include "math/Vector2.h"

namespace jasperengine {

    class Scene;

    struct UIWorldHit {
        bool hit = false;
        float distance = 0.0f;   // world units from the ray origin to the hit point
        Vector2 uv;              // TEXCOORD1 at the hit (submesh textureRect applied); (0,0) = top-left texel
    };

    // The texture coordinate a world ray hits on a mesh, for pointer input on a texture-mode
    // HTML document shown on it (phase-1 spec §3.12.3). Every triangle of the entity's
    // MeshComponent (CPU buffers: the component's own, then its external buffers, with
    // per-submesh attributes; indexed or not; TRIANGLES and TRIANGLE_STRIP) is tested in
    // local space (the ray through the inverse model matrix) with a two-sided
    // Möller–Trumbore test; the nearest hit within the ray's length (Ray: the direction's
    // length bounds it) interpolates TEXCOORD1. Skinned and morph-target meshes are tested in
    // their bind pose. No hit for a disabled, invisible or uv-less mesh.
    JASPERENGINE_API UIWorldHit raycastMeshUV(Scene* scene, Entity meshEntity, const Ray& worldRay);

}

#endif //UIWORLDRAYCAST_H
