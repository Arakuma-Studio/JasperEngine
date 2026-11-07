// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S90-procedural-material.md R26)
// - Purpose: the graph windows' entry points for the Project window and the Inspector, so they
//   include no window (AnimatorEditorHooks style). Each shows only once its hook is set.
// - Thread-safety: main thread; set by App when it makes the graph windows.

#pragma once

#include <functional>
#include <string>

namespace jasperengine::editor{

    struct GraphEditorHooks{
        // opens a graph file (any registered kind's) in the window of its kind; the Project
        // window's double-click. False when no window takes it
        std::function<bool(const std::string& path)> openGraph;
        // the selected node of the graph window the Inspector belongs to, drawn instead of the
        // scene selection; true when it drew
        std::function<bool()> drawSelection;
    };

    inline GraphEditorHooks& graphEditorHooks(){
        static GraphEditorHooks hooks;
        return hooks;
    }

}
