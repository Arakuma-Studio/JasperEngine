// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "System.h"
#include "Project.h"
#include <cstdarg>
#include <string>
#include <vector>

namespace jasperengine::editor{

    class Platform : public System{
    private:
        Project* project;

        static int width;
        static int height;

    public:
        Platform(Project* project);

        static bool setSizes(int width, int height);

        int getScreenWidth() override;
        int getScreenHeight() override;

        std::string getAssetPath() override;
        std::string getLuaPath() override;
        std::string getShaderPath() override;
        std::string getUserDataPath() override;

        sg_environment getSokolEnvironment() override;
        sg_swapchain getSokolSwapchain() override;

        bool isTouchDevice() override;

        void setMouseMode(MouseMode mode) override;

        // the editor's system clipboard (its X11 / Win32 / NSPasteboard backends, through ImGui)
        void setClipboardText(const std::string& utf8) override;
        std::string getClipboardText() override;

        void quit() override;

        // Starts a program and does not wait for it: argv[0] is looked up on PATH, the
        // arguments are passed as they are (no shell, so paths with spaces stay one argument),
        // and the program outlives the editor. False (and why) when it could not start.
        static bool launchDetached(const std::vector<std::string>& argv, std::string* error = nullptr);
        // Opens a file with the program the system associates with it (xdg-open, open,
        // ShellExecute)
        static bool openWithSystemDefault(const std::string& path, std::string* error = nullptr);

    };

}

