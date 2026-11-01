// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (anim layers spec 9, 11.4, 11.5)
// - Purpose: the editor's working copy of an Avatar Mask (.mask) or an Animator Controller
//   (.controller), shared per path by everything that edits or shows it (the Avatar Mask asset
//   Inspector, the Animator window, MCP edit_animator / write_file, the Inspector's player
//   section). AnimationClipDocument's design, for these two JSON assets:
//   * every edit goes through commit(): the data is normalized, written as the canonical text
//     (anim::writeAvatarMask / anim::writeController) through the project command history
//     (WriteFilesCmd: one project undo step; a non-empty merge key merges drags) and read back,
//     so data() is what the file holds. No dirty state.
//   * undo, redo and outside changes (McpAutoSync, the Project window's poll) reach documents
//     through filesChanged() / checkDiskChanges(), which reload and notify. Every new revision
//     (commit, undo, redo, reload) invalidates AvatarMaskPool / AnimatorControllerPool for the
//     path and marks the Animation Players using it for reload, so edit preview and play mode see
//     edits at once. AnimationClipDocument::filesChanged / filesMoved / checkDiskChanges /
//     closeAll forward to both document types (one broadcast for the animation assets).
// - Thread-safety: main thread.
// - Invariants: one document per normalized path; path() is project-relative with '/'; a parse
//   failure on reload keeps the last good data and adds a warning (with line:col); the GUID of
//   an existing file is never changed by commit (a new file gets one).

#pragma once

#include "animation/AnimatorController.h"
#include "animation/AvatarMask.h"
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

    // One use of a mask or a controller: an Animation Player of an open scene, prefab stage or
    // loaded prefab asset (`layer` >= 0: that authored layer's mask; -1: the player's
    // controller), or a layer of a project .controller file (`controllerFile` set, a mask use)
    struct AnimAssetUser{
        uint32_t sceneId = 0;               // 0 for a loaded prefab asset that is not open in a stage
        Entity entity = NULL_ENTITY;
        int layer = -1;
        bool prefabAsset = false;
        std::string prefabPath;             // prefabAsset: its project-relative path
        std::string controllerFile;         // a .controller (project-relative) whose layer `layer` uses the mask
    };

    // Every use of a mask (a path in any accepted form: absolute, project- or asset-relative; or
    // its GUID): players' layers (GUID first, then path) and the layers of the project's
    // .controller files (mask_guid first, then mask)
    std::vector<AnimAssetUser> findMaskUsers(Project* project, const std::string& mask);
    // Every Animation Player whose controller is this one (controllerGuid first, then the path)
    std::vector<AnimAssetUser> findControllerUsers(Project* project, const std::string& controller);

    template<class Data>
    class AnimAssetDocument{
    public:
        enum class Change { Edited, Reloaded, Removed };
        using Listener = std::function<void(AnimAssetDocument&, Change)>;

        // The document of a project-relative (or absolute inside the project, or asset-relative)
        // path, shared per path; loads it on first use. Null + error when the file is missing,
        // outside the project, of another type or does not parse (line:col; Unity YAML refused).
        static std::shared_ptr<AnimAssetDocument> open(Project* project, const std::string& path, std::string* error);
        // Creates the file (fails if it exists) with `initial` (a fresh GUID when it has none or
        // one another asset has) as one project undo step, and opens it
        static std::shared_ptr<AnimAssetDocument> create(Project* project, const std::string& path, const Data& initial, std::string* error);
        static std::vector<std::shared_ptr<AnimAssetDocument>> openDocuments();

        // Files or directories changed on disk (absolute): their documents reload; files no
        // document has open still invalidate the pool and reload their players
        static void filesChanged(const std::vector<std::filesystem::path>& paths, Project* project = nullptr);
        // Stats every open document's file and reloads the changed ones (the editor's poll)
        static void checkDiskChanges();
        // A file or directory renamed or moved by the editor: its documents follow
        static void filesMoved(const std::filesystem::path& oldPath, const std::filesystem::path& newPath);
        // The project closed
        static void closeAll();
        // After a document's WriteFilesCmd ran, was undone or redone (the MCP file sync)
        static void setFilesWrittenHook(std::function<void(const std::vector<std::filesystem::path>&)> hook);
        // The canonical text commit() would write for `data`
        static std::string fileText(const Data& data);

        const std::string& path() const { return relativePath; }        // project-relative, generic
        const std::filesystem::path& absolutePath() const { return filePath; }
        const Data& data() const { return current; }
        uint64_t revision() const { return rev; }
        // warnings of the last load (JSON paths), and the parse error kept after a failed reload
        const std::vector<ClipWarning>& loadWarnings() const { return warnings; }
        // false after the file was deleted (an undone creation): commit() fails until it is back
        bool exists() const { return onDisk; }

        // The single edit path (normalize, canonical text, one project undo step). An edit that
        // changes nothing writes nothing (true, no step). A file changed on disk since it was
        // read is reloaded instead and the commit fails ("make the edit again"). Listeners must
        // not commit from inside a notification. `undoName` is for the caller's reports (the
        // history has no step names).
        bool commit(const Data& newData, const std::string& undoName, const std::string& mergeKey, std::string* error,
                    std::vector<ClipWarning>* warnings = nullptr);

        void reloadFromDisk();

        int addListener(Listener listener);
        void removeListener(int id);

    private:
        Project* project = nullptr;
        std::string relativePath;
        std::filesystem::path filePath;
        Data current;
        std::string text;
        std::vector<ClipWarning> warnings;
        uint64_t rev = 0;
        bool onDisk = false;
        bool committing = false;
        uint64_t mergeEpoch = 0;
        int64_t stampTime = 0;
        uint64_t stampSize = 0;
        std::map<int, Listener> listeners;
        int nextListener = 1;

        AnimAssetDocument() = default;
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

        static void documentFilesWritten(Project* project, const std::vector<std::filesystem::path>& changed);
    };

    using AvatarMaskDocument = AnimAssetDocument<anim::AvatarMaskData>;
    using AnimatorControllerDocument = AnimAssetDocument<anim::ControllerData>;

    // The .controller files whose layers' masks or states' clips name `oldPath` (or lie under it:
    // a directory) are rewritten to name `newPath` (canonical text; the GUIDs stay). For the
    // remapClipHints step of RenameFileCmd / CopyFileCmd: returns (file, before, after) texts in
    // the ClipHintEdit form, which restoreClipHints puts back.
    struct AnimAssetHintEdit{
        std::filesystem::path file;
        std::string before;
        std::string after;
    };
    std::vector<AnimAssetHintEdit> remapControllerReferences(Project* project, const std::filesystem::path& oldPath, const std::filesystem::path& newPath);
    // Clips and masks the asset database saw moved (outside the editor too) or come back: the
    // .controller files naming one by its GUID (states' and blend-tree children's clip_guid,
    // layers' mask_guid) are rewritten to its path (the GUID first, anim layers spec 6.3) and
    // their open documents reload. `moved`: GUID -> the file's absolute path. The files rewritten.
    std::vector<std::filesystem::path> relinkControllerGuids(Project* project, const std::map<std::string, std::filesystem::path>& moved);

}
