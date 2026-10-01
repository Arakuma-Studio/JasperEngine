// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Scene.h"
#include "object/Camera.h"
#include "object/Light.h"

#include <filesystem>

namespace jasperengine::editor{

    class Project;

    // Offscreen render of a prefab for its Project window thumbnail: an instance of it
    // (nested prefabs and their overrides included, Project::instantiatePrefabPreview) in a
    // scene of its own, framed by a perspective camera, or an orthographic one for a 2D or
    // UI prefab. Main thread only: the instance is built with the project's prefab data.
    class PrefabRender{
    private:
        Scene* scene = nullptr;
        Camera* camera = nullptr;
        Light* light = nullptr;
        Entity root = NULL_ENTITY;
        bool flat = false;

    public:
        PrefabRender() = default;
        ~PrefabRender();

        PrefabRender(const PrefabRender&) = delete;
        PrefabRender& operator=(const PrefabRender&) = delete;

        // A new scene holding an instance of the prefab (project-relative path)
        bool build(Project* project, const std::filesystem::path& prefabPath);
        // Points the camera at what the instance draws: call after a frame was drawn (the
        // systems compute the world bounds)
        void frame();
        void clear();

        bool isFlat() const { return flat; }
        Framebuffer* getFramebuffer();
        Scene* getScene();
    };

}
