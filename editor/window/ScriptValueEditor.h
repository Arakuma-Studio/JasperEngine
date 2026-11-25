// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "script/ScriptProperty.h"

#include <string>

namespace jasperengine::editor {

    class Project;
    struct SceneProject;

    // The editor of one value of a script property or Data Asset field (S95 REQ-20, REQ-21), drawn in
    // the current table cell: a list as its elements' rows with Add (+), Remove (x) and reorder
    // (arrows), a reference as its asset's name, a drop target from the Project window (or the
    // Hierarchy for a GameObject) and a picker of the assets that fit (a Data Asset's: of its
    // class). `value` is edited in place; true on the frame it changed. `editing` is true while a
    // drag or text edit is in progress (the caller merges its changes into one undo step) and goes
    // false when it ends. `scene`: where GameObject references live (null: none can be set)
    struct ScriptValueEdit {
        bool changed = false;
        bool editing = false;
    };
    ScriptValueEdit drawScriptValue(Project* project, SceneProject* scene, const ScriptProperty& field, ScriptPropertyValue& value,
                                    const std::string& id);

}
