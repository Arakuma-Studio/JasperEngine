// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef BONE_COMPONENT_H
#define BONE_COMPONENT_H

#include "Export.h"
#include "ecs/Entity.h"
#include "math/Matrix4.h"
#include "math/Quaternion.h"
#include "math/Vector3.h"
#include <string>

namespace jasperengine{

    struct JASPERENGINE_API BoneComponent{
        Entity model = NULL_ENTITY;

        int index = 0;

        Vector3 bindPosition;
        Quaternion bindRotation;
        Vector3 bindScale;

        Matrix4 offsetMatrix; // inverse bind matrix
    };

}

#endif //BONE_COMPONENT_H