// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef LATTICE_H
#define LATTICE_H

#include "GameObject.h"

namespace jasperengine{

    // Box-shaped cage of control points deforming the meshes bound to it (spec §8).
    class JASPERENGINE_API Lattice: public GameObject{
    public:
        Lattice(Scene* scene);
        Lattice(Scene* scene, Entity entity);
        virtual ~Lattice();

        Lattice(const Lattice& rhs);
        Lattice& operator=(const Lattice& rhs);
        Lattice(Lattice&& rhs) noexcept;
        Lattice& operator=(Lattice&& rhs) noexcept;
    };

}

#endif //LATTICE_H
