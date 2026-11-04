// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// How the editor shows a GameObject: by its name, else (a script made it without one) as
// "GameObject <id>" in the disabled colour, so an unnamed object is never a row with no text
// (project-window spec D12)

#include "Entity.h"

#include <string>

namespace jasperengine::editor {

    inline bool isUnnamedEntity(const std::string& name) {
        return name.empty();
    }

    inline std::string entityDisplayName(const std::string& name, Entity id) {
        return isUnnamedEntity(name) ? "GameObject " + std::to_string(id) : name;
    }

}
