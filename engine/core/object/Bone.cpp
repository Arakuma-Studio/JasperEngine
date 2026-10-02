// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "Bone.h"


using namespace jasperengine;

Bone::Bone(Scene* scene, Entity entity): GameObject(scene, entity){
}

Bone::~Bone() = default;
Bone::Bone(const Bone& rhs) = default;
Bone& Bone::operator=(const Bone& rhs) = default;
