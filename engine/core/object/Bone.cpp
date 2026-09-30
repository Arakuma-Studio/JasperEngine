// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "Bone.h"


using namespace jasperengine;

Bone::Bone(Scene* scene, Entity entity): GameObject(scene, entity){
}

Bone::~Bone(){
}

Bone::Bone(const Bone& rhs): GameObject(rhs){
}

Bone& Bone::operator=(const Bone& rhs){
    GameObject::operator =(rhs);

    return *this;
}

