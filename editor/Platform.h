// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "System.h"
#include "Project.h"
#include <cstdarg>

namespace jasperengine::editor{

    class Platform : public System{
    private:
        Project* project;

        static int width;
        static int height;

    public:
        Platform(Project* project);

        // the project this Platform resolves paths for (the editor's, the CLI's)
        Project* getProject() const { return project; }

        static bool setSizes(int width, int height);

        int getScreenWidth() override;
        int getScreenHeight() override;

        std::string getAssetPath() override;
        std::string getLuaPath() override;
        std::string getShaderPath() override;
        std::string getUserDataPath() override;
        std::string getImportPath() override;

        sg_environment getSokolEnvironment() override;
        sg_swapchain getSokolSwapchain() override;

        bool isTouchDevice() override;

        void setMouseMode(MouseMode mode) override;

        // the editor's system clipboard (its X11 / Win32 / NSPasteboard backends, through ImGui)
        void setClipboardText(const std::string& utf8) override;
        std::string getClipboardText() override;

        void quit() override;

        void platformLog(const int type, const char *fmt, va_list args) override;
    };

}

