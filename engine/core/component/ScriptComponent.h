// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef SCRIPT_COMPONENT_H
#define SCRIPT_COMPONENT_H

#include "ScriptProperty.h"
#include <cstdint>
#include <vector>

namespace jasperengine{

    class ScriptBase;

    enum class ScriptType {
        CPP,
        LUA,
    };

    struct JASPERENGINE_API ScriptEntry {
        ScriptType type = ScriptType::CPP;
        std::string path;        // .cpp or .lua (for Lua: script file path)
        std::string headerPath;  // for C++; empty for Lua
        std::string className;   // C++ class or Lua module name (file base name)
        bool enabled = false;
        std::vector<ScriptProperty> properties;

        void* instance = nullptr; // C++ instance OR Lua handle
        // C++: the ScriptBase of the instance (null when it is not one) and the script messages
        // it overrides (ScriptMessages::cppMask), set by the generated script factory
        ScriptBase* cppScript = nullptr;
        uint32_t cppMessageMask = 0;
    };

    struct JASPERENGINE_API ScriptComponent {
        std::vector<ScriptEntry> scripts;
    };

}

#endif //SCRIPT_COMPONENT_H
