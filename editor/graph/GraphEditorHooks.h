// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S90-procedural-material.md R26)
// - Purpose: the graph windows' entry points for the Project window and the Inspector, so they
//   include no window (AnimatorEditorHooks style). Each shows only once its hook is set.
// - Thread-safety: main thread; set by App when it makes the graph windows.

#pragma once

#include "graph/Graph.h"

#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine::editor{

    class Command;
    class Project;

    struct GraphEditorHooks{
        // opens a graph file (any registered kind's) in the window of its kind; the Project
        // window's double-click. False when no window takes it
        std::function<bool(const std::string& path)> openGraph;
        // the selected node of the graph window the Inspector belongs to, drawn instead of the
        // scene selection; true when it drew
        std::function<bool()> drawSelection;
        // A kind whose own window is not a GraphEditorWindow (the Noise Graph window, T2 R50): its files
        // open there, from openGraph and focus_window alike, never in the Graph Editor
        struct KindWindow{
            std::string windowName;
            std::function<bool(const std::string& path)> open;     // a project-relative or absolute path
        };
        std::map<std::string, KindWindow> kindWindows;              // by kind id
        // What a kind adds to an edit of its files (dialogue-graphs.md R21, R26): commands that
        // join the edit's undo step (a dialogue's translations turned needs_review when a source
        // text changed), and what a file made by edit_graph's create or a Create menu starts with
        struct KindEdits{
            std::function<std::vector<std::unique_ptr<Command>>(Project* project, const std::filesystem::path& file, const std::string& before,
                                                                const std::string& after)> companions;
            std::function<void(Project* project, const std::filesystem::path& file, graph::Graph& created)> created;
        };
        std::map<std::string, KindEdits> kindEdits;                 // by kind id
    };

    inline GraphEditorHooks& graphEditorHooks(){
        static GraphEditorHooks hooks;
        return hooks;
    }

}
