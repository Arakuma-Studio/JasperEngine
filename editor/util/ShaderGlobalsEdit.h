// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "shader/ShaderGlobals.h"

#include <map>
#include <string>
#include <vector>

namespace jasperengine::editor {

    class Project;

    // An edit of Project Settings > Graphics > Shader Globals (the Project Settings page draws
    // it). Every valid change is declared to the engine at once (preview), so the Scene and Game
    // views follow; cancel() declares the project's list again, commit() makes the whole edit one
    // project history step (ShaderGlobalsCmd: a renamed global takes the bindings of the open
    // scenes along).
    class ShaderGlobalsEdit {
    public:
        struct Row {
            ShaderGlobalDecl decl;
            std::string originalName;   // "" for a row added in this edit
            char nameBuffer[65] = {0};
            bool selected = false;      // in Copy GLSL
        };

        void begin(Project* project);
        std::vector<Row>& rows() { return editRows; }
        const std::vector<Row>& rows() const { return editRows; }
        // a row of a new global (a unique NewGlobal<n>, a white colour)
        void add();

        std::vector<ShaderGlobalDecl> decls() const;
        // old name -> new name of the rows renamed in this edit
        std::map<std::string, std::string> renames() const;
        // the first problem that blocks commit(), "" when there is none: an invalid list, or
        // during Play a rename or a removal of a global bindings use (the played scenes' bindings
        // come back from before Play at Stop, project.yaml would not)
        std::string error() const;

        // the edited list declared to the engine (when valid)
        void preview();
        // false when error() refuses it; true (and nothing added) when nothing changed
        bool commit();
        // the project's list declared again when a preview changed it and nothing was committed
        void cancel();
        bool isPreviewed() const { return previewed; }

    private:
        Project* project = nullptr;
        std::vector<Row> editRows;
        bool previewed = false;
        bool committed = false;
    };

}
