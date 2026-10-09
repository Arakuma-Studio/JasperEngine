// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef MESHPOLYGON_H
#define MESHPOLYGON_H

#include "Mesh.h"

namespace jasperengine{
    class JASPERENGINE_API MeshPolygon: public Mesh{

    public:
        MeshPolygon(Scene* scene);
        MeshPolygon(Scene* scene, Entity entity);
        virtual ~MeshPolygon();

        MeshPolygon(const MeshPolygon& rhs);
        MeshPolygon& operator=(const MeshPolygon& rhs);
        MeshPolygon(MeshPolygon&& rhs) noexcept;
        MeshPolygon& operator=(MeshPolygon&& rhs) noexcept;

        bool createPolygon();

        void addVertex(Vector3 vertex);
        void addVertex(float x, float y);

        void clearVertices();

        unsigned int getWidth();
        unsigned int getHeight();

        void setFlipY(bool flipY);
        bool isFlipY() const;
    };
}

#endif //MESHPOLYGON_H