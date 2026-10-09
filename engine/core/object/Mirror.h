// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef MIRROR_H
#define MIRROR_H

#include "Primitive.h"

namespace jasperengine{

    // Flat mesh that reflects the scene, see MirrorComponent. createWall matches
    // the default normal, so a mirror needs no rotation.
    class JASPERENGINE_API Mirror: public Primitive{
    public:
        Mirror(Scene* scene);
        Mirror(Scene* scene, Entity entity);
        virtual ~Mirror();

        Mirror(const Mirror& rhs);
        Mirror& operator=(const Mirror& rhs);
        Mirror(Mirror&& rhs) noexcept;
        Mirror& operator=(Mirror&& rhs) noexcept;

        void setNormal(Vector3 normal);
        void setNormal(const float x, const float y, const float z);
        Vector3 getNormal() const;
    };

}

#endif //MIRROR_H
