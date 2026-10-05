// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Scene.h"
#include "object/Camera.h"
#include "object/GameObject.h"
#include "object/Primitive.h"
#include "math/Vector3.h"

namespace jasperengine::editor{

    enum class ViewportGizmoAxis {
        NONE,
        POSITIVE_X,
        NEGATIVE_X,
        POSITIVE_Y,
        NEGATIVE_Y,
        POSITIVE_Z,
        NEGATIVE_Z
    };

    class ViewportGizmo{
    private:
        Scene* scene;
        Camera* camera;

        GameObject* mainObject;

        Primitive* cube;
        Primitive* xaxis;
        Primitive* yaxis;
        Primitive* zaxis;
        Primitive* xarrow;
        Primitive* yarrow;
        Primitive* zarrow;
    public:
        ViewportGizmo();
        virtual ~ViewportGizmo();

        void applyRotation(Camera* sceneCam);
        void setFramebufferSize(int size);

        ViewportGizmoAxis hitTest(float normalizedX, float normalizedY) const;

        Framebuffer* getFramebuffer();
        Scene* getScene();
    };

}

