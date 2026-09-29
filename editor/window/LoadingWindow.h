// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "thread/ResourceProgress.h"
#include "texture/Texture.h"
#include <string>

namespace doriax::editor {

    class LoadingWindow {
    private:
        bool wasShowing = false;
        bool logoLoaded = false;
        Texture logo;

    public:
        LoadingWindow();
        ~LoadingWindow();

        void show();

    private:
        void drawProjectOverlay(const std::string& status);
        void drawProgressModal(const OverallBuildProgress& progress);
    };

}
