// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "object/Camera.h"
#include "object/GameObject.h"
#include "object/Primitive.h"
#include "object/ui/Image.h"

#include "render/RenderUtil.h"

namespace jasperengine::editor{

    class TranslateGizmo: public GameObject{
    private:
        bool use2DGizmo;

        Primitive* sphere;
        Primitive* xaxis;
        Primitive* yaxis;
        Primitive* zaxis;
        Primitive* xarrow;
        Primitive* yarrow;
        Primitive* zarrow;
        Primitive* xyrect;
        Primitive* xzrect;
        Primitive* yzrect;

        static const Vector3 centerColor;
        static const Vector3 xaxisColor;
        static const Vector3 yaxisColor;
        static const Vector3 zaxisColor;
        static const Vector3 centerColorHightlight;
        static const Vector3 xaxisColorHightlight;
        static const Vector3 yaxisColorHightlight;
        static const Vector3 zaxisColorHightlight;
        static const float rectAlpha;

    public:
        TranslateGizmo(Scene* scene, bool use2DGizmo);
        virtual ~TranslateGizmo();

        GizmoSideSelected checkHover(const Ray& ray);
    };

}

