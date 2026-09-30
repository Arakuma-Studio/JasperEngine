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
        virtual ~DirectionRender();

        DirectionRender(const DirectionRender&) = delete;
        DirectionRender& operator=(const DirectionRender&) = delete;
        DirectionRender(DirectionRender&&) noexcept = default;
        DirectionRender& operator=(DirectionRender&&) noexcept = default;

        void setDirection(Vector3 direction);
        void setDirection(float x, float y, float z);
        void setColor(Vector4 color);
        void setColor(float r, float g, float b, float a = 1.0f);
        void setBackground(Vector4 color);

        Vector3 getDirection();

        Framebuffer* getFramebuffer();
        Texture getTexture();
        Scene* getScene();
        GameObject* getGameObject();
    };

}