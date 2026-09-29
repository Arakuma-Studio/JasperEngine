// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef BONE_H
#define BONE_H

#include "Object.h"

namespace jasperengine{

    class JASPERENGINE_API Bone: public Object{
    public:
        Bone(Scene* scene, Entity entity);
        virtual ~Bone();

        Bone(const Bone& rhs);
        Bone& operator=(const Bone& rhs);
    };
}

#endif //BONE_H