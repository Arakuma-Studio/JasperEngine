// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The editor's script templates, one set for every place that makes a script: the Project
// window's Create menu, the Inspector's script dialog (ScriptCreateDialog) and the MCP
// create_script tool

#include "Entity.h"

#include <string>

namespace jasperengine {
    class Scene;
}

namespace jasperengine::editor::ScriptTemplates {

    struct Files {
        std::string header;
        std::string source;
    };

    // a Lua module declared as a ScriptInstance subclass with a field per property
    std::string lua(const std::string& classOrModuleName);
    // a ScriptBase subclass; the source includes headerFileName
    Files cppScript(const std::string& classOrModuleName, const std::string& headerFileName);
    // a subclass of the GameObject's own class (Mesh, Object...)
    Files cppSubclass(Scene* scene, Entity entity, const std::string& classOrModuleName, const std::string& headerFileName);
    // a lone C++ source file: it includes "<name>.h" when that header is beside it
    std::string cppSource(const std::string& name, bool includeHeader);
    // a lone C++ header file
    std::string cppHeader(const std::string& name);

}
