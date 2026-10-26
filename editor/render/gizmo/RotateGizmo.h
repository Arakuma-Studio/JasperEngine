// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "object/Camera.h"
#include "object/GameObject.h"
#include "object/Primitive.h"
#include "object/ui/Image.h"
#include "object/LineRenderer.h"

#include "render/RenderUtil.h"

namespace jasperengine::editor{

    class RotateGizmo: public GameObject{
    private:
        bool use2DGizmo;

        Primitive* maincircle;
        Primitive* xcircle;
        Primitive* ycircle;
        Primitive* zcircle;

        LineRenderer* line;

        std::vector<AABB> xcircleAABBs;
        std::vector<AABB> ycircleAABBs;
        std::vector<AABB> zcircleAABBs;

        static const Vector3 mainColor;
        static const Vector3 xaxisColor;
        static const Vector3 yaxisColor;
        static const Vector3 zaxisColor;
        static const Vector3 lineColor;
        static const Vector3 mainColorHightlight;
        static const Vector3 xaxisColorHightlight;
        static const Vector3 yaxisColorHightlight;
        static const Vector3 zaxisColorHightlight;
        static const float circleAlpha;

        std::vector<AABB> createHalfTorus(Entity entity, float radius, float ringRadius, unsigned int sides, unsigned int rings);

        // Physics (G) is on: colours from physicsGizmoColor
        bool physicsTint = false;
        Vector4 paint(const Vector3& color, float alpha) const { return physicsGizmoColor(color, alpha, physicsTint); }

    public:
        RotateGizmo(Scene* scene, bool use2DGizmo);
        virtual ~RotateGizmo();

        void updateRotations(Camera* camera);
        void drawLine(Vector3 point);
        void removeLine();

        GizmoSideSelected checkHover(const Ray& ray);

        // Takes effect with the next checkHover, which repaints every handle
        void setPhysicsTint(bool tint) { physicsTint = tint; }
    };

}

