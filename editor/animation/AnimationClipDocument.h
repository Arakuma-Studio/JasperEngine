// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (animation clips spec 4.1, 9.R4/R8/R26)
// - Purpose: the editor's working copy of one clip, shared per path by everything that edits or
//   shows it (Animation window, Inspector asset section, MCP, converters, validation). A clone
//   of PoseAnimationDocument's design:
//   * .anim files: every edit goes through commit(): the data is normalized, written as the
//     canonical text (anim::writeClip) through the project command history (WriteFilesCmd: one
//     undo step; a non-empty merge key merges drags) and read back, so data() is what the file
//     holds. No dirty state.
//   * model sub-clips ("<model>#<clip>"): read-only documents built from the model
//     (anim::buildModelSubClip); commit() fails with a "read-only: Extract to .anim" error.
//   Undo, redo and outside changes (McpAutoSync, the Project window's 1 s poll) reach documents
//   through filesChanged() / checkDiskChanges(), which reload and notify. Every new revision
//   (commit, undo, redo, reload) invalidates AnimationClipPool for the path, so players in open
//   scenes rebind (edit preview and play mode see edits immediately).
// - Owner: T0 (API); T2b implements it (and the Project window, AssetDatabase GUID and auto-sync
//   plumbing around it).
// - Thread-safety: main thread.
// - Invariants: one document per normalized path; path() is project-relative with '/'; a parse
//   failure on reload keeps the last good data and adds a warning (with line:col); the GUID of
//   an existing file is never changed by commit (a new file gets one).

#pragma once

#include "animation/AnimationClip.h"
#include "animation/AnimationClipIO.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine::editor{

    class Project;

    class AnimationClipDocument{
    public:
        enum class Change { Edited, Reloaded, Removed };
        using Listener = std::function<void(AnimationClipDocument&, Change)>;

        // The document of a project-relative (or absolute, inside the project) .anim path or a
        // sub-clip reference, shared per path; loads it on first use. Null + error when the file
        // is missing, outside the project, not a clip (the parse error with line:col) or names
        // no animation of the model.
        static std::shared_ptr<AnimationClipDocument> open(Project* project, const std::string& path, std::string* error);
        // Creates the .anim (fails if it exists) with `initial` (a GUID is generated when it has
        // none) as one project undo step, and opens it
        static std::shared_ptr<AnimationClipDocument> create(Project* project, const std::string& path, const ClipData& initial,
                                                             const std::string& undoName, std::string* error);
        // the documents open now (Inspector users, reference remaps)
        static std::vector<std::shared_ptr<AnimationClipDocument>> openDocuments();

        // Files changed on disk (absolute paths; written by a command, its undo/redo, a tool or
        // another program): the documents of those files reload; a changed model reloads its
        // sub-clip documents
        static void filesChanged(const std::vector<std::filesystem::path>& paths);
        // Stats every open document's file and reloads the changed ones (the editor's poll)
        static void checkDiskChanges();
        // Forgets every open document (the project closed)
        static void closeAll();
        // Called after a WriteFilesCmd of a document ran, was undone or redone (the MCP server
        // sets it to its file sync: asset database, Project window, McpAutoSync stamps)
        static void setFilesWrittenHook(std::function<void(const std::vector<std::filesystem::path>&)> hook);

        // The text commit() would write for `data` (normalized, canonical)
        static std::string fileText(const ClipData& data);

        const std::string& path() const { return relativePath; }            // project-relative, generic
        const std::filesystem::path& absolutePath() const { return filePath; }
        const ClipData& data() const { return current; }
        // the runtime form of data(), built once per revision (preview, sampling, validation)
        std::shared_ptr<const AnimationClip> clip();
        // Increases on every change of data() (edit, undo, redo, reload)
        uint64_t revision() const { return rev; }
        // Warnings of the last load (JSON paths) and the load error kept after a failed reload
        const std::vector<ClipWarning>& loadWarnings() const { return warnings; }
        // A model sub-clip: commit() fails
        bool isReadOnly() const { return readOnly; }
        // False after the file was deleted (an undone creation): commit() fails until it is back
        bool exists() const { return onDisk; }

        // The single edit path. An edit that changes nothing writes nothing (true, no undo step).
        // `warnings` (optional) receives what normalize() changed.
        bool commit(const ClipData& newData, const std::string& undoName, const std::string& mergeKey,
                     std::string* error, std::vector<ClipWarning>* warnings = nullptr);

        // Reads the file again (when its text changed) and notifies the listeners
        void reloadFromDisk();

        int addListener(Listener listener);
        void removeListener(int id);

    private:
        Project* project = nullptr;
        std::string relativePath;
        std::filesystem::path filePath;
        ClipData current;
        std::shared_ptr<const AnimationClip> built;    // clip() of revision builtRevision
        uint64_t builtRevision = 0;
        std::string text;                   // the file text data() was read from
        std::vector<ClipWarning> warnings;
        uint64_t rev = 0;
        bool readOnly = false;
        bool onDisk = false;
        bool committing = false;            // inside commit(): reloads are this document's own edit
        int64_t stampTime = 0;              // file mtime (ns) and size when last read
        uint64_t stampSize = 0;
        std::map<int, Listener> listeners;
        int nextListener = 1;

        AnimationClipDocument() = default;
    };

}
