// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef LATTICE_RENDER_H
#define LATTICE_RENDER_H

// Per-frame packing of lattice bindings into MeshComponent::lattice (spec §5).

#include "Engine.h"

namespace jasperengine{

    class Scene;

    class JASPERENGINE_API LatticeRender{
    public:
        // called by RenderSystem::update after transforms and bones are current and before
        // the mesh AABB loop; rebuilds LatticeMeshState of every affected mesh
        static void prepare(Scene* scene);
    };

}

#endif //LATTICE_RENDER_H
