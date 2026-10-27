// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (vertex animation spec 2.1, 5.2)
// - Purpose: the editor's working copy of one ".vat" file, shared per path by everything that
//   shows or edits it (the Inspector's .vat asset view, the Vertex Animator section, the baker
//   window, MCP). The AnimationClipDocument design:
//   * every edit goes through commit(): checked (only the editable fields may change: clip name,
//     wrap, speed, interpolate, events, socket names, removing whole clips), normalized, written
//     as the canonical text (vat::writeVat) through the project command history (WriteFilesCmd:
//     one undo step; a non-empty merge key merges slider drags) and read back, so data() is what
//     the file holds. No dirty state.
//   * every new revision (commit, undo, redo, reload) invalidates VertexAnimationPool for the
//     path, so the Vertex Animators playing it (edit preview and play mode) reload; a file that
//     no longer parses keeps the last valid asset for them (and here, with a warning).
//   Undo, redo and outside changes (McpAutoSync, the Project window's 1 s poll) reach documents
//   through filesChanged() / checkDiskChanges().
// - Thread-safety: main thread.
// - Invariants: one document per normalized absolute path; path() is project-relative with
//   '/'; a parse failure on reload keeps the last good data and adds a warning; the GUID of the
//   file is never changed by commit.

#pragma once

#include "animation/VertexAnimation.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine::editor{

    class Project;

    class VatDocument{
    public:
        enum class Change { Edited, Reloaded, Removed };
        using Listener = std::function<void(VatDocument&, Change)>;

        // The document of a project-relative, asset-relative (the component form) or absolute
        // path inside the project, shared per path; loads it on first use. Null + error when the
        // file is missing, outside the project, not a .vat, or does not parse (the parser's
        // message: a Unity/Houdini file is refused with a pointer to the importer).
        static std::shared_ptr<VatDocument> open(Project* project, const std::string& path, std::string* error);
        // the documents open now
        static std::vector<std::shared_ptr<VatDocument>> openDocuments();

        // Files changed on disk (absolute paths: written by a command, its undo/redo, a tool, the
        // baker or another program; a "<x>.vat.glb" stands for its "<x>.vat"; a directory for
        // what is under it): the open documents of them reload, and every changed .vat that
        // parses is dropped from VertexAnimationPool (whether a document has it open or not), so
        // the animators using it in open scenes reload
        static void filesChanged(const std::vector<std::filesystem::path>& paths, Project* project = nullptr);
        // Stats every open document's file and the .vat files (and their meshes) the Vertex
        // Animators of `project`'s open scenes and loaded prefabs use, and goes through
        // filesChanged for the changed ones (the editor's poll)
        static void checkDiskChanges(Project* project = nullptr);
        // A file or directory was renamed or moved by the editor: the documents of it (or under
        // it) follow: path(), absolutePath() and the registry key change, listeners get Reloaded
        static void filesMoved(const std::filesystem::path& oldPath, const std::filesystem::path& newPath);
        // Forgets every open document (the project closed)
        static void closeAll();
        // Called after a document's WriteFilesCmd ran, was undone or redone (the MCP server sets
        // it to its file sync, as AnimationClipDocument::setFilesWrittenHook)
        static void setFilesWrittenHook(std::function<void(const std::vector<std::filesystem::path>&)> hook);

        // The text commit() would write for `data` (normalized, canonical)
        static std::string fileText(const jasperengine::vat::VatData& data);
        // What a commit of `edited` over `current` changes that only a rebake may change ("" when
        // nothing): "mode", "mesh", "layout", "bounds", "stats", "recipe", "clips[2].frames",
        // "clips[3] (a new clip)", "sockets (order or count)"...
        static std::string generatedChange(const jasperengine::vat::VatData& current, const jasperengine::vat::VatData& edited);
        // Sorts each clip's events by (time, function), clamps nothing else; the problems that
        // refuse a commit (empty or duplicate clip / socket names, a non-finite speed or event
        // time) in `error`. False when refused
        static bool normalize(jasperengine::vat::VatData& data, std::string* error);
        // Global frames the clips no longer cover (clips removed since the bake: their rows stay
        // in the payload until the next Rebake)
        static int unusedSamples(const jasperengine::vat::VatData& data);

        const std::string& path() const { return relativePath; }            // project-relative, generic
        const std::filesystem::path& absolutePath() const { return filePath; }
        // the asset-relative form (what a Vertex Animator's `animation` stores)
        std::string assetPath() const;
        const jasperengine::vat::VatData& data() const { return current; }
        // Increases on every change of data() (edit, undo, redo, reload)
        uint64_t revision() const { return rev; }
        // Warnings of the last load (JSON paths) and the reload error kept after a failed reload
        const std::vector<ClipWarning>& loadWarnings() const { return warnings; }
        // False after the file was deleted: commit() fails until it is back
        bool exists() const { return onDisk; }

        // The single edit path. An edit that changes nothing writes nothing (true, no undo step).
        // Refused (false + error) for a generated field change (generatedChange), a refused
        // normalize, a missing file, or a file changed on disk since it was read (reloaded
        // instead: "make the edit again"). An outside change also ends a merge.
        bool commit(const jasperengine::vat::VatData& newData, const std::string& undoName, const std::string& mergeKey,
                    std::string* error);

        // Reads the file again (when its text changed) and notifies the listeners
        void reloadFromDisk();

        int addListener(Listener listener);
        void removeListener(int id);

    private:
        Project* project = nullptr;
        std::string relativePath;
        std::filesystem::path filePath;
        jasperengine::vat::VatData current;
        std::string text;                   // the file text data() was read from
        std::vector<ClipWarning> warnings;
        uint64_t rev = 0;
        bool onDisk = false;
        bool committing = false;
        uint64_t mergeEpoch = 0;
        int64_t stampTime = 0;
        uint64_t stampSize = 0;
        std::map<int, Listener> listeners;
        int nextListener = 1;

        VatDocument() = default;
        enum class ReloadResult { Unchanged, Changed, Kept, Removed, Busy };
        ReloadResult reload();
        ReloadResult reloadContent();
        bool load(std::string* error);
        void notify(Change change);
        void takeStamp();
        bool stampChanged() const;
        void revisionChanged();
        bool writeUndoable(const std::string& newText, const std::string& mergeKey, std::string* error);
        std::string keptGuid() const;
    };

}
