// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef LATTICESYSTEM_H
#define LATTICESYSTEM_H

// Lattice runtime (spec §6): pose tweens, jiggle springs, followers, the sprite
// tessellation request. Mesh deformation itself happens on the GPU (LatticeRender).

#include "SubSystem.h"

namespace jasperengine{

    class JASPERENGINE_API LatticeSystem : public SubSystem {
    public:
        LatticeSystem(Scene* scene);
        virtual ~LatticeSystem();

        // called by RenderSystem::update once transforms are current (spec §5.6): followers
        void lateUpdate();

        void load() override;
        void draw() override;
        void destroy() override;
        void update(double dt) override;
    };

}

#endif //LATTICESYSTEM_H
