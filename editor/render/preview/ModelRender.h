// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Scene.h"
#include "object/Camera.h"
#include "object/Model.h"
#include "object/Light.h"

namespace jasperengine::editor{

    class ModelRender{
    private:
        Scene* scene = nullptr;
        Camera* camera = nullptr;
        Light* light = nullptr;
        Model* model = nullptr;

        void clearAndInitScene();

    public:
        ModelRender() = default;
        ~ModelRender();

        ModelRender(const ModelRender&) = delete;
        ModelRender& operator=(const ModelRender&) = delete;

        void clearScene();
        bool loadModel(const std::string& filename);
        void positionCameraForModel();
        void fixDarkMaterials();

        Framebuffer* getFramebuffer();
        Scene* getScene();
    };

}
