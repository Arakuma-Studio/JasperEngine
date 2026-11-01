// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "util/NativePlugins.h"

#include <filesystem>
#include <string>
#include <vector>

namespace jasperengine::editor {

    class Project;

    // Assets > Create > Native Plugin... and Project Settings > Native Plugins > Create Plugin...:
    // a native plugin for the chosen platforms plus its Lua mock (NativePlugins::create, the
    // files of the native_plugins MCP tool). A modal; show() every frame (also from inside
    // another modal, which nests it).
    class NativePluginDialog {
    public:
        void open(Project* project);
        void show();
        bool isOpen() const { return m_isOpen; }
        // the Lua mock just written (project-relative to absolute), to open in the code editor; once
        std::filesystem::path consumeCreatedFile();

    private:
        bool m_isOpen = false;
        bool m_popupOpened = false;
        Project* m_project = nullptr;
        char m_name[64] = {0};
        char m_package[128] = {0};
        bool m_android = true, m_ios = true, m_macos = false, m_web = true, m_cpp = true;
        int m_androidLanguage = 0;   // java, kotlin
        int m_appleLanguage = 0;     // objc, swift
        struct Method {
            char name[64] = {0};
            bool async = false;
        };
        std::vector<Method> m_methods;
        bool m_overwrite = false;
        std::string m_error;
        std::filesystem::path m_created;

        NativePlugins::CreateOptions options() const;
        void create();
    };

}
