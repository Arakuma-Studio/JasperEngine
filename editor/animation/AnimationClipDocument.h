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
//   plumbing around it). Order: everything here reads and writes through AnimationClipIO, which
//   is a stub until T1a lands: T2b codes in parallel with T1a but runs its document tests after
//   T1a is merged (or rebases on it).
// - Thread-safety: main thread.
// - Invariants: one document per normalized path; path() is project-relative with '/'; a parse
//   failure on reload keeps the last good data and adds a warning (with line:col); the GUID of
//   an existing file is never changed by commit (a new file gets one).

#pragma once

#include "animation/AnimationClip.h"
#include "animation/AnimationClipIO.h"
#include "ecs/Entity.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine::editor{

    class Project;

    // One Animation Player entry that references a clip (Inspector "users", MCP `users`, the
    // "changes the clip for all N users in open scenes" texts, Retarget)
    struct ClipUser{
        uint32_t sceneId = 0;           // 0 for a loaded prefab asset that is not open in a stage
        Entity entity = NULL_ENTITY;
        size_t entry = 0;               // index into AnimationPlayerComponent::clips
        bool prefabAsset = false;       // a loaded prefab asset (not a scene instance)
        std::string prefabPath;         // prefabAsset: its project-relative path
    };

    // Every Animation Player entry referencing `clip` (a .anim path in any accepted form, its
    // GUID, or a sub-clip reference) in open scenes, prefab stages and loaded prefab assets.
    // Owner: T2b (with the reference remapping that walks the same places). Main thread.
    // Accepted forms: absolute, project-relative or asset-relative (the component form); a
    // sub-clip "<model>#<name>" matches "<model>#<index>" of the same animation once the model's
    // clips can be listed. An .anim entry matches by GUID first, then by path.
    std::vector<ClipUser> findClipUsers(Project* project, const std::string& clip);

    // A rename or move (RenameFileCmd, CopyFileCmd) rewrote the editor-only hints of an .anim
    // file (spec 9.R26): `preview.model` / `source.path` followed a renamed model, prefab, scene,
    // pose animation or directory. The command keeps the texts to undo it as part of its step.
    struct ClipHintEdit{
        std::filesystem::path file;     // absolute
        std::string before;
        std::string after;
    };
    // Rewrites (canonical text) every project .anim whose preview.model or source.path is
    // `oldPath` or lies under it (both absolute, after the move happened); open documents reload
    std::vector<ClipHintEdit> remapClipHints(Project* project, const std::filesystem::path& oldPath, const std::filesystem::path& newPath);
    // Puts the texts back where the file still holds what remapClipHints wrote
    void restoreClipHints(Project* project, const std::vector<ClipHintEdit>& edits);

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
        // sub-clip documents (a changed .bin: the .gltf files next to it naming it). A path may be a directory (moved or deleted): the documents under
        // it. Every .anim, model or directory path also invalidates AnimationClipPool and marks
        // the Animation Players using it for reload (needsLoad) in `project` (null: the project
        // of the open documents), whether a document has it open or not.
        static void filesChanged(const std::vector<std::filesystem::path>& paths, Project* project = nullptr);
        // Stats every open document's file and reloads the changed ones (the editor's poll); with
        // `project`, also the model files (and a .gltf's .bin buffers) the Animation Players'
        // sub-clips come from: a changed one goes through filesChanged (pool, sub-clip documents,
        // players)
        static void checkDiskChanges(Project* project = nullptr);
        // Forgets every open document (the project closed)
        static void closeAll();
        // Called after a WriteFilesCmd of a document ran, was undone or redone (the MCP server
        // sets it to its file sync: asset database, Project window, McpAutoSync stamps)
        static void setFilesWrittenHook(std::function<void(const std::vector<std::filesystem::path>&)> hook);

        // The text commit() would write for `data` (normalized, canonical)
        static std::string fileText(const ClipData& data);

        const std::string& path() const { return relativePath; }            // project-relative, generic
        // what AnimationClipPool calls take (never path(): the engine has no project root)
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
        // .anim: the file; sub-clip: "<absolute model path>#<clip>" (the pool key form)
        std::filesystem::path filePath;
        // the file whose stamp is watched: the .anim, or the model of a sub-clip
        std::filesystem::path statPath;
        // sub-clip: "<asset-relative model>#<clip>" (anim::buildModelSubClip)
        std::string subClipReference;
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
        bool load(std::string* error);
        void notify(Change change);
        void takeStamp();
        bool stampChanged() const;
        // a new revision (or the file went away): the pool drops it, its players reload
        void revisionChanged();
        bool writeUndoable(const std::string& newText, const std::string& mergeKey, std::string* error);
        // the GUID a commit keeps: the file's, else the asset database's, else the one the
        // database's upgrade gives a file without one
        std::string keptGuid() const;
    };

}
