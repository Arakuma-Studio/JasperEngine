// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "Lattice.h"

using namespace jasperengine;

Lattice::Lattice(Scene* scene): GameObject(scene){
    addComponent<LatticeComponent>();
}

Lattice::Lattice(Scene* scene, Entity entity): GameObject(scene, entity){
}

Lattice::~Lattice(){
}

Lattice::Lattice(const Lattice& rhs) = default;
Lattice& Lattice::operator=(const Lattice& rhs) = default;
Lattice::Lattice(Lattice&& rhs) noexcept = default;
Lattice& Lattice::operator=(Lattice&& rhs) noexcept = default;
