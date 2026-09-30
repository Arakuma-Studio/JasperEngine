// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef PRIMITIVE_H
#define PRIMITIVE_H

#include "Mesh.h"

namespace jasperengine{

    class JASPERENGINE_API Primitive: public Mesh{
    public:
        Primitive(Scene* scene);
        Primitive(Scene* scene, Entity entity);
        virtual ~Primitive();

        void createPlane(float width, float depth);
        void createPlane(float width, float depth, unsigned int tiles);

        void createWall(float width, float height);
        void createWall(float width, float height, unsigned int tiles);

        void createBox(float width, float height, float depth);
        void createBox(float width, float height, float depth, unsigned int tiles);

        void createSphere(float radius);
        void createSphere(float radius, unsigned int slices, unsigned int stacks);

        void createCylinder(float radius, float height);
        void createCylinder(float baseRadius, float topRadius, float height);
        void createCylinder(float radius, float height, unsigned int slices, unsigned int stacks);
        void createCylinder(float baseRadius, float topRadius, float height, unsigned int slices, unsigned int stacks);

        void createCapsule(float radius, float height);
        void createCapsule(float baseRadius, float topRadius, float height);
        void createCapsule(float radius, float height, unsigned int slices, unsigned int stacks);
        void createCapsule(float baseRadius, float topRadius, float height, unsigned int slices, unsigned int stacks);

        void createTorus(float radius, float ringRadius);
        void createTorus(float radius, float ringRadius, unsigned int sides, unsigned int rings);
    };
}

#endif //PRIMITIVE_H