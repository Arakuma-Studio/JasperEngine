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

        // Runtime only, never serialized; a copy (EntityCloner) starts over. Set by the generated
        // factory for a C++ script deriving ScriptBase (null for the GameObject wrapper kind):
        // the object the lifecycle and other script messages are called on, and the messages it
        // may override (ScriptMessages::cppMask; the ones it does not are learned when sent,
        // ScriptBase::inheritedMessages). The lifecycle state of the instance (init ran, enabled
        // latch, started...) is ScriptRuntime's, keyed by `instance`.
        ScriptBase* cppScript = nullptr;
        uint32_t cppMessageMask = 0;
    };

    struct JASPERENGINE_API ScriptComponent {
        std::vector<ScriptEntry> scripts;
    };

}

#endif //SCRIPT_COMPONENT_H
