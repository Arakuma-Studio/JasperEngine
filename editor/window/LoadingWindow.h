// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "thread/ResourceProgress.h"
#include "texture/Texture.h"
#include <string>

namespace jasperengine::editor {

    class LoadingWindow {
    private:
        bool wasShowing = false;
        bool logoLoaded = false;
        Texture logo;

    public:
        void show();

    private:
        void drawProjectOverlay(const std::string& status);
        void drawProgressModal(const OverallBuildProgress& progress);
    };

}
