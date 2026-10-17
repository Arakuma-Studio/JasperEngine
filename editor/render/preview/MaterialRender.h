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
        // what applyMaterial was given (the sphere draws an adjusted copy)
        Material material;
    public:
        MaterialRender();
        ~MaterialRender();

        MaterialRender(const MaterialRender&) = delete;
        MaterialRender& operator=(const MaterialRender&) = delete;

        // meshShader: the shader the material draws with when it names none (the owning
        // mesh's customShader or the scene default; "" for the built-in Lit). A colour write
        // without alpha (the material's or its extra pass's, explicit or the shader's @state)
        // draws alpha too: the preview composes with its alpha, cleared to 0, so it would show
        // transparent instead of the colour the game shows on its opaque target.
        void applyMaterial(const Material& material, const std::string& meshShader = std::string());
        // the material last given to applyMaterial
        const Material& getMaterial() const;
        std::string getMeshShader() const;

        // the sphere still waits for its shader variant (a library shader or fork builds off the
        // UI thread) or a texture: what was drawn is not the material yet
        bool isPending() const;

        void setReceiveIBL(bool value);
        bool getReceiveIBL() const;

        Framebuffer* getFramebuffer();
        Texture getTexture();
        Scene* getScene();
    };

}
