// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef SCRIPT_COMPONENT_H
#define SCRIPT_COMPONENT_H

#include "asset/AssetRef.h"
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
        AssetRef path;           // .cpp or .lua (for Lua: script file path)
        AssetRef headerPath;     // for C++; empty for Lua
        std::string className;   // C++ class or Lua module name (file base name)
        bool enabled = false;
        std::vector<ScriptProperty> properties;

        void* instance = nullptr; // C++ instance OR Lua handle
        // Runtime only: a new number each time a Lua instance is made for the entry (a released
        // registry ref is reused by the next instance, this number is not). With `instance`, the
        // bind stamp of clip tracks on the script's fields (anim-properties P4)
        uint64_t instanceGeneration = 0;

        // Runtime only, never serialized; a copy (EntityCloner) starts over. Set by the generated
        // factory for a C++ script deriving ScriptBase (null for the GameObject wrapper kind):
        // the object the lifecycle and other script messages are called on, and the messages it
        // may override (ScriptMessages::cppMask; the ones it does not are learned when sent,
        // ScriptBase::inheritedMessages). The lifecycle state of the instance (init ran, enabled
        // latch, started...) is ScriptRuntime's, keyed by `instance`.
        ScriptBase* cppScript = nullptr;
        uint64_t cppMessageMask = 0;  // ScriptMessageMask
    };

    struct JASPERENGINE_API ScriptComponent {
        std::vector<ScriptEntry> scripts;
    };

}

#endif //SCRIPT_COMPONENT_H
