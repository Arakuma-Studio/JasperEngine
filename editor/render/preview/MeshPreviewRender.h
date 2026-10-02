// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Scene.h"
#include "object/Camera.h"
#include "object/GameObject.h"
#include "object/Primitive.h"
#include "object/Light.h"

#include "yaml-cpp/yaml.h"

namespace jasperengine::editor{

    class MeshPreviewRender{
    private:
        Scene* scene;
        Camera* camera;

        Light* light;

        Mesh* mesh;

        void positionCameraForMesh();

    public:
        MeshPreviewRender();
        ~MeshPreviewRender();

        MeshPreviewRender(const MeshPreviewRender&) = delete;
        MeshPreviewRender& operator=(const MeshPreviewRender&) = delete;

        void applyMesh(YAML::Node meshData, bool updateCamera = true, bool removeMaterial = false);
        void setBackground(Vector4 color);

        Texture getTexture();
        Scene* getScene();
    };

}
