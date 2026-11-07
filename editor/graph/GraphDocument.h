// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S90-procedural-material.md R26, R30, D7; S23's graph model)
// - Purpose: the editor's working copy of one graph file of any registered kind (GraphRegistry),
//   shared per path by everything that edits or shows it (the Graph Editor window, a kind's own
//   window, the Inspector's node rows). AnimatorAssetDocument's design: an edit is a batch of
//   graph ops applied to a copy and written as the file's canonical text (graph::save) through
//   the project command history, one GraphEditCmd, one named undo step; the document reads it back,
//   so graph() is what the file holds.
// - No dirty state: as every asset window, an edit is written at once and is its own undo step
//   (docs/undo.md). A gesture that lasts (a slider) folds into one step through the history's
//   interactive edit, never through a save.
// - Changes reach documents three ways, all through filesChanged(): GraphEditCmd's change listener
//   (BF23 F12: the document's own steps, their undo and redo, and edit_graph's), the hot reload's
//   "graph" handler (another program's writes), and checkDiskChanges() for what neither saw. A file
//   that stops parsing keeps the last good graph and reports the error (loadError()).
// - R30: nodes the file gives no position get one from graph::layoutPositions, in memory only
//   (displayPosition), so opening an agent's graph never writes it; the layout op writes them.
// - Thread-safety: main thread.
// - Invariants: one document per normalized absolute path; a commit over a file that changed on
//   disk since it was read is refused, and the document reloads.

#pragma once

#include "graph/Graph.h"
#include "graph/GraphRegistry.h"

#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine::editor{

    class Project;

    class GraphDocument{
    public:
        // the document of a path (absolute, project-relative or asset-relative); null + `error`
        // when no registered kind has its extension, or it does not load
        static std::shared_ptr<GraphDocument> open(Project* project, const std::string& path, std::string* error);
        // whether a registered kind owns this file's extension
        static bool isGraphFile(const std::string& path);
        // reload the documents of these files (listeners notified when they changed)
        static void filesChanged(const std::vector<std::filesystem::path>& paths);
        // a file or directory was renamed or moved in the editor: the documents under it follow
        static void filesMoved(const std::filesystem::path& oldPath, const std::filesystem::path& newPath);
        // every open document whose file differs from what it read reloads
        static void checkDiskChanges();
        static void closeAll();
        // called with the graph files the editor wrote (a step, its undo or redo): the MCP file
        // sync, so they are not reported as outside changes
        static void setFilesWrittenHook(std::function<void(const std::vector<std::filesystem::path>&)> hook);

        const graph::Graph& graph() const { return data; }
        // null when the file's kind is not registered (it still loads, read-only)
        const std::shared_ptr<const graph::GraphKindDesc>& kind() const { return kindDesc; }
        const graph::NodeTypeDesc* nodeType(const std::string& typeId) const;
        const std::filesystem::path& absolutePath() const { return filePath; }
        const std::string& projectPath() const { return relativePath; }
        uint64_t revision() const { return documentRevision; }
        // the last reload's error (the file stopped parsing); empty when it holds graph()
        const std::string& loadError() const { return error; }
        const graph::LoadReport& loadReport() const { return report; }
        // graph::validate on this revision
        const std::vector<graph::Issue>& issues() const { return validation; }
        bool editable() const { return kindDesc != nullptr && error.empty(); }

        // where a node is drawn: its position, or the automatic layout's for a node without one
        graph::Vec2 displayPosition(const graph::Node& node) const;

        // Applies `ops` in order to a copy and writes the result as one undo step named `undoName`
        // (empty: describe() of a single op, else "Edit Graph (N Changes)"). Nothing is written
        // when an op throws (`error` says which), when nothing changed (true), or when the file
        // changed on disk meanwhile (refused; the document reloads). `inverses` gets each op's
        // inverse, so a caller learns the ids addNode allocated.
        bool apply(const std::vector<graph::GraphOp>& ops, const std::string& undoName, std::string* error,
                   std::vector<graph::GraphOp>* inverses = nullptr);
        // reads the file again (listeners notified when it changed)
        void reload();

        int addListener(std::function<void()> listener);
        void removeListener(int id);

    private:
        Project* project = nullptr;
        std::filesystem::path filePath;
        std::string relativePath;
        std::string text;
        std::string error;
        uint64_t documentRevision = 0;
        graph::Graph data;
        graph::LoadReport report;
        std::shared_ptr<const graph::GraphKindDesc> kindDesc;
        std::vector<graph::Issue> validation;
        std::map<std::string, graph::Vec2> laidOut;
        std::map<int, std::function<void()>> listeners;
        int nextListener = 1;

        bool load(const std::string& newText, std::string* error);
        void notify();
    };

}
