// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Scene.h"
#include "object/Camera.h"
#include "object/GameObject.h"
#include "object/Primitive.h"
#include "object/Light.h"

namespace jasperengine::editor{

    class DirectionRender{
    private:
        Scene* scene;
        Camera* camera;
        Light* light;

        GameObject* arrowObject;
        Primitive* arrowShaft;
        Primitive* arrowHead;

        Vector3 direction;

    public:
        DirectionRender();
        ~DirectionRender();

        DirectionRender(const DirectionRender&) = delete;
        DirectionRender& operator=(const DirectionRender&) = delete;

        void setDirection(Vector3 direction);

        Vector3 getDirection();

        Framebuffer* getFramebuffer();
        Texture getTexture();
        Scene* getScene();
    };

}
