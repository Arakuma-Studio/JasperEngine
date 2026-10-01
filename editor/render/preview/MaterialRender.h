// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Scene.h"
#include "object/Camera.h"
#include "object/GameObject.h"
#include "object/Primitive.h"
#include "object/Light.h"
#include "object/environment/Skybox.h"

namespace jasperengine::editor{

    class MaterialRender{
    private:
        Scene* scene;
        Camera* camera;

        Light* light;
        Skybox* sky;

        Primitive* sphere;
        bool receiveIBL = true;
    public:
        MaterialRender();
        ~MaterialRender();

        MaterialRender(const MaterialRender&) = delete;
        MaterialRender& operator=(const MaterialRender&) = delete;

        void applyMaterial(const Material& material);
        const Material getMaterial();

        void setReceiveIBL(bool value);
        bool getReceiveIBL() const;

        Framebuffer* getFramebuffer();
        Texture getTexture();
        Scene* getScene();
    };

}
