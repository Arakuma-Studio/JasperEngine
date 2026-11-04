// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef INSTANCEDATA_H
#define INSTANCEDATA_H

#include "math/Quaternion.h"
#include "math/Rect.h"
#include "math/Vector3.h"
#include "math/Vector4.h"

namespace jasperengine{

    struct InstanceData{
        Vector3 position = Vector3(0.0, 0.0, 0.0);
        Quaternion rotation;
        Vector3 scale = Vector3(1.0, 1.0, 1.0);
        Vector4 color = Vector4(1.0, 1.0, 1.0, 1.0);  //linear color;
        Rect textureRect = Rect(0.0, 0.0, 1.0, 1.0);
        bool visible = true;
        // Vertex Animator state of this instance (vat::vatPackInstance), written by
        // VertexAnimatorSystem, not serialized; the default packs "no clip" (rest pose)
        Vector4 vat0 = Vector4(-1.0, 0.0, 1.0, -1.0);
        Vector4 vat1 = Vector4(0.0, 1.0, 0.0, 0.0);
        // a particle's (InstanceParticleRender alignment): its billboard roll in radians (the z
        // of its rotation) and its velocity in the world, not serialized. Kept readable for
        // other renderers of the particles (the HTML UI's <particle-system>)
        float roll = 0.0f;
        Vector3 velocity = Vector3(0.0, 0.0, 0.0);
    };

}

#endif //INSTANCEDATA_H
