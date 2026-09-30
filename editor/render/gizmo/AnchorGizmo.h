// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "object/GameObject.h"
#include "object/ui/Polygon.h"
#include "math/Rect.h"

namespace jasperengine::editor{

    class AnchorGizmo: public GameObject{
    private:
        GameObject* center;
        Polygon* points[4];

        Rect area;

        float anchorLeft;
        float anchorTop;
        float anchorRight;
        float anchorBottom;

        void updateVisual();

    public:
        AnchorGizmo(Scene* scene);
        virtual ~AnchorGizmo();

        void setArea(Rect area);
        void setAnchors(float left, float top, float right, float bottom);
    };

}
