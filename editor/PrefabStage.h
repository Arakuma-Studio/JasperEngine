// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// Prefab Mode (Unity's prefab stages): the logic is Project's (editor/PrefabStage.cpp, the
// PrefabStage struct is in Project.h), this is what draws with it.

#include "Scene.h"
#include "object/Camera.h"
#include "object/ui/Polygon.h"

namespace jasperengine::editor {

    // The Gray context of Prefab Mode In Context: a translucent veil drawn over the context
    // scene, under the stage (a UI scene of its own, one rectangle covering the view)
    class PrefabContextOverlay {
    private:
        Scene* scene = nullptr;
        Camera* camera = nullptr;
        Polygon* veil = nullptr;
        int width = 0;
        int height = 0;

    public:
        PrefabContextOverlay();
        ~PrefabContextOverlay();
        PrefabContextOverlay(const PrefabContextOverlay&) = delete;
        PrefabContextOverlay& operator=(const PrefabContextOverlay&) = delete;

        // Covers a view of width x height (engine pixels)
        void resize(int width, int height);
        Scene* getScene() { return scene; }
    };

}
