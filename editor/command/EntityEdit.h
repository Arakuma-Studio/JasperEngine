// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "ecs/Entity.h"

#include <cstdint>
#include <string>
#include <vector>

namespace jasperengine::editor{

    class Project;

    // A command that edits values of GameObjects (a property, a transform, a component's
    // values): what a step that groups several of them names ("Set Properties (3 Objects)")
    class EntityEdit{
    public:
        virtual ~EntityEdit() = default;

        // The scene edited, and its GameObjects edited
        virtual Project* editedProject() const = 0;
        virtual uint32_t editedSceneId() const = 0;
        virtual std::vector<Entity> editedEntities() const = 0;
        // What is set, as a step names it: "Intensity", "Transform", "Light"
        virtual std::string editedLabel() const = 0;
    };

}
