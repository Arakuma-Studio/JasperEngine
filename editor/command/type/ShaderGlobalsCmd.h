// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "command/Command.h"
#include "Project.h"
#include "shader/ShaderGlobals.h"
#include "util/ShaderGlobalsUsage.h"

#include <map>
#include <string>
#include <vector>

namespace jasperengine::editor{

    // Project Settings > Graphics > Shader Globals: the whole list before and after one edit (the
    // dialog's OK, MCP set_project_settings graphics.shader_globals), in the project history.
    // Executing or undoing it declares the list to the engine (the views and a running Play
    // follow at once) and saves project.yaml. Renamed globals (old -> new) also rename the
    // material and component bindings of the open scenes, in the same step.
    class ShaderGlobalsCmd: public Command{

    private:
        Project* project;
        std::vector<ShaderGlobalDecl> before;
        std::vector<ShaderGlobalDecl> after;
        std::map<std::string, std::string> renames;

        struct BindingsEdit {
            ShaderGlobalsUsage::BindingsRef ref;
            ShaderGlobalBindings before;
            ShaderGlobalBindings after;
        };
        std::vector<BindingsEdit> edits;
        bool editsCollected = false;

        bool apply(const std::vector<ShaderGlobalDecl>& decls);

    public:
        ShaderGlobalsCmd(Project* project, std::vector<ShaderGlobalDecl> before, std::vector<ShaderGlobalDecl> after,
                         std::map<std::string, std::string> renames = {});

        bool execute() override;
        void undo() override;

        // the bindings the rename changed (after execute)
        size_t getRenamedBindingCount() const { return edits.size(); }

        // a rename's undo / redo waits for Play to stop (the played scenes' bindings come back
        // from before Play at Stop)
        std::string undoRefusal() const override;
        std::string redoRefusal() const override;

        std::vector<Touched> touchedPaths(bool undo) const override;
        bool affectsStructure() const override { return false; }
    };

}
