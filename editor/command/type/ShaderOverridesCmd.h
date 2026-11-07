// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "command/Command.h"
#include "Project.h"

namespace jasperengine::editor{

    // Build Settings > Shaders: the keys a build adds or leaves out besides the scenes' own, before
    // and after one edit (the Build Settings window, MCP edit_build_shaders and build_project
    // shaders), in the project history; executing or undoing it applies them and saves project.yaml
    class ShaderOverridesCmd: public Command{

    private:
        Project* project;
        ShaderOverrides before;
        ShaderOverrides after;

        bool apply(const ShaderOverrides& overrides);

    public:
        ShaderOverridesCmd(Project* project, ShaderOverrides before, ShaderOverrides after);

        bool execute() override;
        void undo() override;

        std::vector<Touched> touchedPaths(bool undo) const override;
        bool affectsStructure() const override { return false; }

        std::string describe() const override { return "Change Shader Overrides"; }
        const char* typeName() const override { return "ShaderOverridesCmd"; }
    };

}
