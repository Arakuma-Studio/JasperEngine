// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef BONE_H
#define BONE_H

#include "GameObject.h"

namespace jasperengine{

    class JASPERENGINE_API Bone: public GameObject{
    public:
        Bone(Scene* scene, Entity entity);
        virtual ~Bone();

        Bone(const Bone& rhs);
        Bone& operator=(const Bone& rhs);
        Bone(Bone&& rhs) noexcept;
        Bone& operator=(Bone&& rhs) noexcept;
    };
}

#endif //BONE_H