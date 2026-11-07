// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef SHADOWCASTER2D_H
#define SHADOWCASTER2D_H

#include "GameObject.h"
#include "component/ShadowCaster2DComponent.h"

namespace jasperengine{

    class JASPERENGINE_API ShadowCaster2D: public GameObject{
    public:
        ShadowCaster2D(Scene* scene);
        ShadowCaster2D(Scene* scene, Entity entity);
        virtual ~ShadowCaster2D();

        ShadowCaster2D(const ShadowCaster2D& rhs);
        ShadowCaster2D& operator=(const ShadowCaster2D& rhs);
        ShadowCaster2D(ShadowCaster2D&& rhs) noexcept;
        ShadowCaster2D& operator=(ShadowCaster2D&& rhs) noexcept;

        void setShape(ShadowCaster2DShape shape);
        ShadowCaster2DShape getShape() const;

        void addVertex(Vector2 vertex);
        void addVertex(float x, float y);
        void clearVertices();
        unsigned int getVertexCount() const;

        void setClosed(bool closed);
        bool isClosed() const;

        void setEnabled(bool enabled);
        bool isEnabled() const;
    };

}

#endif //SHADOWCASTER2D_H
